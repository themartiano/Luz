#include "Scene/Scene.hpp"
#include "Defaults.hpp"
#include "Color.hpp"
#include "Utilities.hpp"
#include "ANSIColors.hpp"
#include "ImageFiles/BMP.hpp"
#include "ImageFiles/TIFF.hpp"
#include "Hittables/BVHNode.hpp"
#include "Hittables/DirectionalLight.hpp"
#include "LightUnits.hpp"
#include "Materials/Emissive.hpp"
#include "VolumeGuidingField.hpp"
#include <limits>
#include <utility>
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <filesystem>

namespace
{
	double	sanitizedLightSelectionWeight(const std::shared_ptr<Hittable>& hittable)
	{
		if (!hittable)
		{
			return (0.0);
		}

		const double weight = hittable->lightSelectionWeight();
		if (!std::isfinite(weight) || weight <= 0.0)
		{
			return (0.0);
		}
		return (weight);
	}

	bool	isSupportedOutputExtension(std::string extension)
	{
		Utilities::toLower(extension);
		return (
			extension == ".bmp"
			|| extension == ".png"
			|| extension == ".tiff"
		);
	}

	void	requireSupportedOutputFileName(const std::string& fileName, const std::string& settingName)
	{
		if (fileName.empty())
		{
			throw std::invalid_argument(settingName + " file name must not be empty.");
		}

		const std::filesystem::path outputPath(fileName);
		const std::string extension = outputPath.extension().string();
		if (!isSupportedOutputExtension(extension))
		{
			throw std::invalid_argument(settingName + " file name must use .bmp, .png, or .tiff.");
		}
	}

	double	validatedEnvironmentCalibrationTarget(double target, const std::string& description)
	{
		if (!std::isfinite(target) || target < 0.0)
		{
			throw std::invalid_argument(description + " must be finite and non-negative.");
		}
		return (target);
	}

	double	environmentCalibrationScale(
		const std::shared_ptr<EnvironmentMap>& environmentMap,
		double currentValue,
		double targetValue,
		const std::string& description
	)
	{
		validatedEnvironmentCalibrationTarget(targetValue, description);
		if (targetValue == 0.0)
		{
			return (0.0);
		}
		if (!environmentMap || environmentMap->empty())
		{
			throw std::invalid_argument(description + " calibration requires an environment map.");
		}
		if (!std::isfinite(currentValue) || currentValue <= 0.0)
		{
			throw std::invalid_argument(description + " calibration requires an environment map with positive luminance.");
		}
		return (targetValue / currentValue);
	}
}

/*
	Constructors & Destructor
*/

// Constructs the Scene with default values
Scene::Scene(void)
{
	this->_image = std::make_unique<Image>(D_WIDTH, D_HEIGHT);

	this->_sampleCount = D_SAMPLE_COUNT;
	this->_adaptiveSampling = D_ADAPTIVE_SAMPLING;
	this->_adaptiveMinSamples = D_ADAPTIVE_MIN_SAMPLES;
	this->_adaptiveBackgroundMinSamples = 0;
	this->_adaptiveVolumeMinSamples = 0;
	this->_adaptiveCheckInterval = D_ADAPTIVE_CHECK_INTERVAL;
	this->_adaptiveThreshold = D_ADAPTIVE_THRESHOLD;
	this->_maxLightBounces = D_MAX_LIGHT_BOUNCES;
	this->_volumeGuidingTrainingSamples = 0;
	this->_volumeGuidingResolution = 16;
	this->_volumeGuidingLobes = 16;
	this->_volumeGuidingAnisotropy = 0.8;
	this->_volumeGuidingStrength = 0.25;
	this->_volumeGuidingStartBounce = 1;
	this->_viewTransform = ViewTransform::AgX;
	this->_exposure = D_EXPOSURE;
	this->_contrast = D_CONTRAST;
	this->_causticsEnabled = D_CAUSTICS;
	this->_causticPhotonCount = D_CAUSTIC_PHOTONS;
	this->_causticPassCount = D_CAUSTIC_PASSES;
	this->_causticRadiusMeters = D_CAUSTIC_RADIUS_METERS;
	this->_causticAlpha = D_CAUSTIC_ALPHA;
	this->_skyline = 0.5;
	this->_renderSky = SKY_ATMOSPHERE;
	this->_distanceBlueness = true;
	this->_metersPerUnit = 1.0;
	this->_atmosphere = Atmosphere();
	this->_atmosphere.setMetersPerUnit(this->_metersPerUnit);
	this->_backgroundColor = Color(0.0, 0.0, 0.0);
	this->_environmentStrength = 1.0;
	this->_environmentLighting = true;
	this->_environmentRotation = 0.0;

	this->_defaultRenderOutputFileName = D_RENDER_FILE_NAME + ".bmp";

	this->_activeCamera = 0;
	this->_lightSelectionTotalWeight = 0.0;

	this->_t_max = std::numeric_limits<double>::max();

	this->_storePixelRenderTimes = false;
	this->_isFromFile = false;
	this->_benchmarkMode = false;
	this->_renderingThreads = CORE_COUNT;
	this->_bloom = true;
	this->_denoise = D_DENOISE;
	this->_denoiseOutputFileName = "";
	this->resetRenderStats();
}

