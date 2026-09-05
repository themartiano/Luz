#include "SceneFileInternal.hpp"
#include "Utilities.hpp"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	enum class	EnvironmentCalibrationType
	{
		AverageRadiance,
		AverageLuminance,
		HorizontalIrradiance,
		HorizontalIlluminance
	};

	struct	EnvironmentCalibrationSetting
	{
		EnvironmentCalibrationType	type;
		double						value;
	};

	void	requireBinarySetting(int value, const std::string& name)
	{
		if (value != 0 && value != 1)
		{
			throw std::runtime_error(name + " must be 0 or 1.");
		}
	}

	ViewTransform	parseViewTransform(const std::string& value)
	{
		std::string normalized = SceneFile::internal::_lowerCopy(SceneFile::internal::_trim(value));

		if (normalized == "standard")
		{
			return (ViewTransform::Standard);
		}
		if (normalized == "agx")
		{
			return (ViewTransform::AgX);
		}
		if (normalized == "aces")
		{
			return (ViewTransform::ACES);
		}
		if (normalized == "raw")
		{
			return (ViewTransform::Raw);
		}
		throw std::runtime_error("Invalid view_transform setting. Use standard, agx, aces, or raw. Raw is for debugging/HDR data, not display viewing.");
	}

	std::string	settingValue(const std::string& line, const std::string& message)
	{
		const std::size_t separator = line.find('=');

		if (separator == std::string::npos || separator == line.length() - 1)
		{
			throw std::runtime_error(message);
		}
		return (SceneFile::internal::_trim(line.substr(separator + 1)));
	}

	double	parseFiniteDouble(const std::string& value, const std::string& label)
	{
		std::size_t parsed = 0;
		const std::string trimmed = SceneFile::internal::_trim(value);
		const double result = std::stod(trimmed, &parsed);

		if (parsed != trimmed.length() || !std::isfinite(result))
		{
			throw std::runtime_error(label + " must be finite.");
		}
		return (result);
	}

	std::vector<std::string>	splitCommaList(const std::string& value)
	{
		std::vector<std::string> parts;
		std::size_t start = 0;

		while (start <= value.length())
		{
			const std::size_t comma = value.find(',', start);
			const std::size_t end = comma == std::string::npos ? value.length() : comma;

			parts.push_back(SceneFile::internal::_trim(value.substr(start, end - start)));
			if (comma == std::string::npos)
			{
				break;
			}
			start = comma + 1;
		}
		return (parts);
	}

	void	applyPhotographicExposure(Scene& scene, const std::string& value)
	{
		const std::vector<std::string> parts = splitCommaList(value);

		if (parts.size() != 3 || parts[0].empty() || parts[1].empty() || parts[2].empty())
		{
			throw std::runtime_error("Invalid photographic exposure setting. Use photographic_exposure=F_NUMBER,SHUTTER_SECONDS,ISO.");
		}

		scene.setPhotographicExposure(
			parseFiniteDouble(parts[0], "Photographic f-number"),
			parseFiniteDouble(parts[1], "Photographic shutter time"),
			parseFiniteDouble(parts[2], "Photographic ISO")
		);
	}

	void	assignEnvironmentCalibration(
		std::optional<EnvironmentCalibrationSetting>& calibration,
		bool environmentScaleSet,
		EnvironmentCalibrationType type,
		double value
	)
	{
		if (environmentScaleSet)
		{
			throw std::runtime_error("Environment calibration cannot be combined with environment_scale.");
		}
		if (calibration)
		{
			throw std::runtime_error("Environment can define only one physical calibration setting.");
		}
		calibration = EnvironmentCalibrationSetting{type, value};
	}

	void	applyEnvironmentCalibration(Scene& scene, const EnvironmentCalibrationSetting& calibration)
	{
		if (!scene.hasEnvironmentMap())
		{
			throw std::runtime_error("Environment calibration requires environment=PATH.");
		}

		switch (calibration.type)
		{
			case EnvironmentCalibrationType::AverageRadiance:
				scene.calibrateEnvironmentAverageRadiance(calibration.value);
				return;
			case EnvironmentCalibrationType::AverageLuminance:
				scene.calibrateEnvironmentAverageLuminance(calibration.value);
				return;
			case EnvironmentCalibrationType::HorizontalIrradiance:
				scene.calibrateEnvironmentHorizontalIrradiance(calibration.value);
				return;
			case EnvironmentCalibrationType::HorizontalIlluminance:
				scene.calibrateEnvironmentHorizontalIlluminance(calibration.value);
				return;
		}
	}
}

