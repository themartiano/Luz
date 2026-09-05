#include "Scene/Scene.hpp"
#include "Renderer/Renderer.hpp"
#include "ANSIColors.hpp"
#include "SceneFile/SceneFile.hpp"
#include "OBJReader.hpp"
#include "Hittables/Triangle.hpp"
#include "Hittables/PerlinSphere.hpp"
#include "Hittables/Plane.hpp"
#include "Hittables/Sphere.hpp"
#include "Hittables/Mesh.hpp"
#include "Hittables/Rectangle.hpp"
#include "Hittables/Cube.hpp"
#include "Hittables/ConstantVolume.hpp"
#include "Utilities.hpp"
#include "Defaults.hpp"
#include "Materials/Lambertian.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Dielectric.hpp"
#include "Materials/Metal.hpp"
#include "Materials/Isotropic.hpp"
#include "Noise/Perlin.hpp"
#include "Image.hpp"
#include "Charts/Bar.hpp"
#include "FlagsParser.hpp"
#include "Scene/SceneHelpers.hpp"
#include "Random.hpp"
#include "Clock.hpp"
#include <filesystem>
#include <memory>
#include <exception>
#include <stdexcept>
#include <string>

static bool	isTiffOutput(std::string outputFileName)
{
	std::string lowerOutputFileName = outputFileName;

	Utilities::toLower(lowerOutputFileName);
	return (
		Utilities::stringEndsWith(lowerOutputFileName, ".tiff")
	);
}

static bool	isPngOutput(std::string outputFileName)
{
	std::string lowerOutputFileName = outputFileName;

	Utilities::toLower(lowerOutputFileName);
	return (Utilities::stringEndsWith(lowerOutputFileName, ".png"));
}

static bool	isBmpOutput(std::string outputFileName)
{
	std::string lowerOutputFileName = outputFileName;

	Utilities::toLower(lowerOutputFileName);
	return (Utilities::stringEndsWith(lowerOutputFileName, ".bmp"));
}

static void	saveImage(const std::unique_ptr<Image>& image, const std::string& outputFileName)
{
	if (isTiffOutput(outputFileName))
	{
		image->saveToTIFF(outputFileName);
	}
	else if (isPngOutput(outputFileName))
	{
		if (image->getColorEncoding() == ImageColorEncoding::SceneLinearACEScg)
		{
			throw std::runtime_error("Raw scene-linear output is for debugging/HDR data and requires .tiff, not .png.");
		}
		image->saveToPNG(outputFileName);
	}
	else if (isBmpOutput(outputFileName))
	{
		if (image->getColorEncoding() == ImageColorEncoding::SceneLinearACEScg)
		{
			throw std::runtime_error("Raw scene-linear output is for debugging/HDR data and requires .tiff, not .bmp.");
		}
		image->saveToBMP(outputFileName);
	}
	else
	{
		throw std::runtime_error("Output path must use .bmp, .png, or .tiff.");
	}
}

static std::string	denoisedOutputFileName(Scene& scene)
{
	const std::string explicitDenoiseOutput = scene.getDenoiseOutputFileName();
	if (!explicitDenoiseOutput.empty())
	{
		return (explicitDenoiseOutput);
	}

	const std::filesystem::path outputPath(scene.getDefaultRenderOutputFileName());
	const std::filesystem::path extension = outputPath.extension();
	std::string lowerExtension = extension.string();

	Utilities::toLower(lowerExtension);
	if (
		lowerExtension == ".bmp"
		|| lowerExtension == ".png"
		|| lowerExtension == ".tiff"
	)
	{
		std::filesystem::path denoisedPath = outputPath;

		denoisedPath.replace_filename(outputPath.stem().string() + "_denoised" + extension.string());
		return (denoisedPath.string());
	}

	std::filesystem::path denoisedPath = outputPath;
	denoisedPath.replace_filename(outputPath.filename().string() + "_denoised");
	return (denoisedPath.string());
}