// Properly frees all allocated memory (destructor)
Scene::~Scene(void)
{
}

// Appends 'camera' to the camera vector (list)
void	Scene::addCamera(Camera camera)
{
	this->_cameras.push_back(camera);
}

// Appends 'hittable' to the Hittable vector (list)
void	Scene::addHittable(std::shared_ptr<Hittable> hittable)
{
	this->_hittables.push_back(hittable);
}

// Returns the current Sample Count (rays per pixel)
int	Scene::getSampleCount(void) const
{
	return (this->_sampleCount);
}

// Sets the Sample Count
void	Scene::setSampleCount(const int sampleCount)
{
	if (sampleCount <= 0)
	{
		throw std::invalid_argument("Sample count must be positive.");
	}
	this->_sampleCount = sampleCount;
}

bool	Scene::getAdaptiveSampling(void) const
{
	return (this->_adaptiveSampling);
}

void	Scene::setAdaptiveSampling(bool adaptiveSampling)
{
	this->_adaptiveSampling = adaptiveSampling;
}

int	Scene::getAdaptiveMinSamples(void) const
{
	return (this->_adaptiveMinSamples);
}

void	Scene::setAdaptiveMinSamples(int adaptiveMinSamples)
{
	if (adaptiveMinSamples <= 0)
	{
		throw std::invalid_argument("Adaptive minimum samples must be positive.");
	}
	this->_adaptiveMinSamples = adaptiveMinSamples;
}

int	Scene::getAdaptiveBackgroundMinSamples(void) const
{
	return (this->_adaptiveBackgroundMinSamples);
}

void	Scene::setAdaptiveBackgroundMinSamples(int adaptiveBackgroundMinSamples)
{
	if (adaptiveBackgroundMinSamples < 0)
	{
		throw std::invalid_argument("Adaptive background minimum samples must be non-negative.");
	}
	this->_adaptiveBackgroundMinSamples = adaptiveBackgroundMinSamples;
}

int	Scene::getAdaptiveVolumeMinSamples(void) const
{
	return (this->_adaptiveVolumeMinSamples);
}

void	Scene::setAdaptiveVolumeMinSamples(int adaptiveVolumeMinSamples)
{
	if (adaptiveVolumeMinSamples < 0)
	{
		throw std::invalid_argument("Adaptive volume minimum samples must be non-negative.");
	}
	this->_adaptiveVolumeMinSamples = adaptiveVolumeMinSamples;
}

int	Scene::getAdaptiveCheckInterval(void) const
{
	return (this->_adaptiveCheckInterval);
}

void	Scene::setAdaptiveCheckInterval(int adaptiveCheckInterval)
{
	if (adaptiveCheckInterval <= 0)
	{
		throw std::invalid_argument("Adaptive check interval must be positive.");
	}
	this->_adaptiveCheckInterval = adaptiveCheckInterval;
}

double	Scene::getAdaptiveThreshold(void) const
{
	return (this->_adaptiveThreshold);
}

void	Scene::setAdaptiveThreshold(double adaptiveThreshold)
{
	if (!std::isfinite(adaptiveThreshold) || adaptiveThreshold <= 0.0)
	{
		throw std::invalid_argument("Adaptive threshold must be positive.");
	}
	this->_adaptiveThreshold = adaptiveThreshold;
}

// Returns the current Maximum Bounces of Light
int		Scene::getMaxLightBounces(void) const
{
	return (this->_maxLightBounces);
}