// Parses the [settings] section of a Scene file
void	SceneFile::internal::_readSettingsSection(Scene& scene, std::ifstream& stream, SceneFileContext& context)
{
	std::string line;
	bool skySet = false;
	bool environmentScaleSet = false;
	std::optional<EnvironmentCalibrationSetting> environmentCalibration;
	do
	{
		getline(stream, line);

		if (line.length() <= 0)
		{
			break;
		}
		if (line.at(0) == '#')
		{
			continue;
		}
		std::string lowerLine = line;
		Utilities::toLower(lowerLine);

		if (lowerLine.rfind("resolution=", 0) != std::string::npos)
		{
			long long x, y;

			if (sscanf(lowerLine.c_str(), "resolution=%lld,%lld", &x, &y) != 2)
			{
				throw std::runtime_error("Invalid resolution setting. Use resolution=WIDTH,HEIGHT.");
			}
			if (x <= 0 || y <= 0)
			{
				throw std::runtime_error("Scene resolution dimensions must be positive.");
			}
			scene.getImage()->setWidth(static_cast<std::size_t>(x));
			scene.getImage()->setHeight(static_cast<std::size_t>(y));
			scene.getImage()->initialize();
		}
		else if (lowerLine.rfind("volume_primary_samples=", 0) == 0
			|| lowerLine.rfind("volume_primary_max_steps=", 0) == 0
			|| lowerLine.rfind("volume_reference=", 0) == 0)
		{
			const double parsed = parseFiniteDouble(settingValue(line, "Missing volume setting value."), "Volume setting");
			if (parsed < 0.0 || parsed > 65536.0 || parsed != std::floor(parsed))
				throw std::runtime_error("Volume settings require an integer in range.");
			const int value = static_cast<int>(parsed);
			if (lowerLine.rfind("volume_primary_samples=", 0) == 0)
				scene.setVolumePrimarySamples(value);
			else if (lowerLine.rfind("volume_primary_max_steps=", 0) == 0)
				scene.setVolumePrimaryMaxSteps(value);
			else
			{
				requireBinarySetting(value, "volume_reference");
				scene.setVolumeReference(value != 0);
			}
		}
		else if (lowerLine.rfind("samples=", 0) != std::string::npos)
		{
			int samples;

			if (sscanf(lowerLine.c_str(), "samples=%d", &samples) != 1)
			{
				throw std::runtime_error("Invalid samples setting. Use samples=N.");
			}
			scene.setSampleCount(samples);
		}
		else if (
			lowerLine.rfind("adaptive=", 0) != std::string::npos
			|| lowerLine.rfind("adaptivesampling=", 0) != std::string::npos
			|| lowerLine.rfind("adaptive_sampling=", 0) != std::string::npos
		)
		{
			int adaptiveSampling;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &adaptiveSampling) != 1)
			{
				throw std::runtime_error("Invalid adaptive setting. Use adaptive=0 or adaptive=1.");
			}
			requireBinarySetting(adaptiveSampling, "adaptive");
			scene.setAdaptiveSampling(adaptiveSampling);
		}
		else if (
			lowerLine.rfind("adaptiveminsamples=", 0) != std::string::npos
			|| lowerLine.rfind("adaptive_min_samples=", 0) != std::string::npos
		)
		{
			int adaptiveMinSamples;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &adaptiveMinSamples) != 1)
			{
				throw std::runtime_error("Invalid adaptive minimum samples setting. Use adaptiveminsamples=N.");
			}
			scene.setAdaptiveMinSamples(adaptiveMinSamples);
		}
		else if (
			lowerLine.rfind("adaptivebackgroundminsamples=", 0) != std::string::npos
			|| lowerLine.rfind("adaptive_background_min_samples=", 0) != std::string::npos
		)
		{
			int adaptiveBackgroundMinSamples;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &adaptiveBackgroundMinSamples) != 1)
			{
				throw std::runtime_error(
					"Invalid adaptive background minimum samples setting. "
					"Use adaptivebackgroundminsamples=N."
				);
			}
			scene.setAdaptiveBackgroundMinSamples(adaptiveBackgroundMinSamples);
		}
		else if (
			lowerLine.rfind("adaptivevolumeminsamples=", 0) != std::string::npos
			|| lowerLine.rfind("adaptive_volume_min_samples=", 0) != std::string::npos
		)
		{
			int adaptiveVolumeMinSamples;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &adaptiveVolumeMinSamples) != 1)
			{
				throw std::runtime_error(
					"Invalid adaptive volume minimum samples setting. "
					"Use adaptivevolumeminsamples=N."
				);
			}
			scene.setAdaptiveVolumeMinSamples(adaptiveVolumeMinSamples);
		}
		else if (
			lowerLine.rfind("adaptivethreshold=", 0) != std::string::npos
			|| lowerLine.rfind("adaptive_threshold=", 0) != std::string::npos
		)
		{
			double adaptiveThreshold;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%lf", &adaptiveThreshold) != 1)
			{
				throw std::runtime_error("Invalid adaptive threshold setting. Use adaptivethreshold=F.");
			}
			scene.setAdaptiveThreshold(adaptiveThreshold);
		}
		else if (
			lowerLine.rfind("adaptivecheckinterval=", 0) != std::string::npos
			|| lowerLine.rfind("adaptive_check_interval=", 0) != std::string::npos
		)
		{
			int adaptiveCheckInterval;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &adaptiveCheckInterval) != 1)
			{
				throw std::runtime_error("Invalid adaptive check interval setting. Use adaptivecheckinterval=N.");
			}
			scene.setAdaptiveCheckInterval(adaptiveCheckInterval);
		}
		else if (lowerLine.rfind("maxlightbounces=", 0) != std::string::npos)
		{
			int maxLightBounces;

			if (sscanf(lowerLine.c_str(), "maxlightbounces=%d", &maxLightBounces) != 1)
			{
				throw std::runtime_error("Invalid maxlightbounces setting. Use maxlightbounces=N.");
			}
			scene.setMaxLightBounces(maxLightBounces);
		}
		else if (
			lowerLine.rfind("volume_guiding_samples=", 0) != std::string::npos
			|| lowerLine.rfind("volumeguidingsamples=", 0) != std::string::npos
		)
		{
			int samples;
			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &samples) != 1)
				throw std::runtime_error("Invalid volume guiding samples setting. Use volume_guiding_samples=N.");
			scene.setVolumeGuidingTrainingSamples(samples);
		}
		else if (
			lowerLine.rfind("volume_guiding_resolution=", 0) != std::string::npos
			|| lowerLine.rfind("volumeguidingresolution=", 0) != std::string::npos
		)
		{
			unsigned int resolution;
			if (sscanf(lowerLine.c_str(), "%*[^=]=%u", &resolution) != 1)
				throw std::runtime_error("Invalid volume guiding resolution setting. Use volume_guiding_resolution=N.");
			scene.setVolumeGuidingResolution(resolution);
		}
		else if (
			lowerLine.rfind("volume_guiding_lobes=", 0) != std::string::npos
			|| lowerLine.rfind("volumeguidinglobes=", 0) != std::string::npos
		)
		{
			unsigned int lobes;
			if (sscanf(lowerLine.c_str(), "%*[^=]=%u", &lobes) != 1)
				throw std::runtime_error("Invalid volume guiding lobes setting. Use volume_guiding_lobes=N.");
			scene.setVolumeGuidingLobes(lobes);
		}
		else if (
			lowerLine.rfind("volume_guiding_anisotropy=", 0) != std::string::npos
			|| lowerLine.rfind("volumeguidinganisotropy=", 0) != std::string::npos
		)
		{
			double anisotropy;
			if (sscanf(lowerLine.c_str(), "%*[^=]=%lf", &anisotropy) != 1)
				throw std::runtime_error("Invalid volume guiding anisotropy setting. Use volume_guiding_anisotropy=F.");
			scene.setVolumeGuidingAnisotropy(anisotropy);
		}
		else if (
			lowerLine.rfind("volume_guiding_strength=", 0) != std::string::npos
			|| lowerLine.rfind("volumeguidingstrength=", 0) != std::string::npos
		)
		{
			double strength;
			if (sscanf(lowerLine.c_str(), "%*[^=]=%lf", &strength) != 1)
				throw std::runtime_error("Invalid volume guiding strength setting. Use volume_guiding_strength=F.");
			scene.setVolumeGuidingStrength(strength);
		}
		else if (
			lowerLine.rfind("volume_guiding_start_bounce=", 0) != std::string::npos
			|| lowerLine.rfind("volumeguidingstartbounce=", 0) != std::string::npos
		)
		{
			int startBounce;
			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &startBounce) != 1)
				throw std::runtime_error("Invalid volume guiding start bounce setting. Use volume_guiding_start_bounce=N.");
			scene.setVolumeGuidingStartBounce(startBounce);
		}
		else if (lowerLine.rfind("view_transform=", 0) != std::string::npos)
		{
			scene.setViewTransform(parseViewTransform(settingValue(
				line,
				"Invalid view_transform setting. Use view_transform=standard, agx, aces, or raw."
			)));
		}
		else if (lowerLine.rfind("bloom=", 0) != std::string::npos)
		{
			int useBloom;

			if (sscanf(lowerLine.c_str(), "bloom=%d", &useBloom) != 1)
			{
				throw std::runtime_error("Invalid bloom setting. Use bloom=0 or bloom=1.");
			}
			requireBinarySetting(useBloom, "bloom");
			scene.setBloom(useBloom);
		}
		else if (lowerLine.rfind("exposure=", 0) != std::string::npos)
		{
			double exposure;

			if (sscanf(lowerLine.c_str(), "exposure=%lf", &exposure) != 1)
			{
				throw std::runtime_error("Invalid exposure setting. Use exposure=F.");
			}
			scene.setExposure(exposure);
		}
		else if (
			lowerLine.rfind("photographic_exposure=", 0) != std::string::npos
			|| lowerLine.rfind("photographicexposure=", 0) != std::string::npos
			|| lowerLine.rfind("camera_exposure=", 0) != std::string::npos
			|| lowerLine.rfind("cameraexposure=", 0) != std::string::npos
		)
		{
			applyPhotographicExposure(
				scene,
				settingValue(line, "Invalid photographic exposure setting. Use photographic_exposure=F_NUMBER,SHUTTER_SECONDS,ISO.")
			);
		}
		else if (lowerLine.rfind("contrast=", 0) != std::string::npos)
		{
			double contrast;

			if (sscanf(lowerLine.c_str(), "contrast=%lf", &contrast) != 1)
			{
				throw std::runtime_error("Invalid contrast setting. Use contrast=F.");
			}
			scene.setContrast(contrast);
		}
		else if (lowerLine.rfind("caustics=", 0) != std::string::npos)
		{
			int causticsEnabled;

			if (sscanf(lowerLine.c_str(), "caustics=%d", &causticsEnabled) != 1)
			{
				throw std::runtime_error("Invalid caustics setting. Use caustics=0 or caustics=1.");
			}
			requireBinarySetting(causticsEnabled, "caustics");
			scene.setCausticsEnabled(causticsEnabled);
		}
		else if (
			lowerLine.rfind("caustic_photons=", 0) != std::string::npos
			|| lowerLine.rfind("causticphotons=", 0) != std::string::npos
		)
		{
			int causticPhotons;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &causticPhotons) != 1)
			{
				throw std::runtime_error("Invalid caustic photon setting. Use caustic_photons=N.");
			}
			scene.setCausticPhotonCount(causticPhotons);
		}
		else if (
			lowerLine.rfind("caustic_passes=", 0) != std::string::npos
			|| lowerLine.rfind("causticpasses=", 0) != std::string::npos
		)
		{
			int causticPasses;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &causticPasses) != 1)
			{
				throw std::runtime_error("Invalid caustic pass setting. Use caustic_passes=N.");
			}
			scene.setCausticPassCount(causticPasses);
		}
		else if (
			lowerLine.rfind("caustic_radius=", 0) != std::string::npos
			|| lowerLine.rfind("causticradius=", 0) != std::string::npos
		)
		{
			scene.setCausticRadiusMeters(parseFiniteDouble(
				settingValue(line, "Invalid caustic radius setting. Use caustic_radius=METERS."),
				"Caustic radius"
			));
		}
		else if (
			lowerLine.rfind("caustic_alpha=", 0) != std::string::npos
			|| lowerLine.rfind("causticalpha=", 0) != std::string::npos
		)
		{
			scene.setCausticAlpha(parseFiniteDouble(
				settingValue(line, "Invalid caustic alpha setting. Use caustic_alpha=F."),
				"Caustic alpha"
			));
		}
		else if (lowerLine.rfind("denoise=", 0) != std::string::npos)
		{
			int useDenoise;

			if (sscanf(lowerLine.c_str(), "denoise=%d", &useDenoise) != 1)
			{
				throw std::runtime_error("Invalid denoise setting. Use denoise=0 or denoise=1.");
			}
			requireBinarySetting(useDenoise, "denoise");
			scene.setDenoise(useDenoise);
		}
		else if (
			lowerLine.rfind("denoiseoutputfilename=", 0) != std::string::npos
			|| lowerLine.rfind("denoiseoutput=", 0) != std::string::npos
			|| lowerLine.rfind("denoise_output=", 0) != std::string::npos
		)
		{
			const std::size_t separator = line.find('=');

			if (separator == std::string::npos || separator == line.length() - 1)
			{
				throw std::runtime_error("Invalid denoise output setting. Use denoiseoutputfilename=PATH.");
			}
			scene.setDenoiseOutputFileName(line.substr(separator + 1));
		}
		else if (lowerLine.rfind("outputfilename=", 0) != std::string::npos)
		{
			std::string strOutputFileName = line.substr(std::string("outputfilename=").size());
			if (strOutputFileName.empty())
			{
				throw std::runtime_error("Invalid outputfilename setting. Use outputfilename=PATH.");
			}
			std::filesystem::path outputPath(strOutputFileName);
			std::string lowerExtension = outputPath.extension().string();
			Utilities::toLower(lowerExtension);
			if (lowerExtension.empty())
			{
				strOutputFileName += ".bmp";
			}
			else if (
				lowerExtension != ".bmp"
				&& lowerExtension != ".png"
				&& lowerExtension != ".tiff"
			)
			{
				throw std::runtime_error("Invalid outputfilename setting. Use a .bmp, .png, or .tiff path.");
			}
			scene.setDefaultRenderOutputFileName(strOutputFileName);
		}
		else if (
			lowerLine.rfind("meters_per_unit=", 0) != std::string::npos
			|| lowerLine.rfind("metersperunit=", 0) != std::string::npos
		)
		{
			scene.setMetersPerUnit(parseFiniteDouble(
				settingValue(line, "Invalid meters_per_unit setting. Use meters_per_unit=F."),
				"Meters per unit"
			));
		}
		else if (lowerLine.rfind("sky=", 0) != std::string::npos)
		{
			char sky[256];

			if (sscanf(lowerLine.c_str(), "sky=%255s", sky) != 1)
			{
				throw std::runtime_error("Invalid sky setting. Use sky=none, sky=linear, sky=atmosphere, or sky=environment.");
			}
			std::string skyStr(sky);
			Utilities::toLower(skyStr);

			if (skyStr == "atmosphere")
			{
				scene.setRenderSky(SKY_ATMOSPHERE);
			}
			else if (skyStr == "linear")
			{
				scene.setRenderSky(SKY_LINEAR);
			}
			else if (skyStr == "none")
			{
				scene.setRenderSky(SKY_NONE);
			}
			else if (skyStr == "environment")
			{
				scene.setRenderSky(SKY_ENVIRONMENT);
			}
			else
			{
				throw std::runtime_error("Unknown sky setting: " + line);
			}
			skySet = true;
		}
		else if (
			lowerLine.rfind("environment=", 0) != std::string::npos
			|| lowerLine.rfind("environmentmap=", 0) != std::string::npos
			|| lowerLine.rfind("environment_map=", 0) != std::string::npos
			|| lowerLine.rfind("backgroundimage=", 0) != std::string::npos
			|| lowerLine.rfind("background_image=", 0) != std::string::npos
		)
		{
			const std::vector<std::string> parts = splitCommaList(settingValue(
				line,
				"Invalid environment setting. Use environment=PATH[,ROTATION_DEGREES]."
			));

			if (parts.empty() || parts[0].empty() || parts.size() > 2)
			{
				throw std::runtime_error("Invalid environment setting. Use environment=PATH[,ROTATION_DEGREES].");
			}
			scene.setEnvironmentMap(std::make_shared<EnvironmentMap>(
				EnvironmentMap::load(_resolveAssetPath(context.baseDirectory, parts[0]))
			));
			if (parts.size() >= 2 && !parts[1].empty())
			{
				scene.setEnvironmentRotation(parseFiniteDouble(parts[1], "Environment rotation"));
			}
			if (!skySet)
			{
				scene.setRenderSky(SKY_ENVIRONMENT);
			}
		}
		else if (
			lowerLine.rfind("environment_scale=", 0) != std::string::npos
			|| lowerLine.rfind("environmentscale=", 0) != std::string::npos
		)
		{
			if (environmentCalibration)
			{
				throw std::runtime_error("environment_scale cannot be combined with environment physical calibration.");
			}
			scene.setEnvironmentStrength(parseFiniteDouble(
				settingValue(line, "Invalid environment scale setting. Use environment_scale=F."),
				"Environment scale"
			));
			environmentScaleSet = true;
		}
		else if (
			lowerLine.rfind("environment_lighting=", 0) != std::string::npos
			|| lowerLine.rfind("environmentlighting=", 0) != std::string::npos
		)
		{
			int environmentLighting;

			if (sscanf(lowerLine.c_str(), "%*[^=]=%d", &environmentLighting) != 1)
			{
				throw std::runtime_error("Invalid environment_lighting setting. Use environment_lighting=0 or 1.");
			}
			requireBinarySetting(environmentLighting, "environment_lighting");
			scene.setEnvironmentLighting(environmentLighting);
		}
		else if (
			lowerLine.rfind("environment_average_radiance=", 0) != std::string::npos
			|| lowerLine.rfind("environmentaverageradiance=", 0) != std::string::npos
			|| lowerLine.rfind("environment_radiance=", 0) != std::string::npos
			|| lowerLine.rfind("environmentradiance=", 0) != std::string::npos
		)
		{
			assignEnvironmentCalibration(
				environmentCalibration,
				environmentScaleSet,
				EnvironmentCalibrationType::AverageRadiance,
				parseFiniteDouble(settingValue(line, "Invalid environment radiance setting. Use environment_radiance=F."), "Environment radiance")
			);
		}
		else if (
			lowerLine.rfind("environment_average_luminance=", 0) != std::string::npos
			|| lowerLine.rfind("environmentaverageluminance=", 0) != std::string::npos
			|| lowerLine.rfind("environment_luminance=", 0) != std::string::npos
			|| lowerLine.rfind("environmentluminance=", 0) != std::string::npos
		)
		{
			assignEnvironmentCalibration(
				environmentCalibration,
				environmentScaleSet,
				EnvironmentCalibrationType::AverageLuminance,
				parseFiniteDouble(settingValue(line, "Invalid environment luminance setting. Use environment_luminance=F."), "Environment luminance")
			);
		}
		else if (
			lowerLine.rfind("environment_horizontal_irradiance=", 0) != std::string::npos
			|| lowerLine.rfind("environmenthorizontalirradiance=", 0) != std::string::npos
			|| lowerLine.rfind("environment_irradiance=", 0) != std::string::npos
			|| lowerLine.rfind("environmentirradiance=", 0) != std::string::npos
		)
		{
			assignEnvironmentCalibration(
				environmentCalibration,
				environmentScaleSet,
				EnvironmentCalibrationType::HorizontalIrradiance,
				parseFiniteDouble(settingValue(line, "Invalid environment irradiance setting. Use environment_irradiance=F."), "Environment irradiance")
			);
		}
		else if (
			lowerLine.rfind("environment_horizontal_illuminance=", 0) != std::string::npos
			|| lowerLine.rfind("environmenthorizontalilluminance=", 0) != std::string::npos
			|| lowerLine.rfind("environment_illuminance=", 0) != std::string::npos
			|| lowerLine.rfind("environmentilluminance=", 0) != std::string::npos
		)
		{
			assignEnvironmentCalibration(
				environmentCalibration,
				environmentScaleSet,
				EnvironmentCalibrationType::HorizontalIlluminance,
				parseFiniteDouble(settingValue(line, "Invalid environment illuminance setting. Use environment_illuminance=F."), "Environment illuminance")
			);
		}
		else if (
			lowerLine.rfind("environmentrotation=", 0) != std::string::npos
			|| lowerLine.rfind("environment_rotation=", 0) != std::string::npos
			|| lowerLine.rfind("worldrotation=", 0) != std::string::npos
			|| lowerLine.rfind("world_rotation=", 0) != std::string::npos
		)
		{
			scene.setEnvironmentRotation(parseFiniteDouble(
				settingValue(line, "Invalid environment rotation setting. Use environmentrotation=DEGREES."),
				"Environment rotation"
			));
		}
		else if (
			lowerLine.rfind("background=", 0) != std::string::npos
			|| lowerLine.rfind("backgroundcolor=", 0) != std::string::npos
			|| lowerLine.rfind("background_color=", 0) != std::string::npos
		)
		{
			const std::size_t separator = line.find('=');

			if (separator == std::string::npos || separator == line.length() - 1)
			{
				throw std::runtime_error("Invalid background setting. Use background=(R,G,B).");
			}
			scene.setBackgroundColor(_parseColorValue(line.substr(separator + 1), "background", context));
		}
		else if (lowerLine.rfind("atmosphere=", 0) != std::string::npos)
		{
			double sunAngle, earthRadius, atmosphereRadius, hR, hM, starsBrightness;
			int samples, lightSamples;

			if (scene.getRenderSky() != SKY_ATMOSPHERE)
			{
				throw std::runtime_error("Atmosphere settings require sky=atmosphere.");
			}
			if (sscanf(lowerLine.c_str(), "atmosphere=%lf,%lf,%lf,%lf,%lf,%d,%d,%lf", &sunAngle, &earthRadius, &atmosphereRadius, &hR, &hM, &samples, &lightSamples, &starsBrightness) != 8)
			{
				throw std::runtime_error("Invalid atmosphere setting. Use atmosphere=SUN,EARTH_RADIUS,ATMOSPHERE_RADIUS,HR,HM,SAMPLES,LIGHT_SAMPLES,STARS.");
			}
			Atmosphere atmosphere(sunAngle, earthRadius, atmosphereRadius, hR, hM, samples, lightSamples, starsBrightness);
			atmosphere.setSunRadianceScale(scene.getAtmosphere().getSunRadianceScale());
			scene.setAtmosphere(atmosphere);
		}
		else if (
			lowerLine.rfind("atmosphere_sun_scale=", 0) != std::string::npos
			|| lowerLine.rfind("atmospheresunscale=", 0) != std::string::npos
			|| lowerLine.rfind("atmosphere_sun_multiplier=", 0) != std::string::npos
			|| lowerLine.rfind("atmospheresunmultiplier=", 0) != std::string::npos
		)
		{
			const double atmosphereSunScale = parseFiniteDouble(
				settingValue(line, "Invalid atmosphere sun scale setting. Use atmosphere_sun_scale=F."),
				"Atmosphere sun scale"
			);
			Atmosphere atmosphere = scene.getAtmosphere();

			atmosphere.setSunRadianceScale(atmosphereSunScale);
			scene.setAtmosphere(atmosphere);
		}
		else if (lowerLine.rfind("distanceblueness=", 0) != std::string::npos)
		{
			int distanceBlueness;

			if (sscanf(lowerLine.c_str(), "distanceblueness=%d", &distanceBlueness) != 1)
			{
				throw std::runtime_error("Invalid distanceblueness setting. Use distanceblueness=0 or distanceblueness=1.");
			}
			requireBinarySetting(distanceBlueness, "distanceblueness");
			scene.setDistanceBlueness(distanceBlueness);
		}
		else
		{
			throw std::runtime_error("Unknown scene setting: " + line);
		}
	} while (!stream.eof());
	if (environmentCalibration)
	{
		applyEnvironmentCalibration(scene, *environmentCalibration);
	}
}