// Main function
int	main(int argc, char *argv[])
{
	try
	{
		Scene scene;

		FlagsParser(argc, argv).parse(scene);
		if (!scene.getIsFromFile())
		{
			Clock sceneBuildClock;

			sceneBuildClock.start();
			scene.getImage()->setWidth(500);
			scene.getImage()->setHeight(500);
			scene.getImage()->initialize();
			scene.setSampleCount(2000);
			scene.setMaxLightBounces(12);
			scene.setRenderSky(SKY_NONE);
			scene.setDistanceBlueness(false);
			// scene.setAtmosphere(Atmosphere(0.28, D_EARTH_RADIUS, D_ATMOSPHERE_RADIUS, D_HR, D_HM, 16, 8, 0.468)); // Only needed if Scene.Sky == SKY_ATMOSPHERE
			scene.setBackgroundColor(Color(0.0, 0.0, 0.0)); // Only needed if Scene.Sky == SKY_NONE

			// Coordinate system ~~ Right Hand ~~ Forward: -Z | Up: +Y | Right: +X

			Camera camera(
				Vector3(-4.5, 1.0, 4.5),
				Vector3(0.5, 0.0, -0.5),
				0.050397,
				D_CAMERA_SENSOR_WIDTH_METERS,
				D_CAMERA_SENSOR_HEIGHT_METERS,
				D_CAMERA_F_NUMBER,
				20.0
			);
			camera.setPinhole(true);
			scene.addCamera(camera);

			scene.addHittable(std::make_shared<Plane>(
				0.0,
				Vector3(0.0, 1.0, 0.0),
				std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0))
			));

			scene.addHittable(std::make_shared<Sphere>(
				Vector3(0.0, 1.0, 0.0),
				1.0,
				std::make_shared<Dielectric>(Color(1.0, 1.0, 2.0))//Color(0.42, 0.42, 1.0))
			));

			scene.addHittable(std::make_shared<Rectangle>(
				Transform(Vector3(-15.0, 2.5, 0.0), Vector3(1.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0)),
				10.0,
				5.0,
				std::make_shared<Emissive>(Color(1.0, 1.0, 1.0) * 10.0)
			));

			// scene.addHittable(std::make_shared<Rectangle>(
			// 	Transform(Vector3(-10.0, 2.5, -5.0), Vector3(0.0, 0.0, 1.0), Vector3(1.0, 0.0, 0.0)),
			// 	10.0,
			// 	5.0,
			// 	std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0))
			// ));

			// scene.addHittable(std::make_shared<Rectangle>(
			// 	Transform(Vector3(-10.0, 2.5, 5.0), Vector3(0.0, 0.0, -1.0), Vector3(1.0, 0.0, 0.0)),
			// 	10.0,
			// 	5.0,
			// 	std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0))
			// ));

			// scene.addHittable(std::make_shared<Rectangle>(
			// 	Transform(Vector3(-10.0, 5.0, 0.0), Vector3(0.0, -1.0, 0.0), Vector3(1.0, 0.0, 0.0)),
			// 	10.0,
			// 	10.0,
			// 	std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0))
			// ));
			SceneRenderStats stats = scene.getRenderStats();
			stats.sceneBuildMS += sceneBuildClock.elapsedMS();
			scene.setRenderStats(stats);
		}

		if (Renderer::render(scene))
		{
			if (!scene.getBenchmarkMode())
			{
				saveImage(scene.getImage(), scene.getDefaultRenderOutputFileName());
				if (scene.hasDenoisedImage())
				{
					saveImage(scene.getDenoisedImage(), denoisedOutputFileName(scene));
				}
				if (scene.getStorePixelRenderTimes())
				{
					std::unique_ptr<Image> debugTime = scene.generateRenderTimeImage();
					debugTime->saveToBMP("renderTime");
				}
			}
		}
	}
	catch (const std::exception& exception)
	{
		std::cerr << CLR_RED << exception.what() << CLR_RESET << std::endl;
		return (1);
	}

	return (0);
}