// Sets the Maximum Bounces of Light
void	Scene::setMaxLightBounces(const int maxLightBounces)
{
	if (maxLightBounces < 0)
	{
		throw std::invalid_argument("Maximum light bounces must be non-negative.");
	}
	this->_maxLightBounces = maxLightBounces;
}

ViewTransform	Scene::getViewTransform(void) const
{
	return (this->_viewTransform);
}

void	Scene::setViewTransform(ViewTransform viewTransform)
{
	this->_viewTransform = viewTransform;
}

double	Scene::getExposure(void) const
{
	return (this->_exposure);
}

void	Scene::setExposure(double exposure)
{
	if (!std::isfinite(exposure) || !std::isfinite(std::pow(2.0, exposure)))
	{
		throw std::invalid_argument("Exposure must be finite.");
	}
	this->_exposure = exposure;
}

void	Scene::setPhotographicExposure(double fNumber, double shutterSeconds, double iso)
{
	if (!std::isfinite(fNumber) || fNumber <= 0.0)
	{
		throw std::invalid_argument("Photographic f-number must be finite and positive.");
	}
	if (!std::isfinite(shutterSeconds) || shutterSeconds <= 0.0)
	{
		throw std::invalid_argument("Photographic shutter time must be finite and positive.");
	}
	if (!std::isfinite(iso) || iso <= 0.0)
	{
		throw std::invalid_argument("Photographic ISO must be finite and positive.");
	}

	this->setExposure(std::log2((shutterSeconds * (iso / 100.0)) / (fNumber * fNumber)));
}

int	Scene::getVolumeGuidingTrainingSamples(void) const
{
	return (this->_volumeGuidingTrainingSamples);
}

void	Scene::setVolumeGuidingTrainingSamples(int trainingSamples)
{
	if (trainingSamples < 0)
		throw std::invalid_argument("Volume guiding training samples must be non-negative.");
	this->_volumeGuidingTrainingSamples = trainingSamples;
}

std::uint32_t	Scene::getVolumeGuidingResolution(void) const
{
	return (this->_volumeGuidingResolution);
}

void	Scene::setVolumeGuidingResolution(std::uint32_t resolution)
{
	if (resolution < 1 || resolution > 128)
		throw std::invalid_argument("Volume guiding resolution must be between 1 and 128.");
	this->_volumeGuidingResolution = resolution;
}

std::uint32_t	Scene::getVolumeGuidingLobes(void) const
{
	return (this->_volumeGuidingLobes);
}

void	Scene::setVolumeGuidingLobes(std::uint32_t lobes)
{
	if (lobes < 4 || lobes > 64)
		throw std::invalid_argument("Volume guiding lobes must be between 4 and 64.");
	this->_volumeGuidingLobes = lobes;
}

double	Scene::getVolumeGuidingAnisotropy(void) const
{
	return (this->_volumeGuidingAnisotropy);
}

void	Scene::setVolumeGuidingAnisotropy(double anisotropy)
{
	if (!std::isfinite(anisotropy) || anisotropy < 0.0 || anisotropy > 0.95)
		throw std::invalid_argument("Volume guiding anisotropy must be between 0 and 0.95.");
	this->_volumeGuidingAnisotropy = anisotropy;
}

double	Scene::getVolumeGuidingStrength(void) const
{
	return (this->_volumeGuidingStrength);
}

void	Scene::setVolumeGuidingStrength(double strength)
{
	if (!std::isfinite(strength) || strength < 0.0 || strength > 0.75)
		throw std::invalid_argument("Volume guiding strength must be between 0 and 0.75.");
	this->_volumeGuidingStrength = strength;
}

int	Scene::getVolumeGuidingStartBounce(void) const
{
	return (this->_volumeGuidingStartBounce);
}

void	Scene::setVolumeGuidingStartBounce(int startBounce)
{
	if (startBounce < 0 || startBounce > 64)
		throw std::invalid_argument("Volume guiding start bounce must be between 0 and 64.");
	this->_volumeGuidingStartBounce = startBounce;
}

void	Scene::setVolumeGuidingField(std::shared_ptr<VolumeGuidingField> field)
{
	this->_volumeGuidingField = std::move(field);
}

const std::shared_ptr<VolumeGuidingField>&	Scene::getVolumeGuidingField(void) const
{
	return (this->_volumeGuidingField);
}

double	Scene::getContrast(void) const
{
	return (this->_contrast);
}

void	Scene::setContrast(double contrast)
{
	if (!std::isfinite(contrast) || contrast < 0.0)
	{
		throw std::invalid_argument("Contrast must be non-negative.");
	}
	this->_contrast = contrast;
}

bool	Scene::getCausticsEnabled(void) const
{
	return (this->_causticsEnabled);
}

void	Scene::setCausticsEnabled(bool causticsEnabled)
{
	this->_causticsEnabled = causticsEnabled;
}

int	Scene::getCausticPhotonCount(void) const
{
	return (this->_causticPhotonCount);
}

void	Scene::setCausticPhotonCount(int causticPhotonCount)
{
	if (causticPhotonCount < 0)
	{
		throw std::invalid_argument("Caustic photon count must be non-negative.");
	}
	this->_causticPhotonCount = causticPhotonCount;
}

int	Scene::getCausticPassCount(void) const
{
	return (this->_causticPassCount);
}

void	Scene::setCausticPassCount(int causticPassCount)
{
	if (causticPassCount <= 0)
	{
		throw std::invalid_argument("Caustic pass count must be positive.");
	}
	this->_causticPassCount = causticPassCount;
}

double	Scene::getCausticRadiusMeters(void) const
{
	return (this->_causticRadiusMeters);
}

void	Scene::setCausticRadiusMeters(double causticRadiusMeters)
{
	if (!std::isfinite(causticRadiusMeters) || causticRadiusMeters <= 0.0)
	{
		throw std::invalid_argument("Caustic radius must be finite and positive.");
	}
	this->_causticRadiusMeters = causticRadiusMeters;
}

double	Scene::getCausticAlpha(void) const
{
	return (this->_causticAlpha);
}

void	Scene::setCausticAlpha(double causticAlpha)
{
	if (!std::isfinite(causticAlpha) || causticAlpha <= 0.0 || causticAlpha > 1.0)
	{
		throw std::invalid_argument("Caustic alpha must be finite and in (0,1].");
	}
	this->_causticAlpha = causticAlpha;
}

void	Scene::setCausticPhotonMap(std::shared_ptr<CausticPhotonMap> causticPhotonMap)
{
	this->_causticPhotonMap = causticPhotonMap;
}

const std::shared_ptr<CausticPhotonMap>&	Scene::getCausticPhotonMap(void) const
{
	return (this->_causticPhotonMap);
}

// Returns Skyline (used on sky colors interpolation)
double	Scene::getSkyline(void) const
{
	return (this->_skyline);
}

// Returns the RenderSky option
SkyTypes	Scene::getRenderSky(void) const
{
	return (this->_renderSky);
}

// Sets the RenderSky option
void	Scene::setRenderSky(SkyTypes renderSky)
{
	this->_renderSky = renderSky;
}

bool	Scene::getDistanceBlueness(void) const
{
	return (this->_distanceBlueness);
}

void	Scene::setDistanceBlueness(bool distanceBlueness)
{
	this->_distanceBlueness = distanceBlueness;
}

// Sets the Atmosphere object
void	Scene::setAtmosphere(Atmosphere atmosphere)
{
	atmosphere.setMetersPerUnit(this->_metersPerUnit);
	this->_atmosphere = atmosphere;
}

// Returns the Atmosphere object
const Atmosphere&	Scene::getAtmosphere(void) const
{
	return (this->_atmosphere);
}

double	Scene::getMetersPerUnit(void) const
{
	return (this->_metersPerUnit);
}

void	Scene::setMetersPerUnit(double metersPerUnit)
{
	if (!std::isfinite(metersPerUnit) || metersPerUnit <= 0.0)
	{
		throw std::invalid_argument("Scene meters per unit must be finite and positive.");
	}
	this->_metersPerUnit = metersPerUnit;
	this->_atmosphere.setMetersPerUnit(metersPerUnit);
}

double	Scene::sceneUnitsToMeters(double sceneUnits) const
{
	if (!std::isfinite(sceneUnits))
	{
		return (sceneUnits);
	}
	const double maxSceneUnits = T_MAX / this->_metersPerUnit;
	if (sceneUnits >= maxSceneUnits)
	{
		return (T_MAX);
	}
	if (sceneUnits <= -maxSceneUnits)
	{
		return (-T_MAX);
	}
	return (sceneUnits * this->_metersPerUnit);
}

double	Scene::sceneAreaToSquareMeters(double sceneArea) const
{
	return (sceneArea * this->_metersPerUnit * this->_metersPerUnit);
}

void	Scene::syncAtmosphereSunDirection(void)
{
	for (const std::shared_ptr<Hittable>& hittable : this->_hittables)
	{
		const std::shared_ptr<DirectionalLight> directionalLight = std::dynamic_pointer_cast<DirectionalLight>(hittable);
		if (directionalLight)
		{
			this->_atmosphere.setSunDirection(directionalLight->getDirection() * -1.0);
			if (directionalLight->hasAtmosphereSunRadiance())
			{
				this->_atmosphere.setSunRadiance(directionalLight->getAtmosphereSunRadiance());
			}
			else if (directionalLight->getMaterial())
			{
				this->_atmosphere.setSunRadiance(directionalLight->getMaterial()->emitted());
			}
			return;
		}
	}
}

// Returns the Background Color
Color	Scene::getBackgroundColor(void) const
{
	return (this->_backgroundColor);
}

// Sets the Background Color
void	Scene::setBackgroundColor(Color backgroundColor)
{
	this->_backgroundColor = backgroundColor;
}

void	Scene::setEnvironmentMap(std::shared_ptr<EnvironmentMap> environmentMap)
{
	this->_environmentMap = environmentMap;
}

const std::shared_ptr<EnvironmentMap>&	Scene::getEnvironmentMap(void) const
{
	return (this->_environmentMap);
}

bool	Scene::hasEnvironmentMap(void) const
{
	return (this->_environmentMap != nullptr && !this->_environmentMap->empty());
}

void	Scene::setEnvironmentStrength(double environmentStrength)
{
	if (!std::isfinite(environmentStrength) || environmentStrength < 0.0)
	{
		throw std::invalid_argument("Environment strength must be finite and non-negative.");
	}
	this->_environmentStrength = environmentStrength;
}

double	Scene::getEnvironmentStrength(void) const
{
	return (this->_environmentStrength);
}

void	Scene::setEnvironmentLighting(bool environmentLighting)
{
	this->_environmentLighting = environmentLighting;
}

bool	Scene::getEnvironmentLighting(void) const
{
	return (this->_environmentLighting);
}

void	Scene::calibrateEnvironmentAverageRadiance(double averageRadiance)
{
	this->setEnvironmentStrength(environmentCalibrationScale(
		this->_environmentMap,
		this->_environmentMap ? this->_environmentMap->averageLuminance() : 0.0,
		averageRadiance,
		"Environment average radiance"
	));
}

void	Scene::calibrateEnvironmentAverageLuminance(double averageLuminance)
{
	this->calibrateEnvironmentAverageRadiance(
		validatedEnvironmentCalibrationTarget(averageLuminance, "Environment average luminance")
		/ LightUnits::LUMENS_PER_RADIANT_WATT
	);
}

void	Scene::calibrateEnvironmentHorizontalIrradiance(double horizontalIrradiance)
{
	this->setEnvironmentStrength(environmentCalibrationScale(
		this->_environmentMap,
		this->_environmentMap ? this->_environmentMap->horizontalIrradiance() : 0.0,
		horizontalIrradiance,
		"Environment horizontal irradiance"
	));
}

void	Scene::calibrateEnvironmentHorizontalIlluminance(double horizontalIlluminance)
{
	this->calibrateEnvironmentHorizontalIrradiance(
		validatedEnvironmentCalibrationTarget(horizontalIlluminance, "Environment horizontal illuminance")
		/ LightUnits::LUMENS_PER_RADIANT_WATT
	);
}

void	Scene::setEnvironmentRotation(double environmentRotation)
{
	if (!std::isfinite(environmentRotation))
	{
		throw std::invalid_argument("Environment rotation must be finite.");
	}
	this->_environmentRotation = environmentRotation;
}

double	Scene::getEnvironmentRotation(void) const
{
	return (this->_environmentRotation);
}

// Returns the vector (list) of Hittables
const std::vector<std::shared_ptr<Hittable>>&	Scene::getHittables(void) const
{
	return (this->_hittables);
}

// Returns the currently active camera
Camera	Scene::getActiveCamera(void) const
{
	if (this->_cameras.size() > 0)
	{
		return (this->_cameras[this->_activeCamera]);
	}

	return (Camera()); // No camera should be returned
}

// Returns 'true' if this Scene has at least one Camera
bool	Scene::hasCamera(void) const
{
	return (this->_cameras.size() > 0);
}

// Returns the Output File Name
std::string	Scene::getDefaultRenderOutputFileName(void) const
{
	return (this->_defaultRenderOutputFileName);
}

// Sets the Output File Name
void	Scene::setDefaultRenderOutputFileName(std::string defaultRenderOutputFileName)
{
	requireSupportedOutputFileName(defaultRenderOutputFileName, "Output");
	this->_defaultRenderOutputFileName = defaultRenderOutputFileName;
}

void	Scene::updateLights(void)
{
	std::vector<std::shared_ptr<Hittable>> lights;

	this->_lightSelectionCumulativeWeights.clear();
	this->_lightSelectionTotalWeight = 0.0;
	for (std::shared_ptr<Hittable> hittable : this->_hittables)
	{
		Material* material = hittable ? hittable->getMaterial() : nullptr;

		if (material && material->getType() == EMISSIVE)
		{
			lights.push_back(hittable);
			this->_lightSelectionTotalWeight += sanitizedLightSelectionWeight(hittable);
			this->_lightSelectionCumulativeWeights.push_back(this->_lightSelectionTotalWeight);
		}
	}

	if (this->_lightSelectionTotalWeight <= 0.0)
	{
		this->_lightSelectionCumulativeWeights.clear();
	}
	this->_lights = lights;
}

const std::vector<std::shared_ptr<Hittable>>&	Scene::getLights(void) const
{
	return (this->_lights);
}

const std::vector<double>&	Scene::getLightSelectionCumulativeWeights(void) const
{
	return (this->_lightSelectionCumulativeWeights);
}

double	Scene::getLightSelectionTotalWeight(void) const
{
	return (this->_lightSelectionTotalWeight);
}

void	Scene::updateAccelerationStructure(void)
{
	std::vector<std::shared_ptr<Hittable>> boundedHittables;

	this->_unacceleratedHittables.clear();
	boundedHittables.reserve(this->_hittables.size());

	for (const std::shared_ptr<Hittable>& hittable : this->_hittables)
	{
		AABB boundingBox;

		if (hittable->createBoundingBox(boundingBox))
		{
			boundedHittables.push_back(hittable);
		}
		else
		{
			this->_unacceleratedHittables.push_back(hittable);
		}
	}

	if (boundedHittables.empty())
	{
		this->_accelerationStructure = nullptr;
	}
	else if (boundedHittables.size() == 1)
	{
		this->_accelerationStructure = boundedHittables[0];
	}
	else
	{
		this->_accelerationStructure = std::make_shared<BVHNode>(boundedHittables);
	}
}

const std::shared_ptr<Hittable>&	Scene::getAccelerationStructure(void) const
{
	return (this->_accelerationStructure);
}

const std::vector<std::shared_ptr<Hittable>>&	Scene::getUnacceleratedHittables(void) const
{
	return (this->_unacceleratedHittables);
}

bool	Scene::getStorePixelRenderTimes(void) const
{
	return (this->_storePixelRenderTimes);
}

// Sets storePixelRenderTime
void	Scene::setStorePixelRenderTimes(bool storePixelRenderTimes)
{
	this->_storePixelRenderTimes = storePixelRenderTimes;

	if (this->_storePixelRenderTimes)
	{
		this->_pixelRenderTimes.assign(this->_image->getWidth() * this->_image->getHeight(), 0.0);
	}
	else
	{
		this->_pixelRenderTimes.clear();
	}
}

void	Scene::setPixelRenderTime(std::size_t x, std::size_t y, double renderTime)
{
	if (this->_storePixelRenderTimes)
	{
		const std::size_t totalPixels = this->_image->getWidth() * this->_image->getHeight();
		if (this->_pixelRenderTimes.size() != totalPixels)
		{
			this->_pixelRenderTimes.assign(totalPixels, 0.0);
		}
		std::size_t index = (y * this->_image->getWidth()) + x;

		this->_pixelRenderTimes[index] = renderTime;
	}
}

std::unique_ptr<Image>&	Scene::getImage(void)
{
	return (this->_image);
}

std::unique_ptr<Image>	Scene::generateRenderTimeImage(void) const
{
	if (!this->_storePixelRenderTimes)
	{
		return (std::make_unique<Image>(0, 0));
	}


	auto image = std::make_unique<Image>(this->_image->getWidth(), this->_image->getHeight());
	image->initialize();

	if (this->_pixelRenderTimes.empty())
	{
		return (image);
	}

	const double fastest = *std::min_element(this->_pixelRenderTimes.begin(), this->_pixelRenderTimes.end());
	const double slowest = *std::max_element(this->_pixelRenderTimes.begin(), this->_pixelRenderTimes.end());
	const double range = slowest - fastest;

	for (std::size_t i = 0; i < this->_image->getWidth() * this->_image->getHeight(); i++)
	{
		Color orange(1.0, 0.94, 0.84); // Fastest
		Color purple(0.1, 0.0, 0.18); // Slowest

		const double ratio = range > 0.0 ? (this->_pixelRenderTimes[i] - fastest) / range : 0.0;

		image->setPixel(i % this->_image->getWidth(), i / this->_image->getWidth(), (purple - orange) * ratio + orange);
	}

	return (image);
}

void	Scene::setIsFromFile(bool isFromFile)
{
	this->_isFromFile = isFromFile;
}

bool	Scene::getIsFromFile(void) const
{
	return (this->_isFromFile);
}

void	Scene::setBenchmarkMode(bool benchmarkMode)
{
	this->_benchmarkMode = benchmarkMode;
}

bool	Scene::getBenchmarkMode(void) const
{
	return (this->_benchmarkMode);
}

void	Scene::setRenderingThreads(std::size_t renderingThreads)
{
	if (renderingThreads == 0)
	{
		throw std::invalid_argument("Rendering thread count must be positive.");
	}
	this->_renderingThreads = renderingThreads;
}

std::size_t	Scene::getRenderingThreads(void) const
{
	return (this->_renderingThreads);
}

void	Scene::setBloom(bool bloom)
{
	this->_bloom = bloom;
}

bool	Scene::getBloom(void) const
{
	return (this->_bloom);
}

void	Scene::setDenoise(bool denoise)
{
	this->_denoise = denoise;
}

bool	Scene::getDenoise(void) const
{
	return (this->_denoise);
}

void	Scene::setDenoisedImage(std::unique_ptr<Image> denoisedImage)
{
	this->_denoisedImage = std::move(denoisedImage);
}

std::unique_ptr<Image>&	Scene::getDenoisedImage(void)
{
	return (this->_denoisedImage);
}

const std::unique_ptr<Image>&	Scene::getDenoisedImage(void) const
{
	return (this->_denoisedImage);
}

bool	Scene::hasDenoisedImage(void) const
{
	return (this->_denoisedImage != nullptr);
}

void	Scene::clearDenoisedImage(void)
{
	this->_denoisedImage = nullptr;
}

void	Scene::setDenoiseOutputFileName(std::string denoiseOutputFileName)
{
	requireSupportedOutputFileName(denoiseOutputFileName, "Denoise output");
	this->_denoiseOutputFileName = denoiseOutputFileName;
}

std::string	Scene::getDenoiseOutputFileName(void) const
{
	return (this->_denoiseOutputFileName);
}

void	Scene::initializeDenoiseBuffers(std::size_t width, std::size_t height)
{
	this->_denoiseBuffers = std::make_unique<Denoise::NFORBuffers>();
	this->_denoiseBuffers->initialize(width, height);
}

Denoise::NFORBuffers*	Scene::getDenoiseBuffers(void)
{
	return (this->_denoiseBuffers.get());
}

const Denoise::NFORBuffers*	Scene::getDenoiseBuffers(void) const
{
	return (this->_denoiseBuffers.get());
}

void	Scene::clearDenoiseBuffers(void)
{
	this->_denoiseBuffers = nullptr;
}

void	Scene::resetRenderStats(void)
{
	this->_renderStats = SceneRenderStats();
}

void	Scene::setRenderStats(SceneRenderStats renderStats)
{
	this->_renderStats = renderStats;
}

const SceneRenderStats&	Scene::getRenderStats(void) const
{
	return (this->_renderStats);
}

void Scene::setVolumePrimarySamples(int value)
{
	if (value < 1 || value > 256)
		throw std::invalid_argument("Volume primary samples must be between 1 and 256.");
	_volumePrimarySamples = static_cast<unsigned int>(value);
}

void Scene::setVolumePrimaryMaxSteps(int value)
{
	if (value < 48 || value > 65536)
		throw std::invalid_argument("Volume primary max steps must be between 48 and 65536.");
	_volumePrimaryMaxSteps = value;
}
