#include "AABB.hpp"
#include "Atmosphere.hpp"
#include "AssetPath.hpp"
#include "Color.hpp"
#include "ColorManagement.hpp"
#include "ColorScience.hpp"
#include "EnvironmentMap.hpp"
#include "FlagsParser.hpp"
#include "Image.hpp"
#include "IESProfile.hpp"
#include "LightUnits.hpp"
#include "MeasuredMaterials.hpp"
#include "Materials/BSDF.hpp"
#include "Materials/Lambertian.hpp"
#include "Materials/Emissive.hpp"
#include "Materials/Dielectric.hpp"
#include "Materials/Metal.hpp"
#include "Materials/Principled.hpp"
#include "Materials/Glossy.hpp"
#include "Materials/DiffuseGlossy.hpp"
#include "Materials/Isotropic.hpp"
#include "Materials/HenyeyGreenstein.hpp"
#include "OBJReader.hpp"
#include "PDFs/HittablePDF.hpp"
#include "Renderer/CausticPhotonMap.hpp"
#include "Renderer/Renderer.hpp"
#include "Scene/Scene.hpp"
#include "SceneFile/SceneFile.hpp"
#include "SparseVolumeGrid.hpp"
#include "Hittables/BVHNode.hpp"
#include "Hittables/ConstantVolume.hpp"
#include "Hittables/CloudVolume.hpp"
#include "Hittables/SparseGridVolume.hpp"
#include "Hittables/Cube.hpp"
#include "Hittables/DirectionalLight.hpp"
#include "Hittables/Mesh.hpp"
#include "Hittables/MeshInstance.hpp"
#include "Hittables/Plane.hpp"
#include "Hittables/Rectangle.hpp"
#include "Hittables/Sphere.hpp"
#include "Hittables/Triangle.hpp"
#include "Blur/Gaussian.hpp"
#include "Defaults.hpp"
#include "Random.hpp"
#include "Transform.hpp"
#include "Utilities.hpp"
#include "VolumeGuidingField.hpp"
#include "../src/renderer/RendererInternal.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace
{
	struct TiffTag
	{
		std::uint16_t	id;
		std::uint16_t	type;
		std::uint32_t	count;
		std::uint32_t	valueOrOffset;
	};

	void	require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void	requireNear(double actual, double expected, const std::string& message)
	{
		require(std::abs(actual - expected) < 0.000001, message);
	}

	void	requireNearTolerance(double actual, double expected, double tolerance, const std::string& message)
	{
		require(std::abs(actual - expected) < tolerance, message);
	}

	void	requireVectorNear(const Vector3& actual, const Vector3& expected, const std::string& message)
	{
		requireNear(actual.getX(), expected.getX(), message + " X component is wrong.");
		requireNear(actual.getY(), expected.getY(), message + " Y component is wrong.");
		requireNear(actual.getZ(), expected.getZ(), message + " Z component is wrong.");
	}

	void	requireFiniteNonNegativeColor(const Color& color, const std::string& message)
	{
		require(std::isfinite(color.getRed()) && color.getRed() >= 0.0, message + " Red component is invalid.");
		require(std::isfinite(color.getGreen()) && color.getGreen() >= 0.0, message + " Green component is invalid.");
		require(std::isfinite(color.getBlue()) && color.getBlue() >= 0.0, message + " Blue component is invalid.");
	}

	void	requireColorNear(const Color& actual, const Color& expected, const std::string& message)
	{
		requireNear(actual.getRed(), expected.getRed(), message + " red channel is wrong.");
		requireNear(actual.getGreen(), expected.getGreen(), message + " green channel is wrong.");
		requireNear(actual.getBlue(), expected.getBlue(), message + " blue channel is wrong.");
	}

	template <typename Function>
	void	requireThrows(Function function, const std::string& message)
	{
		try
		{
			function();
		}
		catch (const std::exception&)
		{
			return;
		}

		throw std::runtime_error(message);
	}

	std::vector<unsigned char>	readFile(const std::filesystem::path& path)
	{
		std::ifstream stream(path, std::ios::binary);
		require(stream.good(), "Could not open test file: " + path.string());

		return (std::vector<unsigned char>(
			std::istreambuf_iterator<char>(stream),
			std::istreambuf_iterator<char>()
		));
	}

	void	requirePixelBytes(
		const std::vector<unsigned char>& bytes,
		std::size_t offset,
		unsigned char blue,
		unsigned char green,
		unsigned char red,
		const std::string& message
	)
	{
		require(bytes.size() >= offset + 3, "BMP test file is too small.");
		require(bytes[offset] == blue && bytes[offset + 1] == green && bytes[offset + 2] == red, message);
	}

	void	requireFiniteColor(const Color& color, const std::string& message)
	{
		require(std::isfinite(color.getRed()), message + " red channel is non-finite.");
		require(std::isfinite(color.getGreen()), message + " green channel is non-finite.");
		require(std::isfinite(color.getBlue()), message + " blue channel is non-finite.");
	}

	void	requireDisplayRange(const Color& color, const std::string& message)
	{
		requireFiniteColor(color, message);
		require(color.getRed() >= 0.0 && color.getRed() <= 1.0, message + " red channel is outside display range.");
		require(color.getGreen() >= 0.0 && color.getGreen() <= 1.0, message + " green channel is outside display range.");
		require(color.getBlue() >= 0.0 && color.getBlue() <= 1.0, message + " blue channel is outside display range.");
	}

	void	requireImageIsFinite(const Image& image, const std::string& message)
	{
		for (std::size_t y = 0; y < image.getHeight(); y++)
		{
			for (std::size_t x = 0; x < image.getWidth(); x++)
			{
				requireFiniteColor(image.getPixel(x, y), message);
			}
		}
	}

	void	requireImageIsDisplayable(const Image& image, const std::string& message)
	{
		for (std::size_t y = 0; y < image.getHeight(); y++)
		{
			for (std::size_t x = 0; x < image.getWidth(); x++)
			{
				requireDisplayRange(image.getPixel(x, y), message);
			}
		}
	}

	bool	isVisualForegroundPixel(const Color& color)
	{
		return (
			color.getRed() > 0.85
			&& color.getGreen() < 0.25
			&& color.getBlue() < 0.15
		);
	}

	bool	isVisualBackgroundPixel(const Color& color)
	{
		return (
			std::abs(color.getRed() - 0.02) < 0.000001
			&& std::abs(color.getGreen() - 0.04) < 0.000001
			&& std::abs(color.getBlue() - 0.08) < 0.000001
		);
	}

	Camera	testPinholeCamera(Vector3 position, Vector3 direction, double focusDistanceMeters)
	{
		Camera camera(
			position,
			direction,
			D_CAMERA_FOCAL_LENGTH_METERS,
			D_CAMERA_SENSOR_WIDTH_METERS,
			D_CAMERA_SENSOR_HEIGHT_METERS,
			D_CAMERA_F_NUMBER,
			focusDistanceMeters
		);

		camera.setPinhole(true);
		return (camera);
	}

	void	configureVisualRenderScene(Scene& scene, std::size_t width, std::size_t height)
	{
		scene.getImage()->setWidth(width);
		scene.getImage()->setHeight(height);
		scene.getImage()->initialize();
		scene.setSampleCount(1);
		scene.setAdaptiveSampling(false);
		scene.setMaxLightBounces(1);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(false);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.02, 0.04, 0.08));
		scene.setRenderingThreads(1);
		scene.setBenchmarkMode(true);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, -1.0), 1.0));
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, -1.0),
			0.11,
			std::make_shared<Emissive>(Color(1.0, 0.12, 0.04))
		));
	}

	std::size_t	bmpPixelOffset(std::size_t width, std::size_t height, std::size_t x, std::size_t y)
	{
		const std::size_t rowStride = ((width * 3) + 3) / 4 * 4;

		return (54 + ((height - 1 - y) * rowStride) + (x * 3));
	}

	std::uint16_t	readU16(const std::vector<unsigned char>& bytes, std::size_t offset)
	{
		require(bytes.size() >= offset + 2, "TIFF test file is too small.");
		return (
			static_cast<std::uint16_t>(bytes[offset]) |
			(static_cast<std::uint16_t>(bytes[offset + 1]) << 8)
		);
	}

	std::uint32_t	readU32(const std::vector<unsigned char>& bytes, std::size_t offset)
	{
		require(bytes.size() >= offset + 4, "TIFF test file is too small.");
		return (
			static_cast<std::uint32_t>(bytes[offset]) |
			(static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
			(static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
			(static_cast<std::uint32_t>(bytes[offset + 3]) << 24)
		);
	}

	std::uint32_t	readBE32(const std::vector<unsigned char>& bytes, std::size_t offset)
	{
		require(bytes.size() >= offset + 4, "PNG test file is too small.");
		return (
			(static_cast<std::uint32_t>(bytes[offset]) << 24) |
			(static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
			(static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
			static_cast<std::uint32_t>(bytes[offset + 3])
		);
	}

	std::uint32_t	adler32(const std::vector<unsigned char>& data)
	{
		constexpr std::uint32_t MOD_ADLER = 65521;
		std::uint32_t a = 1;
		std::uint32_t b = 0;

		for (unsigned char byte : data)
		{
			a = (a + byte) % MOD_ADLER;
			b = (b + a) % MOD_ADLER;
		}

		return ((b << 16) | a);
	}

	std::vector<unsigned char>	inflatePNGZlib(const std::vector<unsigned char>& zlibData)
	{
		require(zlibData.size() >= 6, "PNG zlib stream is too small.");

		const std::uint16_t header = static_cast<std::uint16_t>((zlibData[0] << 8) | zlibData[1]);
		require((zlibData[0] & 0x0f) == 8, "PNG zlib stream is not deflate.");
		require((header % 31) == 0, "PNG zlib header check bits are wrong.");

		std::vector<unsigned char> output;
		std::size_t offset = 2;
		const std::size_t end = zlibData.size() - 4;
		bool finalBlock = false;

		while (!finalBlock)
		{
			require(offset + 5 <= end, "PNG stored deflate stream ended early.");
			const unsigned char blockHeader = zlibData[offset++];
			finalBlock = (blockHeader & 1) != 0;

			require(((blockHeader >> 1) & 3) == 0, "PNG deflate block is not stored.");
			require((blockHeader & 0xf8) == 0, "PNG stored deflate padding bits are not zero.");

			const std::uint16_t length = static_cast<std::uint16_t>(
				zlibData[offset] | (zlibData[offset + 1] << 8)
			);
			const std::uint16_t inverseLength = static_cast<std::uint16_t>(
				zlibData[offset + 2] | (zlibData[offset + 3] << 8)
			);
			offset += 4;

			require(static_cast<std::uint16_t>(~length) == inverseLength, "PNG stored deflate length check failed.");
			require(offset + length <= end, "PNG stored deflate data ended early.");
			output.insert(output.end(), zlibData.begin() + offset, zlibData.begin() + offset + length);
			offset += length;
		}

		require(offset == end, "PNG zlib stream has trailing deflate data.");
		require(readBE32(zlibData, zlibData.size() - 4) == adler32(output), "PNG zlib Adler-32 is wrong.");
		return (output);
	}

	float	readF32(const std::vector<unsigned char>& bytes, std::size_t offset)
	{
		const std::uint32_t bits = readU32(bytes, offset);
		float value;

		std::memcpy(&value, &bits, sizeof(value));
		return (value);
	}

	TiffTag	readTiffTag(const std::vector<unsigned char>& bytes, std::size_t offset)
	{
		return (TiffTag{
			readU16(bytes, offset),
			readU16(bytes, offset + 2),
			readU32(bytes, offset + 4),
			readU32(bytes, offset + 8)
		});
	}

	TiffTag	findTiffTag(const std::vector<unsigned char>& bytes, std::size_t ifdOffset, std::uint16_t tagId)
	{
		const std::uint16_t tagCount = readU16(bytes, ifdOffset);

		for (std::uint16_t i = 0; i < tagCount; i++)
		{
			TiffTag tag = readTiffTag(bytes, ifdOffset + 2 + (i * 12));
			if (tag.id == tagId)
			{
				return (tag);
			}
		}

		throw std::runtime_error("TIFF tag not found.");
	}

	struct PNGData
	{
		std::uint32_t				width = 0;
		std::uint32_t				height = 0;
		unsigned char				bitDepth = 0;
		unsigned char				colorType = 0;
		std::vector<unsigned char>	imageData;
		bool						hasSRGBChunk = false;
		std::vector<std::string>	textChunks;
	};

	PNGData	readPNG(const std::filesystem::path& path)
	{
		const std::vector<unsigned char> bytes = readFile(path);
		const unsigned char signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
		PNGData png;
		std::size_t offset = 8;
		bool sawIHDR = false;
		bool sawIEND = false;

		require(bytes.size() >= 8, "PNG test file is too small.");
		for (std::size_t i = 0; i < 8; i++)
		{
			require(bytes[i] == signature[i], "PNG signature is wrong.");
		}

		while (offset < bytes.size())
		{
			const std::uint32_t length = readBE32(bytes, offset);
			const std::size_t typeOffset = offset + 4;
			const std::size_t dataOffset = typeOffset + 4;
			const std::size_t crcOffset = dataOffset + length;

			require(crcOffset + 4 <= bytes.size(), "PNG chunk extends past end of file.");
			const std::string type(reinterpret_cast<const char*>(&bytes[typeOffset]), 4);
			if (type == "IHDR")
			{
				require(length == 13, "PNG IHDR chunk length is wrong.");
				png.width = readBE32(bytes, dataOffset);
				png.height = readBE32(bytes, dataOffset + 4);
				png.bitDepth = bytes[dataOffset + 8];
				png.colorType = bytes[dataOffset + 9];
				require(bytes[dataOffset + 10] == 0, "PNG compression method is wrong.");
				require(bytes[dataOffset + 11] == 0, "PNG filter method is wrong.");
				require(bytes[dataOffset + 12] == 0, "PNG interlace method is wrong.");
				sawIHDR = true;
			}
			else if (type == "IDAT")
			{
				png.imageData.insert(png.imageData.end(), bytes.begin() + dataOffset, bytes.begin() + crcOffset);
			}
			else if (type == "sRGB")
			{
				require(length == 1, "PNG sRGB chunk length is wrong.");
				png.hasSRGBChunk = true;
			}
			else if (type == "tEXt")
			{
				png.textChunks.emplace_back(bytes.begin() + dataOffset, bytes.begin() + crcOffset);
			}
			else if (type == "IEND")
			{
				require(length == 0, "PNG IEND chunk length is wrong.");
				sawIEND = true;
				break;
			}
			offset = crcOffset + 4;
		}

		require(sawIHDR, "PNG file is missing IHDR.");
		require(sawIEND, "PNG file is missing IEND.");
		require(!png.imageData.empty(), "PNG file is missing IDAT data.");
		return (png);
	}

	void	requirePNGPixel(
		const std::vector<unsigned char>& scanlines,
		std::uint32_t width,
		std::uint32_t x,
		std::uint32_t y,
		unsigned char red,
		unsigned char green,
		unsigned char blue,
		const std::string& message
	)
	{
		const std::size_t rowSize = 1 + (static_cast<std::size_t>(width) * 3);
		const std::size_t rowOffset = static_cast<std::size_t>(y) * rowSize;
		const std::size_t pixelOffset = rowOffset + 1 + (static_cast<std::size_t>(x) * 3);

		require(scanlines.size() >= pixelOffset + 3, "PNG scanlines are too small.");
		require(scanlines[rowOffset] == 0, "PNG scanline filter is not none.");
		require(
			scanlines[pixelOffset] == red
			&& scanlines[pixelOffset + 1] == green
			&& scanlines[pixelOffset + 2] == blue,
			message
		);
	}

	void	testColorMath(void)
	{
		Color color(0.25, 0.5, 0.75);
		Color result = (color + Color(0.25, 0.25, 0.25)) * 2.0;

		requireNear(result.getRed(), 1.0, "Color red channel math failed.");
		requireNear(result.getGreen(), 1.5, "Color green channel math failed.");
		requireNear(result.getBlue(), 2.0, "Color blue channel math failed.");
	}

	void	testViewTransformBlackPixel(void)
	{
		Image image(1, 1);
		image.initialize();
		image.setPixel(0, 0, Color(0.0, 0.0, 0.0));

		image.applyViewTransform(ViewTransform::ACES);
		const Color pixel = image.getPixel(0, 0);

		require(std::isfinite(pixel.getRed()), "View transform black produced non-finite red value.");
		require(std::isfinite(pixel.getGreen()), "View transform black produced non-finite green value.");
		require(std::isfinite(pixel.getBlue()), "View transform black produced non-finite blue value.");
		requireNear(pixel.getRed(), 0.0, "View transform black changed red value.");
		requireNear(pixel.getGreen(), 0.0, "View transform black changed green value.");
		requireNear(pixel.getBlue(), 0.0, "View transform black changed blue value.");
	}

	void	testACESViewTransformCompressesHighlights(void)
	{
		Image image(1, 1);
		image.initialize();
		image.setPixel(0, 0, Color(4.0, 2.0, 1.0));

		image.applyViewTransform(ViewTransform::ACES);
		const Color pixel = image.getPixel(0, 0);

		require(pixel.getRed() > pixel.getGreen(), "ACES view transform did not preserve channel order.");
		require(pixel.getGreen() > pixel.getBlue(), "ACES view transform did not preserve highlight color.");
		require(pixel.getRed() <= 1.0, "ACES view transform did not compress red highlight.");
		require(pixel.getGreen() <= 1.0, "ACES view transform did not compress green highlight.");
		require(pixel.getBlue() <= 1.0, "ACES view transform did not compress blue highlight.");
		require(pixel.getRed() > 0.0, "ACES view transform crushed red highlight.");
	}

	void	testStandardViewTransformClipsForDisplay(void)
	{
		Image image(1, 1);
		image.initialize();
		image.setPixel(0, 0, ColorManagement::acescgFromLinearSRGB(Color(2.0, 0.5, 0.25)));

		image.applyViewTransform(ViewTransform::Standard);
		const Color pixel = image.getPixel(0, 0);

		requireNear(pixel.getRed(), 1.0, "Standard view transform did not clip red to display range.");
		requireNearTolerance(pixel.getGreen(), 0.5, 0.0001, "Standard view transform changed display-linear green.");
		requireNearTolerance(pixel.getBlue(), 0.25, 0.0001, "Standard view transform changed display-linear blue.");
	}

	void	testRawViewTransformKeepsSceneLinearData(void)
	{
		Image image(1, 1);
		image.initialize();
		image.setPixel(0, 0, Color(4.0, 2.0, 1.0));

		image.applyViewTransformAndEncodeSRGB(ViewTransform::Raw);
		const Color pixel = image.getPixel(0, 0);

		require(image.getColorEncoding() == ImageColorEncoding::SceneLinearACEScg, "Raw view transform changed image encoding.");
		requireNear(pixel.getRed(), 4.0, "Raw view transform changed scene-linear red.");
		requireNear(pixel.getGreen(), 2.0, "Raw view transform changed scene-linear green.");
		requireNear(pixel.getBlue(), 1.0, "Raw view transform changed scene-linear blue.");
	}

	void	testSRGBGammaCorrection(void)
	{
		Image image(1, 1);
		image.initialize();
		image.setColorEncoding(ImageColorEncoding::DisplayLinearSRGB);
		image.setPixel(0, 0, Color(0.0031308, 0.25, -1.0));

		image.gammaCorrect();
		const Color pixel = image.getPixel(0, 0);

		require(image.getColorEncoding() == ImageColorEncoding::DisplayEncodedSRGB, "Gamma correction did not mark the image as encoded sRGB.");
		requireNear(pixel.getRed(), 0.040449936, "sRGB gamma linear segment is wrong.");
		requireNear(pixel.getGreen(), 0.5370987304831942, "sRGB gamma power segment is wrong.");
		requireNear(pixel.getBlue(), 0.0, "sRGB gamma did not clamp negative values.");
	}

	void	testColorManagementACEScgContract(void)
	{
		const Color displayColor(0.25, 0.5, 0.75);
		const Color acescg = ColorManagement::acescgFromSRGB(displayColor);
		const Color roundTrip = ColorManagement::encodedSRGBFromACEScg(acescg);
		const Color white = ColorManagement::acescgFromSRGB(Color(1.0, 1.0, 1.0));

		require(ColorManagement::workingSpaceName() == std::string("ACEScg scene-linear RGB (AP1 primaries, ACES D60 white)"), "Working space contract changed.");
		requireNearTolerance(roundTrip.getRed(), displayColor.getRed(), 0.00005, "sRGB to ACEScg round trip red channel is wrong.");
		requireNearTolerance(roundTrip.getGreen(), displayColor.getGreen(), 0.00005, "sRGB to ACEScg round trip green channel is wrong.");
		requireNearTolerance(roundTrip.getBlue(), displayColor.getBlue(), 0.00005, "sRGB to ACEScg round trip blue channel is wrong.");
		requireNear(ColorManagement::luminance(white), 1.0, "ACEScg white luminance is wrong.");
	}

	void	testACEScgSceneImageEncodesToSRGB(void)
	{
		Image image(1, 1);
		image.initialize();
		image.setPixel(0, 0, ColorManagement::acescgFromSRGB(Color(0.25, 0.5, 0.75)));

		image.gammaCorrect();
		const Color pixel = image.getPixel(0, 0);

		require(image.getColorEncoding() == ImageColorEncoding::DisplayEncodedSRGB, "Scene-linear ACEScg gamma path did not produce encoded sRGB.");
		requireNearTolerance(pixel.getRed(), 0.25, 0.00005, "ACEScg scene image did not encode red to display sRGB.");
		requireNearTolerance(pixel.getGreen(), 0.5, 0.00005, "ACEScg scene image did not encode green to display sRGB.");
		requireNearTolerance(pixel.getBlue(), 0.75, 0.00005, "ACEScg scene image did not encode blue to display sRGB.");
	}

	void	testExposureAndContrast(void)
	{
		Image image(1, 1);
		image.initialize();
		image.setPixel(0, 0, Color(0.25, 0.5, 1.0));

		image.applyExposure(1.0);
		Color pixel = image.getPixel(0, 0);
		requireNear(pixel.getRed(), 0.5, "Exposure did not double red channel.");
		requireNear(pixel.getGreen(), 1.0, "Exposure did not double green channel.");
		requireNear(pixel.getBlue(), 2.0, "Exposure did not double blue channel.");

		image.setPixel(0, 0, Color(0.25, 0.5, 0.75));
		image.applyContrast(2.0);
		pixel = image.getPixel(0, 0);
		requireNear(pixel.getRed(), 0.0, "Contrast lower endpoint is wrong.");
		requireNear(pixel.getGreen(), 0.5, "Contrast pivot is wrong.");
		requireNear(pixel.getBlue(), 1.0, "Contrast upper endpoint is wrong.");
	}

	void	testPhotographicExposureConversion(void)
	{
		Scene scene;

		scene.setPhotographicExposure(1.0, 1.0, 100.0);
		requireNear(scene.getExposure(), 0.0, "Reference photographic exposure is wrong.");
		scene.setPhotographicExposure(2.0, 1.0, 100.0);
		requireNear(scene.getExposure(), -2.0, "F-number photographic exposure is wrong.");
		scene.setPhotographicExposure(2.0, 4.0, 100.0);
		requireNear(scene.getExposure(), 0.0, "Shutter photographic exposure is wrong.");
		scene.setPhotographicExposure(2.0, 1.0, 400.0);
		requireNear(scene.getExposure(), 0.0, "ISO photographic exposure is wrong.");
	}

	void	testBloomExtractionPreservesHighlightColor(void)
	{
		Image image(2, 1);
		image.initialize();
		image.setPixel(0, 0, Color(4.0, 1.0, 0.0));
		image.setPixel(1, 0, Color(0.1, 0.1, 0.1));

		auto bloom = image.extractBloom(1.0, 0.0);
		const Color highlight = bloom->getPixel(0, 0);
		const Color dark = bloom->getPixel(1, 0);

		require(highlight.getRed() > highlight.getGreen(), "Bloom extraction did not preserve highlight hue.");
		requireNear(highlight.getBlue(), 0.0, "Bloom extraction added blue to a warm highlight.");
		requireNear(dark.getRed(), 0.0, "Bloom extraction included sub-threshold red.");
		requireNear(dark.getGreen(), 0.0, "Bloom extraction included sub-threshold green.");
		requireNear(dark.getBlue(), 0.0, "Bloom extraction included sub-threshold blue.");
	}

	void	testBloomExtractionSuppressesIsolatedFireflies(void)
	{
		Image isolated(9, 9);
		isolated.initialize();
		isolated.setPixel(4, 4, Color(100000.0, 100000.0, 100000.0));

		auto isolatedBloom = isolated.extractBloom(1.0, 0.0);
		double isolatedBloomMax = 0.0;
		for (std::size_t y = 0; y < isolatedBloom->getHeight(); y++)
		{
			for (std::size_t x = 0; x < isolatedBloom->getWidth(); x++)
			{
				isolatedBloomMax = std::max(
					isolatedBloomMax,
					Utilities::luminance(isolatedBloom->getPixel(x, y))
				);
			}
		}
		require(isolatedBloomMax < 0.001, "Bloom extraction kept an isolated firefly.");

		Image clustered(9, 9);
		clustered.initialize();
		clustered.setPixel(3, 4, Color(100.0, 100.0, 100.0));
		clustered.setPixel(4, 4, Color(100.0, 100.0, 100.0));
		clustered.setPixel(5, 4, Color(100.0, 100.0, 100.0));

		auto clusteredBloom = clustered.extractBloom(1.0, 0.0);
		require(
			Utilities::luminance(clusteredBloom->getPixel(4, 4)) > 90.0,
			"Bloom extraction suppressed a real clustered highlight."
		);
	}

	void	testDisplayFireflySuppressionRemovesIsolatedWhitePixels(void)
	{
		Image isolated(7, 7);
		isolated.initialize();
		isolated.fill(Color(0.1, 0.1, 0.1));
		isolated.setColorEncoding(ImageColorEncoding::DisplayEncodedSRGB);
		isolated.setPixel(3, 3, Color(1.0, 1.0, 1.0));

		isolated.suppressIsolatedFireflies();
		requireNear(isolated.getPixel(3, 3).getRed(), 0.1, "Display firefly suppression did not replace isolated red.");
		requireNear(isolated.getPixel(3, 3).getGreen(), 0.1, "Display firefly suppression did not replace isolated green.");
		requireNear(isolated.getPixel(3, 3).getBlue(), 0.1, "Display firefly suppression did not replace isolated blue.");

		Image mutedOutlier(7, 7);
		mutedOutlier.initialize();
		mutedOutlier.fill(Color(0.2, 0.22, 0.23));
		mutedOutlier.setColorEncoding(ImageColorEncoding::DisplayEncodedSRGB);
		mutedOutlier.setPixel(3, 3, Color(0.78, 0.84, 0.74));
		mutedOutlier.suppressIsolatedFireflies();
		require(
			mutedOutlier.getPixel(3, 3).getGreen() < 0.3,
			"Display firefly suppression kept an isolated non-saturated outlier."
		);

		Image sceneLinear(7, 7);
		sceneLinear.initialize();
		sceneLinear.fill(Color(0.1, 0.1, 0.1));
		sceneLinear.setPixel(3, 3, Color(1.0, 1.0, 1.0));
		sceneLinear.suppressIsolatedFireflies();
		requireNear(sceneLinear.getPixel(3, 3).getRed(), 1.0, "Display firefly suppression changed scene-linear data.");

		Image clustered(7, 7);
		clustered.initialize();
		clustered.fill(Color(0.1, 0.1, 0.1));
		clustered.setColorEncoding(ImageColorEncoding::DisplayEncodedSRGB);
		for (std::size_t y = 2; y <= 4; y++)
		{
			for (std::size_t x = 2; x <= 4; x++)
			{
				clustered.setPixel(x, y, Color(1.0, 1.0, 1.0));
			}
		}

		clustered.suppressIsolatedFireflies();
		requireNear(clustered.getPixel(3, 3).getRed(), 1.0, "Display firefly suppression changed clustered highlight center.");
		requireNear(clustered.getPixel(2, 2).getRed(), 1.0, "Display firefly suppression changed clustered highlight corner.");

		Image clippedEdge(7, 7);
		clippedEdge.initialize();
		clippedEdge.fill(Color(0.25, 0.25, 0.25));
		clippedEdge.setColorEncoding(ImageColorEncoding::DisplayEncodedSRGB);
		for (std::size_t y = 2; y <= 4; y++)
		{
			for (std::size_t x = 2; x <= 4; x++)
			{
				clippedEdge.setPixel(x, y, Color(0.88, 0.82, 0.82));
			}
		}
		clippedEdge.setPixel(3, 3, Color(1.0, 1.0, 1.0));
		clippedEdge.setPixel(4, 3, Color(1.0, 1.0, 1.0));

		clippedEdge.suppressIsolatedFireflies();
		require(
			clippedEdge.getPixel(3, 3).getRed() < 0.95,
			"Display firefly suppression kept unsupported saturated reflection pixel."
		);
		require(
			clippedEdge.getPixel(4, 3).getRed() < 0.95,
			"Display firefly suppression kept unsupported saturated reflection neighbor."
		);
	}

	void	testPhysicalLightUnitConversions(void)
	{
		requireColorNear(
			LightUnits::surfaceRadiantPower(Color(1.0, 1.0, 1.0), 2.0 * D_PI, 2.0),
			Color(1.0, 1.0, 1.0),
			"Surface radiant power conversion"
		);
		requireColorNear(
			LightUnits::surfaceLuminousFlux(
				Color(1.0, 1.0, 1.0),
				LightUnits::LUMENS_PER_RADIANT_WATT * 2.0 * D_PI,
				2.0
			),
			Color(1.0, 1.0, 1.0),
			"Surface luminous flux conversion"
		);
		requireNear(
			Utilities::luminance(LightUnits::surfaceRadiance(Color(1.0, 0.0, 0.0), 4.0)),
			4.0,
			"Surface radiance did not preserve requested luminance-weighted radiance."
		);
		requireColorNear(
			LightUnits::directionalIlluminance(Color(1.0, 1.0, 1.0), LightUnits::LUMENS_PER_RADIANT_WATT),
			Color(1.0, 1.0, 1.0),
			"Directional illuminance conversion"
		);
		requireColorNear(
			LightUnits::sphericalLuminousIntensity(Color(1.0, 1.0, 1.0), LightUnits::LUMENS_PER_RADIANT_WATT, 4.0),
			Color(1.0, 1.0, 1.0),
			"Spherical luminous intensity conversion"
		);
		requireNear(
			Utilities::luminance(LightUnits::solarDirectionalIrradiance(ColorScience::solar(), 1.0)),
			LightUnits::SOLAR_DIRECT_IRRADIANCE_W_M2,
			"Solar directional irradiance conversion"
		);
		requireNear(
			Utilities::luminance(LightUnits::solarDiskRadiance(ColorScience::solar(), 1.0)),
			LightUnits::SOLAR_DIRECT_IRRADIANCE_W_M2 / LightUnits::solarSolidAngle(),
			"Solar disk radiance conversion"
		);
		requireThrows(
			[]() { LightUnits::surfaceRadiance(Color(0.0, 0.0, 0.0), 1.0); },
			"Physical light units accepted a zero-luminance positive light color."
		);
	}

	void	writeTestIESProfile(const std::filesystem::path& path, double nadirCandela, double horizonCandela, double zenithCandela)
	{
		std::ofstream stream(path);

		stream << "IESNA:LM-63-1995\n";
		stream << "TILT=NONE\n";
		stream << "1 1000 1 3 1 1 1 0 0 0 1 1 100\n";
		stream << "0 90 180\n";
		stream << "0\n";
		stream << nadirCandela << " " << horizonCandela << " " << zenithCandela << "\n";
	}

	void	writeFlatReflectanceCurve(const std::filesystem::path& path, double reflectance)
	{
		std::ofstream stream(path);

		stream << "360," << reflectance << "\n";
		stream << "830," << reflectance << "\n";
	}

	void	writeRedReflectanceCurve(const std::filesystem::path& path)
	{
		std::ofstream stream(path);

		stream << "# wavelength_nm,reflectance\n";
		stream << "360,0.02\n";
		stream << "450,0.03\n";
		stream << "550,0.08\n";
		stream << "650,0.75\n";
		stream << "830,0.80\n";
	}

	void	testIESProfileLoadsPhysicalLampData(void)
	{
		const std::filesystem::path iesPath = std::filesystem::temp_directory_path() / "luz_unit_test.ies";

		writeTestIESProfile(iesPath, 10.0, 10.0, 10.0);
		const IESProfile uniformProfile = IESProfile::load(iesPath.string());
		require(
			std::abs(uniformProfile.totalLumens() - (4.0 * D_PI * 10.0)) < 0.01,
			"IES profile did not integrate uniform candela to lumens."
		);
		requireNear(uniformProfile.candelaAt(45.0, 0.0), 10.0, "IES uniform candela interpolation");

		writeTestIESProfile(iesPath, 20.0, 10.0, 0.0);
		const IESProfile directionalProfile = IESProfile::load(iesPath.string());
		require(
			directionalProfile.relativeIntensity(Vector3(0.0, -1.0, 0.0), Vector3(0.0, -1.0, 0.0), 0.0)
			> directionalProfile.relativeIntensity(Vector3(1.0, 0.0, 0.0), Vector3(0.0, -1.0, 0.0), 0.0),
			"IES profile did not preserve stronger nadir intensity."
		);
		requireNear(
			directionalProfile.relativeIntensity(Vector3(0.0, 1.0, 0.0), Vector3(0.0, -1.0, 0.0), 0.0),
			0.0,
			"IES profile did not preserve zero zenith intensity."
		);

		std::filesystem::remove(iesPath);
	}

	void	testSpectralColorConversions(void)
	{
		const Color blue = ColorScience::wavelength(450.0);
		const Color green = ColorScience::wavelength(550.0);
		const Color red = ColorScience::wavelength(650.0);

		require(blue.getBlue() > blue.getRed() && blue.getBlue() > blue.getGreen(), "450 nm did not convert to a blue-dominant color.");
		require(green.getGreen() > green.getRed() && green.getGreen() > green.getBlue(), "550 nm did not convert to a green-dominant color.");
		require(red.getRed() > red.getGreen() && red.getRed() > red.getBlue(), "650 nm did not convert to a red-dominant color.");

		const Color warm = ColorScience::blackbody(3000.0);
		const Color cool = ColorScience::blackbody(10000.0);

		require(warm.getRed() > warm.getBlue(), "3000 K blackbody did not convert to a warm color.");
		require(cool.getBlue() > cool.getRed(), "10000 K blackbody did not convert to a cool color.");
		requireThrows([]() { ColorScience::wavelength(200.0); }, "Spectral conversion accepted non-visible wavelength.");
		requireThrows([]() { ColorScience::blackbody(100.0); }, "Spectral conversion accepted invalid blackbody temperature.");
	}

	void	testSpectralReflectanceCurveConversion(void)
	{
		const std::filesystem::path flatPath = std::filesystem::temp_directory_path() / "luz_flat_reflectance.spd";
		const std::filesystem::path redPath = std::filesystem::temp_directory_path() / "luz_red_reflectance.spd";

		writeFlatReflectanceCurve(flatPath, 0.5);
		const Color flat = ColorScience::loadReflectanceCurve(flatPath.string());
		requireColorNear(flat, Color(0.5, 0.5, 0.5), "Flat spectral reflectance did not preserve neutral reflectance.");

		writeRedReflectanceCurve(redPath);
		const Color red = ColorScience::loadReflectanceCurve(redPath.string());
		require(red.getRed() > red.getGreen() && red.getRed() > red.getBlue(), "Spectral reflectance curve did not produce red-dominant color.");
		requireThrows(
			[]()
			{
				ColorScience::reflectanceCurve(std::vector<ColorScience::SpectralSample>{
					{360.0, 0.5},
					{361.0, 1.5}
				});
			},
			"Spectral reflectance accepted reflectance above one."
		);

		std::filesystem::remove(flatPath);
		std::filesystem::remove(redPath);
	}

	void	testGaussianBlurSupportsInPlaceSmallImages(void)
	{
		Image image(3, 3);
		image.initialize();
		image.setPixel(1, 1, Color(1.0, 1.0, 1.0));

		Gaussian::blur(image, image, 3, 1.0);
		const Color center = image.getPixel(1, 1);

		require(std::isfinite(center.getRed()), "In-place Gaussian blur produced non-finite red.");
		require(center.getRed() > 0.0, "In-place Gaussian blur lost the center sample.");
	}

	void	testGaussianBlurPreservesCenteredEnergyAndEdges(void)
	{
		Image centered(5, 5);
		Image centeredBlurred(5, 5);
		centered.initialize();
		centeredBlurred.initialize();
		centered.setPixel(2, 2, Color(1.0, 0.5, 0.25));

		Gaussian::blur(centered, centeredBlurred, 3, 1.0);
		Color sum;
		for (std::size_t y = 0; y < centeredBlurred.getHeight(); y++)
		{
			for (std::size_t x = 0; x < centeredBlurred.getWidth(); x++)
			{
				sum += centeredBlurred.getPixel(x, y);
			}
		}
		requireNear(sum.getRed(), 1.0, "Gaussian blur did not preserve centered red energy.");
		requireNear(sum.getGreen(), 0.5, "Gaussian blur did not preserve centered green energy.");
		requireNear(sum.getBlue(), 0.25, "Gaussian blur did not preserve centered blue energy.");

		Image edge(3, 3);
		Image edgeBlurred(3, 3);
		edge.initialize();
		edgeBlurred.initialize();
		edge.setPixel(0, 0, Color(1.0, 1.0, 1.0));

		Gaussian::blur(edge, edgeBlurred, 3, 1.0);
		const Color corner = edgeBlurred.getPixel(0, 0);
		require(corner.getRed() > 0.0, "Gaussian blur dropped edge highlights at the image border.");
		require(std::isfinite(corner.getRed()), "Gaussian blur edge sample produced non-finite red.");
	}

	void	testTerminalFilePath(void)
	{
		require(
			Utilities::terminalFilePath("named_blocks.bmp") == "./named_blocks.bmp",
			"Terminal file path did not prefix bare relative output."
		);
		require(
			Utilities::terminalFilePath("exports/render.bmp") == "./exports/render.bmp",
			"Terminal file path did not prefix nested relative output."
		);
		require(
			Utilities::terminalFilePath("./named_blocks.bmp") == "./named_blocks.bmp",
			"Terminal file path changed an explicit current-directory path."
		);
		require(
			Utilities::terminalFilePath("../named_blocks.bmp") == "../named_blocks.bmp",
			"Terminal file path changed a parent-directory path."
		);

		const std::string absolutePath = (std::filesystem::temp_directory_path() / "named_blocks.bmp").string();
		require(
			Utilities::terminalFilePath(absolutePath) == absolutePath,
			"Terminal file path changed an absolute path."
		);
	}

	void	testNonSquareBMP(void)
	{
		const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "luz_test_non_square";
		const std::filesystem::path bmpPath = outputPath.string() + ".bmp";

		Image image(3, 2);
		image.initialize();
		image.setPixel(0, 0, Color(1.0, 0.0, 0.0));
		image.setPixel(1, 0, Color(0.0, 1.0, 0.0));
		image.setPixel(2, 0, Color(0.0, 0.0, 1.0));
		image.setPixel(0, 1, Color(0.0, 1.0, 1.0));
		image.setPixel(1, 1, Color(1.0, 0.0, 1.0));
		image.setPixel(2, 1, Color(1.0, 1.0, 0.0));

		image.saveToBMP(outputPath.string());
		const std::vector<unsigned char> bytes = readFile(bmpPath);
		const std::size_t pixelDataOffset = 54;
		const std::size_t rowStride = 12;

		requirePixelBytes(bytes, pixelDataOffset + 0, 255, 255, 0, "BMP bottom-left pixel is wrong.");
		requirePixelBytes(bytes, pixelDataOffset + 3, 255, 0, 255, "BMP bottom-middle pixel is wrong.");
		requirePixelBytes(bytes, pixelDataOffset + 6, 0, 255, 255, "BMP bottom-right pixel is wrong.");
		requirePixelBytes(bytes, pixelDataOffset + rowStride + 0, 0, 0, 255, "BMP top-left pixel is wrong.");
		requirePixelBytes(bytes, pixelDataOffset + rowStride + 3, 0, 255, 0, "BMP top-middle pixel is wrong.");
		requirePixelBytes(bytes, pixelDataOffset + rowStride + 6, 255, 0, 0, "BMP top-right pixel is wrong.");

		std::filesystem::remove(bmpPath);
	}

	void	requireRGBFloats(
		const std::vector<unsigned char>& bytes,
		std::size_t offset,
		float red,
		float green,
		float blue,
		const std::string& message
	)
	{
		requireNear(readF32(bytes, offset), red, message + " red channel is wrong.");
		requireNear(readF32(bytes, offset + 4), green, message + " green channel is wrong.");
		requireNear(readF32(bytes, offset + 8), blue, message + " blue channel is wrong.");
	}

	void	testNonSquareTIFF(void)
	{
		const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "luz_test_non_square";
		const std::filesystem::path tiffPath = outputPath.string() + ".tiff";

		Image image(3, 2);
		image.initialize();
		image.setPixel(0, 0, Color(1.5, 0.25, 0.0));
		image.setPixel(1, 0, Color(0.0, 2.25, 0.125));
		image.setPixel(2, 0, Color(0.5, 0.0, 4.0));
		image.setPixel(0, 1, Color(0.0, 1.0, 1.0));
		image.setPixel(1, 1, Color(1.0, 0.0, 1.0));
		image.setPixel(2, 1, Color(1.0, 1.0, 0.0));

		image.saveToTIFF(outputPath.string());
		const std::vector<unsigned char> bytes = readFile(tiffPath);

		require(bytes[0] == 'I' && bytes[1] == 'I', "TIFF byte order is wrong.");
		require(readU16(bytes, 2) == 42, "TIFF version is wrong.");

		const std::uint32_t ifdOffset = readU32(bytes, 4);
		require(findTiffTag(bytes, ifdOffset, 256).valueOrOffset == 3, "TIFF width is wrong.");
		require(findTiffTag(bytes, ifdOffset, 257).valueOrOffset == 2, "TIFF height is wrong.");

		const TiffTag bitsPerSample = findTiffTag(bytes, ifdOffset, 258);
		require(bitsPerSample.type == 3 && bitsPerSample.count == 3, "TIFF BitsPerSample tag is wrong.");
		require(readU16(bytes, bitsPerSample.valueOrOffset) == 32, "TIFF red bit depth is wrong.");
		require(readU16(bytes, bitsPerSample.valueOrOffset + 2) == 32, "TIFF green bit depth is wrong.");
		require(readU16(bytes, bitsPerSample.valueOrOffset + 4) == 32, "TIFF blue bit depth is wrong.");

		const TiffTag sampleFormat = findTiffTag(bytes, ifdOffset, 339);
		require(sampleFormat.type == 3 && sampleFormat.count == 3, "TIFF SampleFormat tag is wrong.");
		require(readU16(bytes, sampleFormat.valueOrOffset) == 3, "TIFF red sample format is wrong.");
		require(readU16(bytes, sampleFormat.valueOrOffset + 2) == 3, "TIFF green sample format is wrong.");
		require(readU16(bytes, sampleFormat.valueOrOffset + 4) == 3, "TIFF blue sample format is wrong.");

		require(findTiffTag(bytes, ifdOffset, 259).valueOrOffset == 1, "TIFF compression is wrong.");
		require(findTiffTag(bytes, ifdOffset, 262).valueOrOffset == 2, "TIFF photometric interpretation is wrong.");
		require(findTiffTag(bytes, ifdOffset, 277).valueOrOffset == 3, "TIFF samples per pixel is wrong.");
		require(findTiffTag(bytes, ifdOffset, 278).valueOrOffset == 2, "TIFF rows per strip is wrong.");
		require(findTiffTag(bytes, ifdOffset, 279).valueOrOffset == 72, "TIFF strip byte count is wrong.");
		require(findTiffTag(bytes, ifdOffset, 284).valueOrOffset == 1, "TIFF planar configuration is wrong.");
		const TiffTag imageDescription = findTiffTag(bytes, ifdOffset, 270);
		require(imageDescription.type == 2 && imageDescription.count > 1, "TIFF ImageDescription tag is wrong.");
		const std::string description(
			bytes.begin() + imageDescription.valueOrOffset,
			bytes.begin() + imageDescription.valueOrOffset + imageDescription.count - 1
		);
		require(description.find("scene-linear ACEScg") != std::string::npos, "TIFF output is missing ACEScg color metadata.");

		const std::uint32_t pixelDataOffset = findTiffTag(bytes, ifdOffset, 273).valueOrOffset;
		requireRGBFloats(bytes, pixelDataOffset + 0, 1.5f, 0.25f, 0.0f, "TIFF top-left pixel");
		requireRGBFloats(bytes, pixelDataOffset + 12, 0.0f, 2.25f, 0.125f, "TIFF top-middle pixel");
		requireRGBFloats(bytes, pixelDataOffset + 24, 0.5f, 0.0f, 4.0f, "TIFF top-right pixel");
		requireRGBFloats(bytes, pixelDataOffset + 36, 0.0f, 1.0f, 1.0f, "TIFF bottom-left pixel");
		requireRGBFloats(bytes, pixelDataOffset + 48, 1.0f, 0.0f, 1.0f, "TIFF bottom-middle pixel");
		requireRGBFloats(bytes, pixelDataOffset + 60, 1.0f, 1.0f, 0.0f, "TIFF bottom-right pixel");

		std::filesystem::remove(tiffPath);
	}

	void	writeTestPNG(const std::filesystem::path& outputPath)
	{
		Image image(3, 2);

		image.initialize();
		image.setColorEncoding(ImageColorEncoding::DisplayEncodedSRGB);
		image.setPixel(0, 0, Color(1.0, 0.0, 0.0));
		image.setPixel(1, 0, Color(0.0, 1.0, 0.0));
		image.setPixel(2, 0, Color(0.0, 0.0, 1.0));
		image.setPixel(0, 1, Color(0.0, 1.0, 1.0));
		image.setPixel(1, 1, Color(1.0, 0.0, 1.0));
		image.setPixel(2, 1, Color(1.0, 1.0, 0.0));
		image.saveToPNG(outputPath.string());
	}

	void	requireTestPNG(const std::filesystem::path& pngPath)
	{
		const PNGData png = readPNG(pngPath);
		const std::vector<unsigned char> scanlines = inflatePNGZlib(png.imageData);
		const std::size_t rowSize = 1 + (png.width * 3);

		require(png.width == 3, "PNG width is wrong.");
		require(png.height == 2, "PNG height is wrong.");
		require(png.bitDepth == 8, "PNG bit depth is wrong.");
		require(png.colorType == 2, "PNG color type is wrong.");
		require(png.hasSRGBChunk, "PNG display output is missing an sRGB color-space chunk.");
		bool sawColorEncoding = false;
		for (const std::string& textChunk : png.textChunks)
		{
			if (
				textChunk.rfind("LuzColorEncoding", 0) == 0
				&& textChunk.find("display-referred sRGB") != std::string::npos
			)
			{
				sawColorEncoding = true;
			}
		}
		require(sawColorEncoding, "PNG output is missing Luz color encoding metadata.");
		require(scanlines.size() == rowSize * png.height, "PNG scanline byte count is wrong.");
		require((png.imageData[2] & 0x07) == 0x01, "PNG did not use stored deflate.");

		requirePNGPixel(scanlines, png.width, 0, 0, 255, 0, 0, "PNG top-left pixel is wrong.");
		requirePNGPixel(scanlines, png.width, 1, 0, 0, 255, 0, "PNG top-middle pixel is wrong.");
		requirePNGPixel(scanlines, png.width, 2, 0, 0, 0, 255, "PNG top-right pixel is wrong.");
		requirePNGPixel(scanlines, png.width, 0, 1, 0, 255, 255, "PNG bottom-left pixel is wrong.");
		requirePNGPixel(scanlines, png.width, 1, 1, 255, 0, 255, "PNG bottom-middle pixel is wrong.");
		requirePNGPixel(scanlines, png.width, 2, 1, 255, 255, 0, "PNG bottom-right pixel is wrong.");
	}

	void	testNonSquarePNG(void)
	{
		const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "luz_test_non_square";
		const std::filesystem::path pngPath = outputPath.string() + ".png";

		writeTestPNG(outputPath);
		requireTestPNG(pngPath);

		std::filesystem::remove(pngPath);
	}

	void	testSceneFileOutputName(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_output_test.luz";
		const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "luz_custom_output";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "resolution=2,2\n"
				<< "outputfilename=" << outputPath.string() << "\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(
			scene.getDefaultRenderOutputFileName() == outputPath.string() + ".bmp",
			"Scene outputfilename setting was not applied."
		);

		std::filesystem::remove(scenePath);

		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "resolution=2,2\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,1)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "pinhole=1\n"
				<< "focus_distance=1\n"
				<< "}\n"
				<< "objects{\n"
				<< "sphere=(0,0,-1),0.5,material[\n"
				<< "emissive=(1.0,1.0,1.0),4.0\n"
				<< "]\n"
				<< "}\n";
		}

		Scene oldEmissiveSyntaxScene;
		requireThrows(
			[&oldEmissiveSyntaxScene, &scenePath]()
			{
				SceneFile::read(oldEmissiveSyntaxScene, scenePath.string());
			},
			"Scene file accepted an emissive material scalar."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileTiffOutputName(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_tiff_output_test.luz";
		const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "luz_custom_output.tiff";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "resolution=2,2\n"
				<< "outputfilename=" << outputPath.string() << "\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(
			scene.getDefaultRenderOutputFileName() == outputPath.string(),
			"Scene TIFF outputfilename setting was not preserved."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFilePNGOutputSettings(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_png_output_test.luz";
		const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "luz_custom_output.png";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "resolution=2,2\n"
				<< "outputfilename=" << outputPath.string() << "\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(
			scene.getDefaultRenderOutputFileName() == outputPath.string(),
			"Scene PNG outputfilename setting was not preserved."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileDenoiseOutputName(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_denoise_output_test.luz";
		const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "luz_custom_denoised.tiff";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "resolution=2,2\n"
				<< "denoise=1\n"
				<< "denoiseoutputfilename=" << outputPath.string() << "\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getDenoise(), "Scene denoise setting was not applied.");
		require(
			scene.getDenoiseOutputFileName() == outputPath.string(),
			"Scene denoise output setting was not applied."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileUsesSceneDefaults(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_defaults_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "resolution=2,2\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getAdaptiveSampling(), "Scene file did not keep default adaptive sampling.");
		require(scene.getDenoise(), "Scene file did not keep default denoising.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileAdaptiveSettings(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_adaptive_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "adaptive=1\n"
				<< "adaptiveminsamples=32\n"
				<< "adaptivebackgroundminsamples=8\n"
				<< "adaptive_volume_min_samples=48\n"
				<< "adaptivethreshold=0.015\n"
				<< "adaptivecheckinterval=8\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getAdaptiveSampling(), "Scene adaptive setting was not applied.");
		require(scene.getAdaptiveMinSamples() == 32, "Scene adaptive minimum samples setting was not applied.");
		require(
			scene.getAdaptiveBackgroundMinSamples() == 8,
			"Scene adaptive background minimum samples setting was not applied."
		);
		require(
			scene.getAdaptiveVolumeMinSamples() == 48,
			"Scene adaptive volume minimum samples setting was not applied."
		);
		requireNear(scene.getAdaptiveThreshold(), 0.015, "Scene adaptive threshold setting was not applied.");
		require(scene.getAdaptiveCheckInterval() == 8, "Scene adaptive check interval setting was not applied.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileVolumeGuidingSettings(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path()
			/ "luz_scene_volume_guiding_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "volume_guiding_samples=4096\n"
				<< "volume_guiding_resolution=12\n"
				<< "volume_guiding_lobes=24\n"
				<< "volume_guiding_anisotropy=0.85\n"
				<< "volume_guiding_strength=0.3\n"
				<< "volume_guiding_start_bounce=2\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getVolumeGuidingTrainingSamples() == 4096,
			"Scene volume guiding training samples were not parsed.");
		require(scene.getVolumeGuidingResolution() == 12,
			"Scene volume guiding resolution was not parsed.");
		require(scene.getVolumeGuidingLobes() == 24,
			"Scene volume guiding lobe count was not parsed.");
		requireNear(scene.getVolumeGuidingAnisotropy(), 0.85,
			"Scene volume guiding anisotropy was not parsed.");
		requireNear(scene.getVolumeGuidingStrength(), 0.3,
			"Scene volume guiding strength was not parsed.");
		require(scene.getVolumeGuidingStartBounce() == 2,
			"Scene volume guiding start bounce was not parsed.");
		std::filesystem::remove(scenePath);
	}

	void	testSceneFilePostProcessSettings(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_postprocess_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "view_transform=raw\n"
				<< "bloom=0\n"
				<< "exposure=1.5\n"
				<< "contrast=1.25\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getViewTransform() == ViewTransform::Raw, "Scene view_transform setting was not applied.");
		require(!scene.getBloom(), "Scene bloom setting was not applied.");
		requireNear(scene.getExposure(), 1.5, "Scene exposure setting was not applied.");
		requireNear(scene.getContrast(), 1.25, "Scene contrast setting was not applied.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileCausticSettings(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_caustic_settings_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "caustics=1\n"
				<< "caustic_photons=321\n"
				<< "caustic_passes=5\n"
				<< "caustic_radius=0.25\n"
				<< "caustic_alpha=0.6\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getCausticsEnabled(), "Scene caustics setting was not applied.");
		require(scene.getCausticPhotonCount() == 321, "Scene caustic photon count was not parsed.");
		require(scene.getCausticPassCount() == 5, "Scene caustic pass count was not parsed.");
		requireNear(scene.getCausticRadiusMeters(), 0.25, "Scene caustic radius was not parsed.");
		requireNear(scene.getCausticAlpha(), 0.6, "Scene caustic alpha was not parsed.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFilePhotographicExposureSetting(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_photographic_exposure_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "photographic_exposure=2,4,100\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		requireNear(scene.getExposure(), 0.0, "Scene photographic exposure setting was not applied.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileCameraPhotographicExposure(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_camera_photographic_exposure_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,1)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "focus_distance=1\n"
				<< "f_stop=4\n"
				<< "shutter=1\n"
				<< "iso=100\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.hasCamera(), "Scene camera block with photographic exposure did not load a camera.");
		requireNear(scene.getExposure(), -4.0, "Camera photographic exposure was not applied.");

		std::filesystem::remove(scenePath);
	}

	void	requireSceneFileSettingThrows(const std::string& settingLine, const std::string& message)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_invalid_setting_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< settingLine << "\n\n";
		}

		bool threw = false;
		try
		{
			Scene scene;
			SceneFile::read(scene, scenePath.string());
		}
		catch (const std::exception&)
		{
			threw = true;
		}

		std::filesystem::remove(scenePath);
		require(threw, message);
	}

	void	testSceneFileRejectsInvalidSettings(void)
	{
		requireSceneFileSettingThrows("resolution=0,10", "Scene file accepted zero width.");
		requireSceneFileSettingThrows("samples=0", "Scene file accepted zero samples.");
		requireSceneFileSettingThrows("adaptive=2", "Scene file accepted non-binary adaptive setting.");
		requireSceneFileSettingThrows("adaptiveminsamples=0", "Scene file accepted zero adaptive minimum samples.");
		requireSceneFileSettingThrows(
			"adaptivebackgroundminsamples=-1",
			"Scene file accepted negative adaptive background minimum samples."
		);
		requireSceneFileSettingThrows(
			"adaptivevolumeminsamples=-1",
			"Scene file accepted negative adaptive volume minimum samples."
		);
		requireSceneFileSettingThrows("adaptivethreshold=0", "Scene file accepted zero adaptive threshold.");
		requireSceneFileSettingThrows("adaptivecheckinterval=0", "Scene file accepted zero adaptive check interval.");
		requireSceneFileSettingThrows("maxlightbounces=-1", "Scene file accepted negative max light bounces.");
		requireSceneFileSettingThrows("volume_guiding_samples=-1", "Scene file accepted negative guide samples.");
		requireSceneFileSettingThrows("volume_guiding_resolution=0", "Scene file accepted zero guide resolution.");
		requireSceneFileSettingThrows("volume_guiding_lobes=3", "Scene file accepted too few guide lobes.");
		requireSceneFileSettingThrows("volume_guiding_anisotropy=1", "Scene file accepted guide anisotropy above 0.95.");
		requireSceneFileSettingThrows("volume_guiding_strength=0.8", "Scene file accepted guide strength above 0.75.");
		requireSceneFileSettingThrows("volume_guiding_start_bounce=-1", "Scene file accepted a negative guide start bounce.");
		requireSceneFileSettingThrows("gamma=1", "Scene file accepted removed gamma setting.");
		requireSceneFileSettingThrows("tonemapping=1", "Scene file accepted removed tone mapping setting.");
		requireSceneFileSettingThrows("viewtransform=raw", "Scene file accepted view_transform alias.");
		requireSceneFileSettingThrows("view_transform=display", "Scene file accepted invalid view transform.");
		requireSceneFileSettingThrows("bloom=2", "Scene file accepted non-binary bloom.");
		requireSceneFileSettingThrows("exposure=nan", "Scene file accepted non-finite exposure.");
		requireSceneFileSettingThrows("photographic_exposure=0,1,100", "Scene file accepted zero photographic f-number.");
		requireSceneFileSettingThrows("photographic_exposure=2,0,100", "Scene file accepted zero photographic shutter.");
		requireSceneFileSettingThrows("photographic_exposure=2,1,0", "Scene file accepted zero photographic ISO.");
		requireSceneFileSettingThrows("photographic_exposure=2,1", "Scene file accepted incomplete photographic exposure.");
		requireSceneFileSettingThrows("contrast=-1", "Scene file accepted negative contrast.");
		requireSceneFileSettingThrows("caustics=2", "Scene file accepted non-binary caustics.");
		requireSceneFileSettingThrows("caustic_photons=-1", "Scene file accepted negative caustic photons.");
		requireSceneFileSettingThrows("caustic_passes=0", "Scene file accepted zero caustic passes.");
		requireSceneFileSettingThrows("caustic_radius=0", "Scene file accepted zero caustic radius.");
		requireSceneFileSettingThrows("caustic_alpha=0", "Scene file accepted zero caustic alpha.");
		requireSceneFileSettingThrows("caustic_alpha=1.5", "Scene file accepted caustic alpha above one.");
		requireSceneFileSettingThrows("denoise=2", "Scene file accepted non-binary denoise.");
		requireSceneFileSettingThrows("compression=75", "Scene file accepted removed compression setting.");
		requireSceneFileSettingThrows("outputfilename=render.tif", "Scene file accepted .tif output.");
		requireSceneFileSettingThrows("outputfilename=render.jpg", "Scene file accepted unsupported output extension.");
		requireSceneFileSettingThrows("denoiseoutputfilename=", "Scene file accepted empty denoise output name.");
		requireSceneFileSettingThrows("denoiseoutputfilename=render_denoised", "Scene file accepted denoise output without an extension.");
		requireSceneFileSettingThrows("denoiseoutputfilename=render_denoised.tif", "Scene file accepted .tif denoise output.");
		requireSceneFileSettingThrows("denoiseoutputfilename=render_denoised.jpg", "Scene file accepted unsupported denoise output extension.");
		requireSceneFileSettingThrows("sky=wat", "Scene file accepted unknown sky setting.");
		requireSceneFileSettingThrows("environment_scale=-1", "Scene file accepted negative environment scale.");
		requireSceneFileSettingThrows("environment_lighting=2", "Scene file accepted non-binary environment lighting.");
		requireSceneFileSettingThrows("environment_radiance=1\nenvironment_scale=2", "Scene file accepted environment calibration plus scale.");
		requireSceneFileSettingThrows("environment_radiance=1\nenvironment_illuminance=2", "Scene file accepted multiple environment calibrations.");
		requireSceneFileSettingThrows("environmentrotation=nan", "Scene file accepted non-finite environment rotation.");
		requireSceneFileSettingThrows("meters_per_unit=0", "Scene file accepted zero meters per unit.");
		requireSceneFileSettingThrows("meters_per_unit=nan", "Scene file accepted non-finite meters per unit.");
		requireSceneFileSettingThrows("sky=atmosphere\natmosphere=0,1,2,3,4,0,1,0", "Scene file accepted zero atmosphere samples.");
		requireSceneFileSettingThrows("sky=atmosphere\natmosphere=0,2,1,3,4,1,1,0", "Scene file accepted atmosphere radius smaller than Earth radius.");
		requireSceneFileSettingThrows("sky=atmosphere\natmosphere_sun_scale=-1", "Scene file accepted negative atmosphere sun scale.");
	}

	void	testSceneFileAtmosphereSetting(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_atmosphere_setting_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "sky=atmosphere\n"
				<< "atmosphere=0.25,12840000.0,12910000.0,7994.0,1200.0,12,6,0.1\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getRenderSky() == SKY_ATMOSPHERE, "Atmosphere scene sky type was not parsed.");
		requireNear(scene.getAtmosphere().getSunAngle(), 0.25, "Atmosphere sun angle was not parsed.");
		requireVectorNear(
			scene.getAtmosphere().getSunDirection(),
			Vector3(0.0, std::sqrt(0.5), -std::sqrt(0.5)),
			"Atmosphere fallback sun direction"
		);
		requireNear(scene.getAtmosphere().getEarthRadius(), 12840000.0, "Atmosphere Earth radius was not parsed.");
		requireNear(scene.getAtmosphere().getAtmosphereRadius(), 12910000.0, "Atmosphere radius was not parsed.");
		requireNear(scene.getAtmosphere().getStarsBrightness(), 0.1, "Atmosphere stars brightness was not parsed.");
		requireColorNear(
			scene.getAtmosphere().getSunRadiance(),
			LightUnits::solarDirectionalIrradiance(ColorScience::solar(), 1.0),
			"Atmosphere fallback sun radiance"
		);
		requireNear(scene.getAtmosphere().getSunRadianceScale(), 1.0, "Atmosphere sun scale default was not preserved.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileDirectionalSunDrivesAtmosphere(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_atmosphere_directional_sun_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "sky=atmosphere\n"
				<< "atmosphere_sun_scale=0.5\n"
				<< "atmosphere=0.25,12840000.0,12910000.0,7994.0,1200.0,12,6,0.1\n\n"
				<< "[scene]\n"
				<< "directional_light sun {\n"
				<< "direction=(2,0,0)\n"
				<< "color=(1,0.95,0.8)\n"
				<< "irradiance=2\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		requireVectorNear(
			scene.getAtmosphere().getSunDirection(),
			Vector3(-1.0, 0.0, 0.0),
			"Atmosphere did not use the scene directional light as sun direction"
		);
		requireColorNear(
			scene.getAtmosphere().getSunRadiance(),
			LightUnits::directionalIrradiance(Color(1.0, 0.95, 0.8), 2.0),
			"Atmosphere did not use the scene directional light as sun radiance"
		);
		requireNear(scene.getAtmosphere().getSunRadianceScale(), 0.5, "Atmosphere sun scale was not parsed.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileBackgroundSetting(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_background_setting_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "sky=none\n"
				<< "background=(0.1,0.2,0.3)\n\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		const Color background = scene.getBackgroundColor();
		requireNear(background.getRed(), 0.1, "Background red component was not parsed.");
		requireNear(background.getGreen(), 0.2, "Background green component was not parsed.");
		requireNear(background.getBlue(), 0.3, "Background blue component was not parsed.");

		std::filesystem::remove(scenePath);
	}

	void	writeSinglePixelPPM(const std::filesystem::path& path, int red, int green, int blue, int maxValue = 10)
	{
		std::ofstream textureStream(path);
		textureStream
			<< "P3\n"
			<< "1 1\n"
			<< maxValue << "\n"
			<< red << " " << green << " " << blue << "\n";
	}

	void	writeUniformPPM(const std::filesystem::path& path, int width, int height, int red, int green, int blue, int maxValue = 10)
	{
		std::ofstream textureStream(path);
		textureStream
			<< "P3\n"
			<< width << " " << height << "\n"
			<< maxValue << "\n";
		for (int i = 0; i < width * height; i++)
		{
			textureStream << red << " " << green << " " << blue << "\n";
		}
	}

	void	testEnvironmentMapLoadsPPM(void)
	{
		const std::filesystem::path environmentPath = std::filesystem::temp_directory_path() / "luz_environment_ppm_test.ppm";
		writeSinglePixelPPM(environmentPath, 1, 2, 3);

		const EnvironmentMap environmentMap = EnvironmentMap::load(environmentPath.string());
		const Color color = environmentMap.sampleDirection(Vector3(1.0, 0.0, 0.0));
		const Color expected = ColorManagement::acescgFromSRGB(Color(0.1, 0.2, 0.3));
		const EnvironmentMap::Sample sample = environmentMap.sample(0.5, Sampler::Sample2D{0.5, 0.5});

		require(environmentMap.getWidth() == 1, "PPM environment width was not loaded.");
		require(environmentMap.getHeight() == 1, "PPM environment height was not loaded.");
		requireColorNear(color, expected, "PPM environment did not convert sRGB input to ACEScg.");
		require(sample.valid, "PPM environment importance sample was invalid.");
		require(std::isfinite(sample.pdf) && sample.pdf > 0.0, "PPM environment sample PDF was invalid.");

		std::filesystem::remove(environmentPath);
	}

	void	testEnvironmentMapPhysicalMetrics(void)
	{
		std::vector<Color> pixels(4, Color(2.0, 2.0, 2.0));
		const EnvironmentMap environmentMap(2, 2, pixels);

		requireNear(environmentMap.averageLuminance(), 2.0, "Environment average luminance metric is wrong.");
		requireNear(
			environmentMap.horizontalIrradiance(),
			2.0 * std::sqrt(2.0) * D_PI,
			"Environment horizontal irradiance metric is wrong."
		);
	}

	void	testSceneEnvironmentPhysicalCalibration(void)
	{
		std::vector<Color> onePixel(1, Color(2.0, 2.0, 2.0));
		std::vector<Color> fourPixels(4, Color(1.0, 1.0, 1.0));

		Scene scene;
		scene.setEnvironmentMap(std::make_shared<EnvironmentMap>(1, 1, onePixel));
		scene.calibrateEnvironmentAverageRadiance(10.0);
		requireNear(scene.getEnvironmentStrength(), 5.0, "Environment average radiance calibration is wrong.");
		scene.calibrateEnvironmentAverageLuminance(683.0);
		requireNear(scene.getEnvironmentStrength(), 0.5, "Environment average luminance calibration is wrong.");

		const EnvironmentMap horizontalMap(2, 2, fourPixels);
		const double horizontalIrradiance = horizontalMap.horizontalIrradiance();
		scene.setEnvironmentMap(std::make_shared<EnvironmentMap>(horizontalMap));
		scene.calibrateEnvironmentHorizontalIrradiance(horizontalIrradiance * 2.0);
		requireNear(scene.getEnvironmentStrength(), 2.0, "Environment horizontal irradiance calibration is wrong.");
		scene.calibrateEnvironmentHorizontalIlluminance(horizontalIrradiance * 683.0 * 3.0);
		requireNear(scene.getEnvironmentStrength(), 3.0, "Environment horizontal illuminance calibration is wrong.");
	}

	void	testEnvironmentMapLoadsHDR(void)
	{
		const std::filesystem::path environmentPath = std::filesystem::temp_directory_path() / "luz_environment_hdr_test.hdr";
		{
			std::ofstream stream(environmentPath, std::ios::binary);
			const unsigned char pixel[4] = {128, 64, 32, 130};

			stream << "#?RADIANCE\n";
			stream << "FORMAT=32-bit_rle_rgbe\n";
			stream << "\n";
			stream << "-Y 1 +X 1\n";
			stream.write(reinterpret_cast<const char*>(pixel), 4);
		}

		const EnvironmentMap environmentMap = EnvironmentMap::load(environmentPath.string());
		const Color color = environmentMap.sampleDirection(Vector3(0.0, 1.0, 0.0));
		const Color expected = ColorManagement::acescgFromLinearSRGB(Color(2.0, 1.0, 0.5));

		requireColorNear(color, expected, "HDR environment did not convert linear RGBE input to ACEScg.");

		std::filesystem::remove(environmentPath);
	}

	void	testEnvironmentMapLoadsRLEHDR(void)
	{
		const std::filesystem::path environmentPath = std::filesystem::temp_directory_path() / "luz_environment_rle_hdr_test.hdr";
		{
			std::ofstream stream(environmentPath, std::ios::binary);
			const unsigned char scanlineHeader[4] = {2, 2, 0, 8};
			const unsigned char redRun[2] = {136, 128};
			const unsigned char greenRun[2] = {136, 64};
			const unsigned char blueRun[2] = {136, 32};
			const unsigned char exponentRun[2] = {136, 130};

			stream << "#?RADIANCE\n";
			stream << "FORMAT=32-bit_rle_rgbe\n";
			stream << "\n";
			stream << "-Y 1 +X 8\n";
			stream.write(reinterpret_cast<const char*>(scanlineHeader), 4);
			stream.write(reinterpret_cast<const char*>(redRun), 2);
			stream.write(reinterpret_cast<const char*>(greenRun), 2);
			stream.write(reinterpret_cast<const char*>(blueRun), 2);
			stream.write(reinterpret_cast<const char*>(exponentRun), 2);
		}

		const EnvironmentMap environmentMap = EnvironmentMap::load(environmentPath.string());
		const Color color = environmentMap.sampleDirection(Vector3(1.0, 0.0, 0.0));
		const Color expected = ColorManagement::acescgFromLinearSRGB(Color(2.0, 1.0, 0.5));

		require(environmentMap.getWidth() == 8, "RLE HDR environment width was not loaded.");
		requireColorNear(color, expected, "RLE HDR environment did not convert linear RGBE input to ACEScg.");

		std::filesystem::remove(environmentPath);
	}

	void	testSceneFileEnvironmentSetting(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_environment_setting_test.luz";
		const std::filesystem::path environmentPath = directory / "luz_environment_setting_test.ppm";

		writeSinglePixelPPM(environmentPath, 2, 4, 6);
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "environment_luminance=683\n"
				<< "environment=" << environmentPath.filename().string() << ",90\n"
				<< "environment_lighting=0\n\n";
		}

		const EnvironmentMap expectedEnvironment = EnvironmentMap::load(environmentPath.string());
		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getRenderSky() == SKY_ENVIRONMENT, "Environment setting did not enable environment sky.");
		require(scene.hasEnvironmentMap(), "Environment setting did not load an environment map.");
		requireNear(scene.getEnvironmentStrength(), 1.0 / expectedEnvironment.averageLuminance(), "Environment luminance calibration was not parsed.");
		require(!scene.getEnvironmentLighting(), "Environment lighting setting was not parsed.");
		requireNear(scene.getEnvironmentRotation(), 90.0, "Environment rotation was not parsed.");

		std::filesystem::remove(scenePath);
		std::filesystem::remove(environmentPath);
	}

	void	testSceneFileAtmosphereCombinesWithEnvironment(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_atmosphere_environment_setting_test.luz";
		const std::filesystem::path environmentPath = directory / "luz_atmosphere_environment_setting_test.ppm";

		writeUniformPPM(environmentPath, 2, 2, 10, 10, 10);
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "sky=atmosphere\n"
				<< "atmosphere=0.25,12840000.0,12910000.0,7994.0,1200.0,12,6,0.1\n"
				<< "environment=" << environmentPath.filename().string() << ",45\n"
				<< "environment_irradiance=4\n\n";
		}

		const EnvironmentMap expectedEnvironment = EnvironmentMap::load(environmentPath.string());
		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getRenderSky() == SKY_ATMOSPHERE, "Environment setting overwrote explicit atmosphere sky.");
		require(scene.hasEnvironmentMap(), "Atmosphere environment scene did not load an environment map.");
		requireNear(scene.getEnvironmentRotation(), 45.0, "Atmosphere environment rotation was not parsed.");
		requireNear(scene.getEnvironmentStrength(), 4.0 / expectedEnvironment.horizontalIrradiance(), "Environment irradiance calibration was not parsed.");

		std::filesystem::remove(scenePath);
		std::filesystem::remove(environmentPath);
	}

	void	testAtmosphereCompositesEnvironmentBackground(void)
	{
		std::vector<Color> pixels(1, Color(0.2, 0.3, 0.4));
		Scene scene;
		scene.setEnvironmentMap(std::make_shared<EnvironmentMap>(1, 1, pixels));
		scene.setEnvironmentStrength(2.0);
		scene.setRenderSky(SKY_ATMOSPHERE);

		Ray ray(
			Vector3(0.0, 0.0, D_ATMOSPHERE_RADIUS + 100000.0),
			Vector3(0.0, 0.0, 1.0)
		);
		const Color color = Renderer::internal::_computeAtmosphereColor(scene, ray);

		requireColorNear(color, Color(0.4, 0.6, 0.8), "Atmosphere did not composite environment background through transmittance.");
	}

	void	testSceneFileLoadsPhysicalCamera(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_physical_camera_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "meters_per_unit=0.01\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,100)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=24\n"
				<< "f_stop=5\n"
				<< "focus_distance=2\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.hasCamera(), "Physical camera scene did not load a camera.");
		const Camera camera = scene.getActiveCamera();
		requireNear(camera.getFocalLengthMeters(), 0.05, "Camera focal length was not parsed as meters.");
		requireNear(camera.getSensorWidthMeters(), 0.036, "Camera sensor width was not parsed as meters.");
		requireNear(camera.getSensorHeightMeters(), 0.024, "Camera sensor height was not parsed as meters.");
		requireNear(camera.getFNumber(), 5.0, "Camera f-stop was not parsed.");
		requireNear(camera.getFocusDistanceMeters(), 2.0, "Camera focus distance was not parsed as meters.");

		Renderer::internal::RenderCamera renderCamera = Renderer::internal::_prepareRenderCamera(scene);
		requireNear(renderCamera.lensRadius, 0.5, "Camera lens radius did not convert through meters_per_unit.");
		requireNear(Utilities::vectorLength(renderCamera.horizontal), 144.0, "Camera sensor width did not project through physical focal length.");

		std::filesystem::remove(scenePath);
	}

	void	testRenderCameraFitsSensorGateToImageAspect(void)
	{
		Scene wideScene;
		wideScene.getImage()->setWidth(768);
		wideScene.getImage()->setHeight(432);
		wideScene.getImage()->initialize();
		wideScene.addCamera(Camera(
			Vector3(0.0, 0.0, 1.0),
			Vector3(0.0, 0.0, -1.0),
			0.05,
			0.036,
			0.024,
			8.0,
			1.0
		));

		Renderer::internal::RenderCamera wideCamera = Renderer::internal::_prepareRenderCamera(wideScene);
		requireNear(Utilities::vectorLength(wideCamera.horizontal), 0.72, "Wide render changed horizontal camera gate.");
		requireNear(Utilities::vectorLength(wideCamera.vertical), 0.405, "Wide render did not crop vertical camera gate to image aspect.");

		Scene tallScene;
		tallScene.getImage()->setWidth(600);
		tallScene.getImage()->setHeight(800);
		tallScene.getImage()->initialize();
		tallScene.addCamera(Camera(
			Vector3(0.0, 0.0, 1.0),
			Vector3(0.0, 0.0, -1.0),
			0.05,
			0.036,
			0.024,
			8.0,
			1.0
		));

		Renderer::internal::RenderCamera tallCamera = Renderer::internal::_prepareRenderCamera(tallScene);
		requireNear(Utilities::vectorLength(tallCamera.horizontal), 0.36, "Tall render did not crop horizontal camera gate to image aspect.");
		requireNear(Utilities::vectorLength(tallCamera.vertical), 0.48, "Tall render changed vertical camera gate.");
	}

	void	testSceneFileRejectsCompactCameraSyntax(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_compact_camera_rejected_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "camera=(0,0,1),(0,0,-1),45,0,1\n\n";
		}

		Scene scene;
		requireThrows(
			[&scene, &scenePath]()
			{
				SceneFile::read(scene, scenePath.string());
			},
			"Scene file accepted compact camera syntax."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileLoadsRelativeObject(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_relative_object_test.luz";
		const std::filesystem::path objectPath = directory / "luz_relative_object_test.obj";
		{
			std::ofstream objectStream(objectPath);
			objectStream
				<< "v 0.0 0.0 0.0\n"
				<< "v 1.0 0.0 0.0\n"
				<< "v 0.0 1.0 0.0\n"
				<< "f 1 2 3\n";
		}
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[meshes]\n"
				<< "mesh triangle_mesh {\n"
				<< "file=" << objectPath.filename().string() << "\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,5)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "pinhole=1\n"
				<< "focus_distance=5\n"
				<< "}\n"
				<< "object triangle {\n"
				<< "mesh=triangle_mesh\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.hasCamera(), "Relative-object scene did not load a camera.");
		require(scene.getHittables().size() == 1, "Relative-object scene did not load one hittable.");

		std::filesystem::remove(scenePath);
		std::filesystem::remove(objectPath);
	}

	void	testSceneFileCachesRepeatedMeshFileLoads(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_repeated_mesh_cache_test.luz";
		const std::filesystem::path objectPath = directory / "luz_repeated_mesh_cache_test.obj";
		{
			std::ofstream objectStream(objectPath);
			objectStream
				<< "v 0.0 0.0 0.0\n"
				<< "v 1.0 0.0 0.0\n"
				<< "v 0.0 1.0 0.0\n"
				<< "f 1 2 3\n";
		}
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[meshes]\n"
				<< "mesh triangle_mesh {\n"
				<< "file=" << objectPath.filename().string() << "\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "object triangle_a {\n"
				<< "mesh=triangle_mesh\n"
				<< "position=(0,0,0)\n"
				<< "}\n"
				<< "object triangle_b {\n"
				<< "mesh=triangle_mesh\n"
				<< "position=(2,0,0)\n"
				<< "}\n";
		}

		Scene scene;
		std::ostringstream output;
		std::streambuf* const previousOutput = std::cout.rdbuf(output.rdbuf());
		try
		{
			SceneFile::read(scene, scenePath.string());
			std::cout.rdbuf(previousOutput);
		}
		catch (...)
		{
			std::cout.rdbuf(previousOutput);
			throw;
		}

		require(scene.getHittables().size() == 2, "Repeated mesh scene did not load both object instances.");
		require(
			output.str().find("[ 50% ]") == std::string::npos,
			"Repeated named mesh file was counted as multiple mesh loads."
		);

		std::filesystem::remove(scenePath);
		std::filesystem::remove(objectPath);
	}

	void	testAssetPathUsesProvidedScenePaths(void)
	{
		const std::filesystem::path baseDirectory = std::filesystem::temp_directory_path() / "luz_scene_assets";
		const std::filesystem::path relativePath = std::filesystem::path("objects") / "mesh.obj";
		const std::filesystem::path expectedRelativePath = (baseDirectory / relativePath).lexically_normal();
		const std::filesystem::path absolutePath = (baseDirectory / "textures" / "albedo.ppm").lexically_normal();

		require(
			AssetPath::resolve(baseDirectory, relativePath.string()) == expectedRelativePath.string(),
			"Relative scene asset path was not resolved from the scene directory."
		);
		require(
			AssetPath::resolve(baseDirectory, absolutePath.string()) == absolutePath.string(),
			"Absolute scene asset path was not preserved."
		);
		require(
			AssetPath::resolve(relativePath.string()) == relativePath.string(),
			"Relative asset path without a base directory was not preserved."
		);
	}

	void	testSceneFileLoadsTransformedObject(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_obj_transform_test.luz";
		const std::filesystem::path objectPath = directory / "luz_obj_transform_test.obj";
		{
			std::ofstream objectStream(objectPath);
			objectStream
				<< "v 0.0 0.0 0.0\n"
				<< "v 1.0 0.0 0.0\n"
				<< "v 0.0 1.0 0.0\n"
				<< "f 1 2 3\n";
		}
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "objects{\n"
				<< "obj=" << objectPath.filename().string() << ",(1.0,2.0,3.0),material[\n"
				<< "metal=(0.8,0.7,0.6),0.2\n"
				<< "]\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "Transformed OBJ scene did not load one hittable.");
		require(scene.getHittables()[0]->getMaterial()->getType() == METAL, "Transformed OBJ material was not applied.");

		AABB boundingBox;
		require(scene.getHittables()[0]->createBoundingBox(boundingBox), "Transformed OBJ did not create a bounding box.");
		require(boundingBox.getMinimum().getX() > 0.99 && boundingBox.getMinimum().getX() < 1.0, "Transformed OBJ X offset was not applied.");
		require(boundingBox.getMinimum().getY() > 1.99 && boundingBox.getMinimum().getY() < 2.0, "Transformed OBJ Y offset was not applied.");
		require(boundingBox.getMinimum().getZ() > 2.99 && boundingBox.getMinimum().getZ() < 3.0, "Transformed OBJ Z offset was not applied.");

		std::filesystem::remove(scenePath);
		std::filesystem::remove(objectPath);
	}

	void	testSceneFileLoadsNamedBlocks(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_named_blocks_test.luz";
		const std::filesystem::path objectPath = directory / "luz_named_blocks_test.obj";
		{
			std::ofstream objectStream(objectPath);
			objectStream
				<< "v 0.0 0.0 0.0\n"
				<< "v 1.0 0.0 0.0\n"
				<< "v 0.0 1.0 0.0\n"
				<< "f 1 2 3\n";
		}
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "resolution=2,2\n\n"
				<< "[materials]\n"
				<< "material shiny {\n"
				<< "type=principled\n"
				<< "base_color=(0.9,0.8,0.7)\n"
				<< "metallic=1\n"
				<< "roughness=0.2\n"
				<< "}\n\n"
				<< "[meshes]\n"
				<< "mesh triangle_mesh {\n"
				<< "file=" << objectPath.filename().string() << "\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,5)\n"
				<< "direction=(0,0,-1)\n"
				<< "up=(0,1,0)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "pinhole=1\n"
				<< "focus_distance=5\n"
				<< "}\n"
				<< "object triangle {\n"
				<< "mesh=triangle_mesh\n"
				<< "position=(1,2,3)\n"
				<< "rotation=(0,0,90)\n"
				<< "scale=(2,1,1)\n"
				<< "material=shiny\n"
				<< "}\n"
				<< "area_light key {\n"
				<< "position=(0,4,0)\n"
				<< "normal=(0,-1,0)\n"
				<< "size=(2,3)\n"
				<< "color=(1,1,1)\n"
				<< "power=4\n"
				<< "}\n"
				<< "directional_light sun {\n"
				<< "direction=(0,-1,0)\n"
				<< "color=(1,0.95,0.8)\n"
				<< "irradiance=2\n"
				<< "}\n"
				<< "point_light fill {\n"
				<< "position=(2,2,2)\n"
				<< "radius=0.25\n"
				<< "color=(0.5,0.6,1.0)\n"
				<< "power=1.5\n"
				<< "visible=0\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.hasCamera(), "Named-block scene did not load a camera.");
		requireNear(scene.getActiveCamera().getUpDirection().getY(), 1.0, "Named-block camera up vector was not parsed.");
		require(scene.getHittables().size() == 4, "Named-block scene did not load object and lights.");
		require(scene.getHittables()[0]->getMaterial()->getType() == PRINCIPLED, "Metallic principled material did not remain layered principled.");
		require(scene.getHittables()[1]->getMaterial()->getType() == EMISSIVE, "Area light did not create an emissive hittable.");
		require(scene.getHittables()[2]->getMaterial()->getType() == EMISSIVE, "Directional light did not create an emissive hittable.");
		require(scene.getHittables()[3]->getMaterial()->getType() == EMISSIVE, "Point light did not create an emissive hittable.");
		auto pointLight = std::dynamic_pointer_cast<Sphere>(scene.getHittables()[3]);
		require(pointLight != nullptr, "Point light did not create a sphere light.");
		require(!pointLight->isVisible(), "Point light visible flag was not parsed.");
		Ray pointLightRay(Vector3(2.0, 2.0, 3.0), Vector3(0.0, 0.0, -1.0));
		HitRecord pointLightHit;
		require(!pointLight->hit(pointLightRay, pointLightHit, 0.001, 100.0), "Invisible point light was visible to camera rays.");
		HittableLightSample pointLightSample;
		require(pointLight->sampleLight(Vector3(0.0, 0.0, 0.0), pointLightSample), "Invisible point light could not be sampled.");

		scene.updateLights();
		require(scene.getLights().size() == 3, "Named-block lights were not registered as lights.");

		AABB boundingBox;
		require(scene.getHittables()[0]->createBoundingBox(boundingBox), "Named-block object did not create a bounding box.");
		require(boundingBox.getMinimum().getX() > -0.01 && boundingBox.getMinimum().getX() < 0.01, "Named-block object rotation X minimum is wrong.");
		require(boundingBox.getMaximum().getY() > 3.99 && boundingBox.getMaximum().getY() < 4.01, "Named-block object rotation Y maximum is wrong.");
		require(boundingBox.getMinimum().getZ() > 2.99 && boundingBox.getMinimum().getZ() < 3.0, "Named-block object Z offset is wrong.");

		std::filesystem::remove(scenePath);
		std::filesystem::remove(objectPath);
	}

	void	testSceneFileCompactPrimitivesUseNamedMaterials(void)
	{
		const std::filesystem::path objectPath = std::filesystem::temp_directory_path() / "luz_named_material_obj_triangle.obj";
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_compact_named_materials_test.luz";
		{
			std::ofstream objectStream(objectPath);
			objectStream
				<< "v 0.0 0.0 0.0\n"
				<< "v 1.0 0.0 0.0\n"
				<< "v 0.0 1.0 0.0\n"
				<< "f 1 2 3\n";
		}
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[materials]\n"
				<< "material matte_green {\n"
				<< "type=lambertian\n"
				<< "color=(0.1,0.8,0.2)\n"
				<< "}\n"
				<< "material measured_silver {\n"
				<< "type=metal\n"
				<< "preset=silver\n"
				<< "roughness=0.1\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "objects{\n"
				<< "sphere=(0,0,0),1,material=matte_green\n"
				<< "cube=(3,0,0),(0,1,0),1,1,1,material=measured_silver\n"
				<< "plane=-1,(0,1,0),material=matte_green\n"
				<< "rectangle=(0,0,2),(0,0,-1),1,1,material=matte_green\n"
				<< "triangle=(0,0,0),(1,0,0),(0,1,0),material=matte_green\n"
				<< "obj=" << objectPath.filename().string() << ",(0,0,0),material=matte_green\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 6, "Compact primitive named-material scene did not load all hittables.");
		require(scene.getHittables()[0]->getMaterial()->getType() == LAMBERTIAN, "Named material was not applied to compact sphere.");
		require(scene.getHittables()[1]->getMaterial()->getType() == METAL, "Named metal material was not applied to compact cube.");
		require(scene.getHittables()[2]->getMaterial()->getType() == LAMBERTIAN, "Named material was not applied to compact plane.");
		require(scene.getHittables()[3]->getMaterial()->getType() == LAMBERTIAN, "Named material was not applied to compact rectangle.");
		require(scene.getHittables()[4]->getMaterial()->getType() == LAMBERTIAN, "Named material was not applied to compact triangle.");
		require(scene.getHittables()[5]->getMaterial()->getType() == LAMBERTIAN, "Named material was not applied to transformed OBJ.");
		requireColorNear(
			scene.getHittables()[4]->getMaterial()->getColor(),
			Color(0.1, 0.8, 0.2),
			"Compact triangle named material color is wrong."
		);

		std::filesystem::remove(scenePath);
		std::filesystem::remove(objectPath);
	}

	void	testSceneFileRectangleAxesAndUVs(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_rectangle_axes_uv_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[materials]\n"
				<< "material matte {\n"
				<< "type=lambertian\n"
				<< "color=(0.8,0.8,0.8)\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "objects{\n"
				<< "rectangle=(0,0,0),(0,1,0),(0,0,1),(1,0,0),4,2,"
				<< "uvs=(0,0),(1,0),(1,1),(0,1),material=matte\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getHittables().size() == 1, "Rectangle axes/UV scene did not load one hittable.");

		Ray ray(Vector3(0.5, 2.0, 1.0), Vector3(0.0, -1.0, 0.0));
		HitRecord hitRecord;
		require(scene.getHittables()[0]->hit(ray, hitRecord, 0.001, 100.0), "Rectangle axes/UV scene rectangle was not hit.");
		requireNear(hitRecord.u, 0.75, "Scene rectangle U coordinate was not parsed.");
		requireNear(hitRecord.v, 0.75, "Scene rectangle V coordinate was not parsed.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFilePhysicalLightUnits(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_physical_light_units_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream << std::setprecision(17);
			sceneStream
				<< "[materials]\n"
				<< "material unit_emitter {\n"
				<< "type=emissive\n"
				<< "color=(1,1,1)\n"
				<< "radiance=2\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "sphere material_source {\n"
				<< "position=(0,0,0)\n"
				<< "radius=1\n"
				<< "material=unit_emitter\n"
				<< "}\n"
				<< "area_light area {\n"
				<< "position=(0,4,0)\n"
				<< "normal=(0,-1,0)\n"
				<< "size=(2,1)\n"
				<< "color=(1,1,1)\n"
				<< "power=" << 2.0 * D_PI << "\n"
				<< "}\n"
				<< "directional_light sun {\n"
				<< "direction=(0,-1,0)\n"
				<< "color=(1,1,1)\n"
				<< "illuminance=" << LightUnits::LUMENS_PER_RADIANT_WATT << "\n"
				<< "}\n"
				<< "point_light point {\n"
				<< "position=(1,2,3)\n"
				<< "radius=1\n"
				<< "color=(1,1,1)\n"
				<< "lumens=" << LightUnits::LUMENS_PER_RADIANT_WATT * 4.0 * D_PI * D_PI << "\n"
				<< "visible=0\n"
				<< "}\n"
				<< "point_light candela_point {\n"
				<< "position=(3,2,1)\n"
				<< "radius=1\n"
				<< "color=(1,1,1)\n"
				<< "candela=" << LightUnits::LUMENS_PER_RADIANT_WATT * D_PI << "\n"
				<< "visible=0\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 5, "Physical unit scene did not load all emitters.");
		requireColorNear(scene.getHittables()[0]->getMaterial()->emitted(), Color(2.0, 2.0, 2.0), "Material radiance unit");
		requireColorNear(scene.getHittables()[1]->getMaterial()->emitted(), Color(1.0, 1.0, 1.0), "Area light power unit");
		requireColorNear(scene.getHittables()[2]->getMaterial()->emitted(), Color(1.0, 1.0, 1.0), "Directional light illuminance unit");
		requireColorNear(scene.getHittables()[3]->getMaterial()->emitted(), Color(1.0, 1.0, 1.0), "Point light luminous flux unit");
		requireColorNear(scene.getHittables()[4]->getMaterial()->emitted(), Color(1.0, 1.0, 1.0), "Point light candela unit");

		std::filesystem::remove(scenePath);

		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "area_light area {\n"
				<< "position=(0,4,0)\n"
				<< "normal=(0,-1,0)\n"
				<< "size=(2,1)\n"
				<< "color=(1,1,1)\n"
				<< "intensity=2\n"
				<< "}\n";
		}

		Scene invalidScene;
		requireThrows(
			[&invalidScene, &scenePath]()
			{
				SceneFile::read(invalidScene, scenePath.string());
			},
			"Scene file accepted area_light intensity."
		);

		std::filesystem::remove(scenePath);

		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[materials]\n"
				<< "material bad_emitter {\n"
				<< "type=emissive\n"
				<< "color=(1,1,1)\n"
				<< "}\n\n";
		}

		Scene invalidMaterialScene;
		requireThrows(
			[&invalidMaterialScene, &scenePath]()
			{
				SceneFile::read(invalidMaterialScene, scenePath.string());
			},
			"Scene file accepted an emissive material without a physical unit."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileSolarDirectionalLightPreset(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_solar_directional_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "sky=atmosphere\n"
				<< "atmosphere=0.25,12840000.0,12910000.0,7994.0,1200.0,12,6,0.1\n\n"
				<< "[scene]\n"
				<< "directional_light sun {\n"
				<< "direction=(0,-2,0)\n"
				<< "solar=1\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		const std::shared_ptr<DirectionalLight> sun = std::dynamic_pointer_cast<DirectionalLight>(scene.getHittables()[0]);
		require(sun != nullptr, "Solar preset did not create a directional light.");
		require(sun->hasAtmosphereSunRadiance(), "Solar preset did not store atmosphere sun radiance.");
		requireNear(
			Utilities::luminance(sun->getMaterial()->emitted()),
			LightUnits::SOLAR_DIRECT_IRRADIANCE_W_M2,
			"Solar preset did not use calibrated direct irradiance."
		);
		requireColorNear(
			scene.getAtmosphere().getSunRadiance(),
			LightUnits::solarDirectionalIrradiance(ColorScience::solar(), 1.0),
			"Solar preset did not drive atmosphere sun radiance."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileLoadsIESPointLight(void)
	{
		const std::filesystem::path iesPath = std::filesystem::temp_directory_path() / "luz_scene_light.ies";
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_scene_ies_light_test.luz";

		writeTestIESProfile(iesPath, 10.0, 10.0, 10.0);
		const IESProfile profile = IESProfile::load(iesPath.string());
		{
			std::ofstream sceneStream(scenePath);
			sceneStream << std::setprecision(17);
			sceneStream
				<< "[scene]\n"
				<< "point_light lamp {\n"
				<< "position=(0,2,0)\n"
				<< "radius=1\n"
				<< "color=(1,1,1)\n"
				<< "ies=" << iesPath.string() << "\n"
				<< "visible=0\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "IES point light scene did not load the light.");
		const double surfaceArea = 4.0 * D_PI;
		requireColorNear(
			scene.getHittables()[0]->getMaterial()->emitted(),
			LightUnits::surfaceLuminousFlux(Color(1.0, 1.0, 1.0), profile.totalLumens(), surfaceArea),
			"IES point light did not derive flux from the IES candela table."
		);

		std::filesystem::remove(scenePath);
		std::filesystem::remove(iesPath);
	}

	void	testSceneFileMetersPerUnitScalesPhysicalLightArea(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_meters_per_unit_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream << std::setprecision(17);
			sceneStream
				<< "[settings]\n"
				<< "meters_per_unit=0.5\n\n"
				<< "[scene]\n"
				<< "area_light area {\n"
				<< "position=(0,4,0)\n"
				<< "normal=(0,-1,0)\n"
				<< "size=(4,2)\n"
				<< "color=(1,1,1)\n"
				<< "power=" << 2.0 * D_PI << "\n"
				<< "}\n"
				<< "point_light point {\n"
				<< "position=(1,2,3)\n"
				<< "radius=2\n"
				<< "color=(1,1,1)\n"
				<< "lumens=" << LightUnits::LUMENS_PER_RADIANT_WATT * 4.0 * D_PI * D_PI << "\n"
				<< "visible=0\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		requireNear(scene.getMetersPerUnit(), 0.5, "Scene meters_per_unit was not parsed.");
		requireNear(scene.getAtmosphere().getMetersPerUnit(), 0.5, "Scene atmosphere did not inherit meters_per_unit.");
		require(scene.getHittables().size() == 2, "Meters-per-unit scene did not load both lights.");
		requireColorNear(scene.getHittables()[0]->getMaterial()->emitted(), Color(1.0, 1.0, 1.0), "Area light power did not use physical square meters.");
		requireColorNear(scene.getHittables()[1]->getMaterial()->emitted(), Color(1.0, 1.0, 1.0), "Point light lumens did not use physical square meters.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileSpectralColorValues(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_spectral_color_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "background=blackbody(3000K)\n\n"
				<< "[materials]\n"
				<< "material spectral_paint {\n"
				<< "type=lambertian\n"
				<< "color=wavelength(550nm)\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "sphere painted {\n"
				<< "position=(0,0,0)\n"
				<< "radius=1\n"
				<< "material=spectral_paint\n"
				<< "}\n"
				<< "area_light warm_key {\n"
				<< "position=(0,4,0)\n"
				<< "normal=(0,-1,0)\n"
				<< "size=(1,1)\n"
				<< "color=blackbody(3000K)\n"
				<< "radiance=1\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		const Color background = scene.getBackgroundColor();
		require(background.getRed() > background.getBlue(), "Spectral blackbody background was not warm.");
		require(scene.getHittables().size() == 2, "Spectral color scene did not load both hittables.");

		const Color paint = scene.getHittables()[0]->getMaterial()->getColor();
		require(paint.getGreen() > paint.getRed() && paint.getGreen() > paint.getBlue(), "Spectral wavelength material was not green.");

		const Color emission = scene.getHittables()[1]->getMaterial()->emitted();
		require(emission.getRed() > emission.getBlue(), "Spectral blackbody light was not warm.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileLoadsNamedReflectanceSpectra(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_named_reflectance_spectra_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[spectra]\n"
				<< "reflectance green_curve {\n"
				<< "400 0.0\n"
				<< "550 1.0\n"
				<< "700 0.0\n"
				<< "}\n\n"
				<< "[materials]\n"
				<< "material measured_green {\n"
				<< "type=lambertian\n"
				<< "color=reflectance(green_curve)\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "sphere painted {\n"
				<< "position=(0,0,0)\n"
				<< "radius=1\n"
				<< "material=measured_green\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "Named reflectance spectrum scene did not load the sphere.");
		const Color paint = scene.getHittables()[0]->getMaterial()->getColor();
		require(paint.getGreen() > paint.getRed() && paint.getGreen() > paint.getBlue(), "Named reflectance spectrum was not applied to material color.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileExplicitColorSpaces(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_color_space_values_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "sky=none\n"
				<< "background=srgb(0.25,0.5,0.75)\n\n"
				<< "[materials]\n"
				<< "material linear_paint {\n"
				<< "type=lambertian\n"
				<< "color=linear_srgb(0.2,0.4,0.6)\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "sphere painted {\n"
				<< "position=(0,0,0)\n"
				<< "radius=1\n"
				<< "material=linear_paint\n"
				<< "}\n"
				<< "area_light linear_lamp {\n"
				<< "position=(0,2,0)\n"
				<< "normal=(0,-1,0)\n"
				<< "size=(1,1)\n"
				<< "color=linear_srgb(0.7,0.8,0.9)\n"
				<< "radiance=1\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		requireColorNear(
			scene.getBackgroundColor(),
			ColorManagement::acescgFromSRGB(Color(0.25, 0.5, 0.75)),
			"Scene background srgb() value did not convert to ACEScg."
		);
		requireColorNear(
			scene.getHittables()[0]->getMaterial()->getColor(),
			ColorManagement::acescgFromLinearSRGB(Color(0.2, 0.4, 0.6)),
			"Scene material linear_srgb() value did not convert to ACEScg."
		);
		requireColorNear(
			scene.getHittables()[1]->getMaterial()->emitted(),
			LightUnits::surfaceRadiance(ColorManagement::acescgFromLinearSRGB(Color(0.7, 0.8, 0.9)), 1.0),
			"Scene light linear_srgb() value did not convert to ACEScg."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileLoadsAbsorbingDielectricMaterial(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_absorbing_dielectric_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[materials]\n"
				<< "material glass {\n"
				<< "type=dielectric\n"
				<< "color=(1,1,1)\n"
				<< "ior=1.33\n"
				<< "transmittance=(0.25,0.5,1)\n"
				<< "attenuation_distance=2\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "sphere glass_ball {\n"
				<< "position=(0,0,0)\n"
				<< "radius=1\n"
				<< "material=glass\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "Absorbing dielectric scene did not load the sphere.");
		Dielectric* dielectric = dynamic_cast<Dielectric*>(scene.getHittables()[0]->getMaterial());
		require(dielectric != nullptr, "Absorbing dielectric material did not produce a dielectric.");
		requireNear(dielectric->getRefractiveIndex(), 1.33, "Dielectric IOR was not parsed.");
		const Color coefficient = dielectric->getAbsorptionCoefficient();
		requireNear(coefficient.getRed(), -std::log(0.25) / 2.0, "Dielectric red transmittance was not parsed.");
		requireNear(coefficient.getGreen(), -std::log(0.5) / 2.0, "Dielectric green transmittance was not parsed.");
		requireNear(coefficient.getBlue(), 0.0, "Dielectric blue transmittance was not parsed.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileLoadsConductorMetalMaterial(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_conductor_metal_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[materials]\n"
				<< "material measured_gold {\n"
				<< "type=metal\n"
				<< "eta=(0.17,0.35,1.5)\n"
				<< "k=(3.1,2.7,1.9)\n"
				<< "roughness=0\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "sphere metal_ball {\n"
				<< "position=(0,0,0)\n"
				<< "radius=1\n"
				<< "material=measured_gold\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "Conductor metal scene did not load the sphere.");
		Metal* metal = dynamic_cast<Metal*>(scene.getHittables()[0]->getMaterial());
		require(metal != nullptr, "Conductor material did not produce a metal.");
		require(metal->usesConductorFresnel(), "Conductor metal did not enable Fresnel mode.");
		requireColorNear(metal->getEta(), Color(0.17, 0.35, 1.5), "Conductor eta was not parsed.");
		requireColorNear(metal->getExtinctionCoefficient(), Color(3.1, 2.7, 1.9), "Conductor k was not parsed.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileLoadsMeasuredMaterialPresets(void)
	{
		const std::filesystem::path curvePath = std::filesystem::temp_directory_path() / "luz_measured_red_reflectance.spd";
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_measured_materials_test.luz";
		const MeasuredMaterials::Conductor gold = MeasuredMaterials::conductorPreset("gold");
		const MeasuredMaterials::Glass bk7 = MeasuredMaterials::glassPreset("bk7");

		writeRedReflectanceCurve(curvePath);
		{
			std::ofstream sceneStream(scenePath);
			sceneStream << std::setprecision(17);
			sceneStream
				<< "[materials]\n"
				<< "material measured_paint {\n"
				<< "type=lambertian\n"
				<< "color=reflectance(" << curvePath.string() << ")\n"
				<< "}\n"
				<< "material measured_gold {\n"
				<< "type=metal\n"
				<< "preset=gold\n"
				<< "roughness=0.15\n"
				<< "}\n"
				<< "material bk7_glass {\n"
				<< "type=dielectric\n"
				<< "glass_preset=bk7\n"
				<< "ior_wavelength=486.1327nm\n"
				<< "}\n"
				<< "material custom_glass {\n"
				<< "type=dielectric\n"
				<< "ior=1.5168\n"
				<< "abbe_number=64.17\n"
				<< "ior_wavelength=656.2725nm\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "sphere paint {\n"
				<< "position=(-3,0,0)\n"
				<< "radius=1\n"
				<< "material=measured_paint\n"
				<< "}\n"
				<< "sphere metal {\n"
				<< "position=(-1,0,0)\n"
				<< "radius=1\n"
				<< "material=measured_gold\n"
				<< "}\n"
				<< "sphere glass {\n"
				<< "position=(1,0,0)\n"
				<< "radius=1\n"
				<< "material=bk7_glass\n"
				<< "}\n"
				<< "sphere custom {\n"
				<< "position=(3,0,0)\n"
				<< "radius=1\n"
				<< "material=custom_glass\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 4, "Measured material scene did not load all spheres.");
		const Color paint = scene.getHittables()[0]->getMaterial()->getColor();
		require(paint.getRed() > paint.getGreen() && paint.getRed() > paint.getBlue(), "Scene spectral reflectance curve was not red-dominant.");

		Metal* metal = dynamic_cast<Metal*>(scene.getHittables()[1]->getMaterial());
		require(metal != nullptr, "Measured metal preset did not create a metal material.");
		require(metal->usesConductorFresnel(), "Measured metal preset did not enable conductor Fresnel.");
		requireColorNear(metal->getEta(), gold.eta, "Measured gold eta preset is wrong.");
		requireColorNear(metal->getExtinctionCoefficient(), gold.extinctionCoefficient, "Measured gold k preset is wrong.");
		requireNear(metal->getRoughness(), 0.15, "Measured metal roughness was not preserved.");

		Dielectric* presetGlass = dynamic_cast<Dielectric*>(scene.getHittables()[2]->getMaterial());
		require(presetGlass != nullptr, "BK7 preset did not create a dielectric.");
		requireNear(
			presetGlass->getRefractiveIndex(),
			MeasuredMaterials::refractiveIndexFromSellmeier(bk7.sellmeierB, bk7.sellmeierC, 486.1327),
			"BK7 Sellmeier preset did not evaluate at requested wavelength."
		);

		Dielectric* customGlass = dynamic_cast<Dielectric*>(scene.getHittables()[3]->getMaterial());
		require(customGlass != nullptr, "Custom Abbe glass did not create a dielectric.");
		requireNear(
			customGlass->getRefractiveIndex(),
			MeasuredMaterials::refractiveIndexFromAbbe(1.5168, 64.17, 656.2725),
			"Custom Abbe dispersion did not evaluate at requested wavelength."
		);

		std::filesystem::remove(scenePath);
		std::filesystem::remove(curvePath);
	}

	void	testSceneFileLoadsMeasuredVolumeData(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_measured_volumes_test.luz";
		const MeasuredMaterials::Volume haze = MeasuredMaterials::volumePreset("haze");
		const Color manualSigmaS(0.05, 0.02, 0.01);
		const Color manualSigmaA(0.01, 0.02, 0.04);

		{
			std::ofstream sceneStream(scenePath);
			sceneStream << std::setprecision(17);
			sceneStream
				<< "[scene]\n"
				<< "volume measured_haze {\n"
				<< "shape=sphere\n"
				<< "position=(0,0,0)\n"
				<< "radius=1\n"
				<< "preset=haze\n"
				<< "density_scale=2\n"
				<< "}\n"
				<< "volume measured_coefficients {\n"
				<< "shape=box\n"
				<< "position=(3,0,0)\n"
				<< "size=(1,1,1)\n"
				<< "sigma_s=(0.05,0.02,0.01)\n"
				<< "sigma_a=(0.01,0.02,0.04)\n"
				<< "anisotropy=0.4\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 2, "Measured volume scene did not load both volumes.");
		const std::shared_ptr<ConstantVolume> hazeVolume = std::dynamic_pointer_cast<ConstantVolume>(scene.getHittables()[0]);
		const std::shared_ptr<ConstantVolume> manualVolume = std::dynamic_pointer_cast<ConstantVolume>(scene.getHittables()[1]);
		require(hazeVolume != nullptr, "Measured haze preset did not create a ConstantVolume.");
		require(manualVolume != nullptr, "Measured sigma coefficients did not create a ConstantVolume.");

		const Color scaledHazeSigmaS(haze.scatteringCoefficient.getRed() * 2.0, haze.scatteringCoefficient.getGreen() * 2.0, haze.scatteringCoefficient.getBlue() * 2.0);
		const Color scaledHazeSigmaA(haze.absorptionCoefficient.getRed() * 2.0, haze.absorptionCoefficient.getGreen() * 2.0, haze.absorptionCoefficient.getBlue() * 2.0);
		const double hazeDensity = MeasuredMaterials::volumeDensity(scaledHazeSigmaS, scaledHazeSigmaA);
		requireNear(hazeVolume->getDensity(), hazeDensity, "Measured volume preset density is wrong.");
		requireColorNear(
			hazeVolume->getMaterial()->getColor(),
			MeasuredMaterials::volumeScatteringAlbedo(scaledHazeSigmaS, hazeDensity),
			"Measured volume preset albedo is wrong."
		);
		HenyeyGreenstein* hazePhase = dynamic_cast<HenyeyGreenstein*>(hazeVolume->getMaterial());
		require(hazePhase != nullptr, "Measured haze preset did not create an anisotropic phase function.");
		requireNear(hazePhase->getAnisotropy(), haze.anisotropy, "Measured haze anisotropy is wrong.");

		const double manualDensity = MeasuredMaterials::volumeDensity(manualSigmaS, manualSigmaA);
		requireNear(manualVolume->getDensity(), manualDensity, "Measured sigma volume density is wrong.");
		requireColorNear(
			manualVolume->getMaterial()->getColor(),
			MeasuredMaterials::volumeScatteringAlbedo(manualSigmaS, manualDensity),
			"Measured sigma volume albedo is wrong."
		);
		HenyeyGreenstein* manualPhase = dynamic_cast<HenyeyGreenstein*>(manualVolume->getMaterial());
		require(manualPhase != nullptr, "Measured sigma volume did not create an anisotropic phase function.");
		requireNear(manualPhase->getAnisotropy(), 0.4, "Measured sigma volume anisotropy is wrong.");

		std::filesystem::remove(scenePath);
	}

	void	testSphereUVProjectionModes(void)
	{
		const auto material = std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0));
		Sphere sphere(Vector3(0.0, 0.0, 0.0), 1.0, material);
		Ray ray(Vector3(0.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;

		require(sphere.hit(ray, hitRecord, 0.001, 100.0), "Lat-long sphere was not hit.");
		requireNear(hitRecord.u, 0.75, "Lat-long sphere longitude UV was not generated.");
		requireNear(hitRecord.v, 0.5, "Lat-long sphere latitude UV was not generated.");

		sphere.setUVProjection(SphereUVProjection::CubeCross);
		Ray cubeCrossRay(Vector3(0.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0));
		HitRecord cubeCrossHitRecord;
		require(sphere.hit(cubeCrossRay, cubeCrossHitRecord, 0.001, 100.0), "Cube-cross sphere was not hit.");
		requireNear(cubeCrossHitRecord.u, 0.375, "Cube-cross sphere U was not generated.");
		requireNear(cubeCrossHitRecord.v, 0.5, "Cube-cross sphere V was not generated.");
	}

	void	testSphereHitTracksFrontFace(void)
	{
		const auto material = std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0));
		Sphere sphere(Vector3(0.0, 0.0, 0.0), 1.0, material);
		Ray outsideRay(Vector3(0.0, 0.0, -3.0), Vector3(0.0, 0.0, 1.0));
		HitRecord outsideHit;

		require(sphere.hit(outsideRay, outsideHit, 0.001, 100.0), "Outside sphere ray was not hit.");
		require(outsideHit.frontFace, "Outside sphere hit was not marked as front-facing.");
		requireVectorNear(outsideHit.normal, Vector3(0.0, 0.0, -1.0), "Outside sphere normal");

		Ray insideRay(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, 1.0));
		HitRecord insideHit;

		require(sphere.hit(insideRay, insideHit, 0.001, 100.0), "Inside sphere ray was not hit.");
		require(!insideHit.frontFace, "Inside sphere hit was not marked as back-facing.");
		requireVectorNear(insideHit.normal, Vector3(0.0, 0.0, -1.0), "Inside sphere shading normal");
	}

	void	testDielectricUsesExitRefractionRatio(void)
	{
		Dielectric glass(Color(1.0, 1.0, 1.0), 1.5);
		Ray ray(Vector3(0.0, 0.0, 0.0), Vector3(0.9, 0.0, std::sqrt(1.0 - (0.9 * 0.9))));
		HitRecord hitRecord;
		ScatterRecord scatterRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, -1.0);
		hitRecord.frontFace = false;
		Sampler::setRenderSeed(1234);
		Sampler::beginPixelSample(13, 17, 5);
		require(
			Sampler::sample1D(Sampler::DIM_MATERIAL_DECISION) > 0.5,
			"Dielectric test setup did not choose a refractive random sample."
		);
		require(glass.scatter(ray, hitRecord, scatterRecord), "Glass did not scatter.");
		Sampler::endPixelSample();

		require(
			scatterRecord.specularRay.getDirection().getZ() < 0.0,
			"Glass exit above the critical angle did not total-internally reflect."
		);
	}

	void	testDielectricBeerLambertAbsorption(void)
	{
		auto glass = std::make_shared<Dielectric>(Color(1.0, 1.0, 1.0), 1.0);
		glass->setAbsorptionCoefficient(Color(1.0, 0.0, 0.0));

		Scene scene;
		scene.setMetersPerUnit(2.0);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(1.0, 1.0, 1.0));
		scene.setMaxLightBounces(1);
		scene.addHittable(std::make_shared<Sphere>(Vector3(0.0, 0.0, 0.0), 1.0, glass));

		Ray ray(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, 1.0));
		const Color color = Renderer::internal::_calculateLightRaysColor(ray, scene);

		requireNear(color.getRed(), std::exp(-2.0), "Dielectric Beer-Lambert red absorption used the wrong physical distance.");
		requireNear(color.getGreen(), 1.0, "Dielectric Beer-Lambert green absorption is wrong.");
		requireNear(color.getBlue(), 1.0, "Dielectric Beer-Lambert blue absorption is wrong.");

		glass->setTransmittance(Color(std::exp(-2.0), std::exp(-4.0), 1.0), 2.0);
		const Color coefficient = glass->getAbsorptionCoefficient();
		requireNear(coefficient.getRed(), 1.0, "Dielectric transmittance red coefficient is wrong.");
		requireNear(coefficient.getGreen(), 2.0, "Dielectric transmittance green coefficient is wrong.");
		requireNear(coefficient.getBlue(), 0.0, "Dielectric transmittance blue coefficient is wrong.");
	}

	void	testMetalConductorFresnel(void)
	{
		Metal metal;
		metal.setConductorFresnel(Color(0.2, 0.2, 0.2), Color(3.0, 3.0, 3.0));

		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		ScatterRecord scatterRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, 1.0);
		hitRecord.frontFace = true;
		require(metal.scatter(ray, hitRecord, scatterRecord), "Conductor metal did not scatter.");

		const double expectedReflectance = ((0.2 - 1.0) * (0.2 - 1.0) + 9.0) / ((0.2 + 1.0) * (0.2 + 1.0) + 9.0);
		requireNear(scatterRecord.attenuation.getRed(), expectedReflectance, "Conductor Fresnel red reflectance is wrong.");
		requireNear(scatterRecord.attenuation.getGreen(), expectedReflectance, "Conductor Fresnel green reflectance is wrong.");
		requireNear(scatterRecord.attenuation.getBlue(), expectedReflectance, "Conductor Fresnel blue reflectance is wrong.");
	}

	void	testRoughMetalUsesGGXBSDF(void)
	{
		Metal metal(Color(0.8, 0.7, 0.6), 0.45);
		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		ScatterRecord scatterRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, 1.0);
		hitRecord.frontFace = true;
		Sampler::setRenderSeed(2027);
		Sampler::beginPixelSample(3, 5, 7);
		require(metal.scatter(ray, hitRecord, scatterRecord), "Rough metal did not scatter.");
		Sampler::endPixelSample();

		require(!scatterRecord.isSpecular, "Rough metal used a delta specular scatter.");
		require(scatterRecord.pdfType == SCATTER_PDF_BSDF, "Rough metal did not use the BSDF PDF path.");
		require(scatterRecord.bsdfMaterial == &metal, "Rough metal did not store its BSDF material.");
		require(scatterRecord.sampledPDF > 0.0 && std::isfinite(scatterRecord.sampledPDF), "Rough metal sampled PDF is invalid.");
		requireFiniteNonNegativeColor(scatterRecord.attenuation, "Rough metal throughput");

		const double materialPDF = metal.scatteringPDF(ray, hitRecord, scatterRecord.sampledDirection);
		const Color bsdfCos = metal.evaluateBSDFCos(ray, hitRecord, scatterRecord.sampledDirection);
		requireNear(scatterRecord.sampledPDF, materialPDF, "Rough metal sampled PDF does not match material PDF.");
		requireFiniteNonNegativeColor(bsdfCos, "Rough metal BSDF cosine");
		require(BSDF::maxChannel(bsdfCos) > 0.0, "Rough metal BSDF cosine was zero.");
		requireNear(scatterRecord.attenuation.getRed(), bsdfCos.getRed() / materialPDF, "Rough metal red throughput is wrong.");
	}

	void	testGlossyUsesGGXBSDF(void)
	{
		Glossy glossy(Color(0.9, 0.7, 0.5), 0.25);
		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		ScatterRecord scatterRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, 1.0);
		hitRecord.frontFace = true;
		Sampler::setRenderSeed(2031);
		Sampler::beginPixelSample(31, 37, 41);
		require(glossy.scatter(ray, hitRecord, scatterRecord), "Glossy material did not scatter.");
		Sampler::endPixelSample();

		require(!scatterRecord.isSpecular, "Rough glossy material used a delta scatter.");
		require(scatterRecord.pdfType == SCATTER_PDF_BSDF, "Glossy material did not use the BSDF PDF path.");
		require(scatterRecord.bsdfMaterial == &glossy, "Glossy material did not store its BSDF material.");
		require(scatterRecord.sampledPDF > 0.0 && std::isfinite(scatterRecord.sampledPDF), "Glossy sampled PDF is invalid.");
		requireFiniteNonNegativeColor(scatterRecord.attenuation, "Glossy throughput");

		const double materialPDF = glossy.scatteringPDF(ray, hitRecord, scatterRecord.sampledDirection);
		const Color bsdfCos = glossy.evaluateBSDFCos(ray, hitRecord, scatterRecord.sampledDirection);
		requireNear(scatterRecord.sampledPDF, materialPDF, "Glossy sampled PDF does not match material PDF.");
		requireFiniteNonNegativeColor(bsdfCos, "Glossy BSDF cosine");
		require(BSDF::maxChannel(bsdfCos) > 0.0, "Glossy BSDF cosine was zero.");

		const Vector3 mirrorDirection(0.0, 0.0, 1.0);
		const Color mirror = glossy.evaluateBSDFCos(ray, hitRecord, mirrorDirection);
		const Color expected = BSDF::ggxReflectionBSDFCos(
			Color(0.9, 0.7, 0.5),
			hitRecord.normal,
			Vector3(0.0, 0.0, 1.0),
			mirrorDirection,
			BSDF::roughnessToAlpha(0.25)
		);
		requireColorNear(mirror, expected, "Glossy mirror BSDF does not match GGX reflection.");
		require(mirror.getRed() > mirror.getGreen() && mirror.getGreen() > mirror.getBlue(), "Glossy color did not tint reflection.");
	}

	void	testDiffuseGlossyMixPreservesStrongGlossyComponent(void)
	{
		DiffuseGlossy material(Color(1.0, 1.0, 1.0), Color(1.0, 1.0, 1.0), 0.6, 0.1);
		Principled clearcoatProxy(Color(1.0, 1.0, 1.0), 0.0, 0.1, 0.0, 1.5, 0.6, 0.1, 0.0);
		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		ScatterRecord scatterRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, 1.0);
		hitRecord.frontFace = true;
		Sampler::setRenderSeed(2032);
		Sampler::beginPixelSample(43, 47, 53);
		require(material.scatter(ray, hitRecord, scatterRecord), "Diffuse-glossy material did not scatter.");
		Sampler::endPixelSample();

		require(scatterRecord.pdfType == SCATTER_PDF_BSDF, "Diffuse-glossy material did not use the BSDF PDF path.");
		require(scatterRecord.bsdfMaterial == &material, "Diffuse-glossy material did not store its BSDF material.");
		require(scatterRecord.sampledPDF > 0.0 && std::isfinite(scatterRecord.sampledPDF), "Diffuse-glossy sampled PDF is invalid.");

		const Vector3 mirrorDirection(0.0, 0.0, 1.0);
		const Color mixedMirror = material.evaluateBSDFCos(ray, hitRecord, mirrorDirection);
		const Color proxyMirror = clearcoatProxy.evaluateBSDFCos(ray, hitRecord, mirrorDirection);
		require(mixedMirror.getRed() > proxyMirror.getRed() * 10.0, "Diffuse-glossy mix collapsed to weak clearcoat reflection.");
		requireNear(
			material.scatteringPDF(ray, hitRecord, mirrorDirection),
			(0.4 / D_PI) + (0.6 * BSDF::ggxReflectionPDF(
				hitRecord.normal,
				Vector3(0.0, 0.0, 1.0),
				mirrorDirection,
				BSDF::roughnessToAlpha(0.1)
			)),
			"Diffuse-glossy mixture PDF is wrong."
		);
	}

	void	testRoughDielectricUsesGGXBSDF(void)
	{
		Dielectric glass(Color(0.9, 0.95, 1.0), 1.5, 0.35);
		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		ScatterRecord scatterRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, 1.0);
		hitRecord.frontFace = true;
		Sampler::setRenderSeed(2028);
		Sampler::beginPixelSample(11, 13, 17);
		require(glass.scatter(ray, hitRecord, scatterRecord), "Rough dielectric did not scatter.");
		Sampler::endPixelSample();

		require(!scatterRecord.isSpecular, "Rough dielectric used a delta specular scatter.");
		require(scatterRecord.pdfType == SCATTER_PDF_BSDF, "Rough dielectric did not use the BSDF PDF path.");
		require(scatterRecord.bsdfMaterial == &glass, "Rough dielectric did not store its BSDF material.");
		require(scatterRecord.sampledPDF > 0.0 && std::isfinite(scatterRecord.sampledPDF), "Rough dielectric sampled PDF is invalid.");
		requireFiniteNonNegativeColor(scatterRecord.attenuation, "Rough dielectric throughput");

		const double materialPDF = glass.scatteringPDF(ray, hitRecord, scatterRecord.sampledDirection);
		const Color bsdfCos = glass.evaluateBSDFCos(ray, hitRecord, scatterRecord.sampledDirection);
		requireNear(scatterRecord.sampledPDF, materialPDF, "Rough dielectric sampled PDF does not match material PDF.");
		requireFiniteNonNegativeColor(bsdfCos, "Rough dielectric BSDF cosine");
		require(BSDF::maxChannel(bsdfCos) > 0.0, "Rough dielectric BSDF cosine was zero.");
	}

	void	testPrincipledLayeredBSDF(void)
	{
		Principled material(Color(0.8, 0.3, 0.1), 0.2, 0.45, 0.35, 1.45, 0.5, 0.12, 0.4);
		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		ScatterRecord scatterRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, 1.0);
		hitRecord.frontFace = true;
		material.setAbsorptionCoefficient(Color(0.1, 0.2, 0.3));
		Sampler::setRenderSeed(2029);
		Sampler::beginPixelSample(19, 23, 29);
		require(material.scatter(ray, hitRecord, scatterRecord), "Principled material did not scatter.");
		Sampler::endPixelSample();

		require(!scatterRecord.isSpecular, "Principled material used a delta scatter.");
		require(scatterRecord.pdfType == SCATTER_PDF_BSDF, "Principled material did not use the BSDF PDF path.");
		require(scatterRecord.bsdfMaterial == &material, "Principled material did not store its BSDF material.");
		require(scatterRecord.sampledPDF > 0.0 && std::isfinite(scatterRecord.sampledPDF), "Principled sampled PDF is invalid.");
		require(scatterRecord.hasMediumAbsorption, "Transmissive principled material did not carry absorption.");
		requireFiniteNonNegativeColor(scatterRecord.attenuation, "Principled throughput");

		const double materialPDF = material.scatteringPDF(ray, hitRecord, scatterRecord.sampledDirection);
		const Color bsdfCos = material.evaluateBSDFCos(ray, hitRecord, scatterRecord.sampledDirection);
		requireNear(scatterRecord.sampledPDF, materialPDF, "Principled sampled PDF does not match material PDF.");
		requireFiniteNonNegativeColor(bsdfCos, "Principled BSDF cosine");
		require(BSDF::maxChannel(bsdfCos) > 0.0, "Principled BSDF cosine was zero.");
	}

	void	testSmoothTransmissivePrincipledUsesDeltaScatter(void)
	{
		Principled glass(Color(0.8, 0.9, 1.0), 0.0, 0.0, 1.0, 1.5, 0.0, 0.03, 0.0);
		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		ScatterRecord scatterRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, 1.0);
		hitRecord.frontFace = true;
		glass.setAbsorptionCoefficient(Color(0.1, 0.2, 0.3));
		Sampler::setRenderSeed(2030);
		Sampler::beginPixelSample(31, 37, 41);
		require(glass.scatter(ray, hitRecord, scatterRecord), "Smooth transmissive principled material did not scatter.");
		Sampler::endPixelSample();

		require(scatterRecord.isSpecular, "Smooth transmissive principled material did not use delta scatter.");
		require(scatterRecord.pdfType == SCATTER_PDF_NONE, "Smooth transmissive principled material used a sampled BSDF PDF.");
		require(scatterRecord.sampledPDF == 0.0, "Smooth transmissive principled material left a sampled PDF.");
		require(scatterRecord.bsdfMaterial == nullptr, "Smooth transmissive principled material stored a BSDF material.");
		require(scatterRecord.hasMediumAbsorption, "Smooth transmissive principled material did not carry absorption.");
		requireColorNear(
			scatterRecord.mediumAbsorptionCoefficient,
			Color(0.1, 0.2, 0.3),
			"Smooth transmissive principled absorption changed."
		);
		require(
			std::isfinite(scatterRecord.specularRay.getDirection().getX())
			&& std::isfinite(scatterRecord.specularRay.getDirection().getY())
			&& std::isfinite(scatterRecord.specularRay.getDirection().getZ()),
			"Smooth transmissive principled produced an invalid specular direction."
		);
		requireFiniteNonNegativeColor(scatterRecord.attenuation, "Smooth transmissive principled throughput");
	}

	void	testPrincipledSubsurfaceControls(void)
	{
		Principled burley(Color(0.8, 0.42, 0.28), 0.0, 0.5);
		Principled thin(Color(0.8, 0.42, 0.28), 0.0, 0.5);
		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;

		hitRecord.position = Vector3(0.0, 0.0, 0.0);
		hitRecord.normal = Vector3(0.0, 0.0, 1.0);
		hitRecord.geometricNormal = hitRecord.normal;
		hitRecord.frontFace = true;

		burley.setSkinSubsurfaceProfile();
		requireNear(burley.getSubsurface(), 0.5, "Skin subsurface profile did not set a useful weight.");
		require(burley.getSubsurfaceMethod() == SUBSURFACE_BURLEY, "Skin subsurface profile did not use Burley diffusion.");
		requireColorNear(burley.getSubsurfaceRadius(), Color(1.0, 0.35, 0.18), "Skin subsurface radius is wrong.");
		requireNear(burley.getSubsurfaceScale(), 0.0012, "Skin subsurface scale is wrong.");
		requireColorNear(burley.getSubsurfaceColor(), Color(1.0, 0.42, 0.28), "Skin subsurface color is wrong.");

		const Vector3 frontDirection(0.0, 0.0, 1.0);
		const Vector3 backDirection(0.0, 0.0, -1.0);
		const Color burleyFront = burley.evaluateBSDFCos(ray, hitRecord, frontDirection);
		const Color burleyBack = burley.evaluateBSDFCos(ray, hitRecord, backDirection);
		require(BSDF::maxChannel(burleyFront) > 0.0, "Burley subsurface did not evaluate on the front side.");
		requireColorNear(burleyBack, Color(0.0, 0.0, 0.0), "Burley subsurface incorrectly transmitted through a surface.");

		thin.setSubsurface(0.8);
		thin.setSubsurfaceMethod(SUBSURFACE_THIN);
		thin.setSubsurfaceRadius(Color(1.2, 0.45, 0.2));
		thin.setSubsurfaceScale(0.002);
		thin.setSubsurfaceColor(Color(1.0, 0.55, 0.3));
		require(thin.usesThinSubsurface(), "Thin subsurface was not reported as transmissive.");
		requireColorNear(thin.getSubsurfaceRadius(), Color(1.2, 0.45, 0.2), "Thin subsurface radius setter failed.");
		requireNear(thin.getSubsurfaceScale(), 0.002, "Thin subsurface scale setter failed.");
		requireColorNear(thin.getSubsurfaceColor(), Color(1.0, 0.55, 0.3), "Thin subsurface color setter failed.");
		require(
			BSDF::maxChannel(thin.evaluateBSDFCos(ray, hitRecord, backDirection)) > 0.0,
			"Thin subsurface did not evaluate through the back side."
		);
		require(
			thin.scatteringPDF(ray, hitRecord, backDirection) > 0.0,
			"Thin subsurface PDF was zero through the back side."
		);
	}

	void	testSceneFileLoadsNamedTexturedSphere(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_named_textured_sphere_test.luz";
		const std::filesystem::path texturePath = directory / "luz_named_textured_sphere_test.ppm";
		{
			std::ofstream textureStream(texturePath);
			textureStream
				<< "P3\n"
				<< "1 1\n"
				<< "255\n"
				<< "255 0 0\n";
		}
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "resolution=2,2\n\n"
				<< "[materials]\n"
				<< "material textured_earth {\n"
				<< "type=lambertian\n"
				<< "color=(1,1,1)\n"
				<< "texture=" << texturePath.filename().string() << "\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(1,2,10)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "pinhole=1\n"
				<< "focus_distance=7\n"
				<< "}\n"
				<< "sphere earth {\n"
				<< "position=(1,2,3)\n"
				<< "radius=4\n"
				<< "material=textured_earth\n"
				<< "uv_projection=cube_cross\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "Named textured sphere scene did not load one hittable.");
		auto sphere = std::dynamic_pointer_cast<Sphere>(scene.getHittables()[0]);
		require(sphere != nullptr, "Named textured sphere block did not create a sphere.");
		requireVectorNear(sphere->getPosition(), Vector3(1.0, 2.0, 3.0), "Named sphere position was not parsed.");
		requireNear(sphere->getRadius(), 4.0, "Named sphere radius was not parsed.");
		require(sphere->getUVProjection() == SphereUVProjection::CubeCross, "Named sphere UV projection was not parsed.");

		Ray ray(Vector3(1.0, 2.0, 10.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		require(sphere->hit(ray, hitRecord, 0.001, 100.0), "Named textured sphere was not hit.");
		requireNear(hitRecord.u, 0.375, "Sphere cube-cross U was not generated.");
		requireNear(hitRecord.v, 0.5, "Sphere cube-cross V was not generated.");
		const Color texturedColor = hitRecord.material->colorAt(hitRecord);
		requireColorNear(
			texturedColor,
			ColorManagement::acescgFromSRGB(Color(1.0, 0.0, 0.0)),
			"Named sphere texture did not convert sRGB input to ACEScg."
		);

		std::filesystem::remove(scenePath);
		std::filesystem::remove(texturePath);
	}

	void	testSceneFileLoadsVolumeBlock(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_volume_block_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[materials]\n"
				<< "material forward_mist {\n"
				<< "type=phase\n"
				<< "color=(0.7,0.78,1.0)\n"
				<< "anisotropy=0.62\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,8)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "pinhole=1\n"
				<< "focus_distance=8\n"
				<< "}\n"
				<< "volume mist {\n"
				<< "shape=sphere\n"
				<< "position=(1,2,3)\n"
				<< "radius=4\n"
				<< "density=0.25\n"
				<< "material=forward_mist\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "Volume block did not load one hittable.");
		auto volume = std::dynamic_pointer_cast<ConstantVolume>(scene.getHittables()[0]);
		require(volume != nullptr, "Volume block did not create a ConstantVolume.");
		requireNear(volume->getDensity(), 0.25, "Volume density was not parsed.");
		require(volume->getMaterial()->getType() == HENYEY_GREENSTEIN, "Volume phase material was not parsed.");
		const HenyeyGreenstein* phase = dynamic_cast<const HenyeyGreenstein*>(volume->getMaterial());
		require(phase != nullptr, "Volume phase material has the wrong runtime type.");
		requireNear(phase->getAnisotropy(), 0.62, "Volume phase anisotropy was not parsed.");

		AABB boundingBox;
		require(volume->createBoundingBox(boundingBox), "Volume did not create a bounding box.");
		requireNear(boundingBox.getMinimum().getX(), -3.0, "Volume sphere bounds minimum X is wrong.");
		requireNear(boundingBox.getMaximum().getX(), 5.0, "Volume sphere bounds maximum X is wrong.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileLoadsProceduralCloudBlock(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_cloud_block_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "meters_per_unit=2\n\n"
				<< "[scene]\n"
				<< "cloud thunderhead {\n"
				<< "type=cumulonimbus\n"
				<< "position=(10,20,30)\n"
				<< "size=(4000,8000,5000)\n"
				<< "coverage=0.52\n"
				<< "extinction=0.02\n"
				<< "albedo=(0.98,0.99,1.0)\n"
				<< "anisotropy=0.86\n"
				<< "backscatter=-0.3\n"
				<< "forward_weight=0.9\n"
				<< "feature_scale=750\n"
				<< "detail=0.8\n"
				<< "erosion=0.4\n"
				<< "puffiness=0.88\n"
				<< "towering=0.94\n"
				<< "dominance=0.91\n"
				<< "overhang=0.83\n"
				<< "fine_detail=0.87\n"
				<< "shear_direction=(1,0,-0.25)\n"
				<< "seed=1234\n"
				<< "offset=(1,2,3)\n"
				<< "quality=cinematic\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getHittables().size() == 1, "Cloud block did not load one hittable.");
		const std::shared_ptr<CloudVolume> cloud = std::dynamic_pointer_cast<CloudVolume>(scene.getHittables()[0]);
		require(cloud != nullptr, "Cloud block did not create a CloudVolume.");
		const CloudParameters& parameters = cloud->getParameters();
		require(parameters.type == CloudType::Cumulonimbus, "Cloud type was not parsed.");
		requireVectorNear(parameters.position, Vector3(10.0, 20.0, 30.0), "Cloud position");
		requireVectorNear(parameters.size, Vector3(4000.0, 8000.0, 5000.0), "Cloud size");
		requireNear(parameters.coverage, 0.52, "Cloud coverage was not parsed.");
		requireNear(parameters.extinction, 0.02, "Cloud extinction was not parsed.");
		requireColorNear(parameters.albedo, Color(0.98, 0.99, 1.0), "Cloud albedo");
		requireNear(parameters.anisotropy, 0.86, "Cloud anisotropy was not parsed.");
		requireNear(parameters.backscatter, -0.3, "Cloud backscatter was not parsed.");
		requireNear(parameters.forwardWeight, 0.9, "Cloud forward phase weight was not parsed.");
		requireNear(parameters.featureScale, 750.0, "Cloud feature scale was not parsed.");
		requireNear(parameters.detail, 0.8, "Cloud detail was not parsed.");
		requireNear(parameters.erosion, 0.4, "Cloud erosion was not parsed.");
		requireNear(parameters.puffiness, 0.88, "Cloud puffiness was not parsed.");
		requireNear(parameters.towering, 0.94, "Cloud towering was not parsed.");
		requireNear(parameters.dominance, 0.91, "Cloud dominance was not parsed.");
		requireNear(parameters.overhang, 0.83, "Cloud overhang was not parsed.");
		requireNear(parameters.fineDetail, 0.87, "Cloud fine detail was not parsed.");
		requireVectorNear(parameters.shearDirection, Vector3(1.0, 0.0, -0.25), "Cloud shear direction");
		require(parameters.seed == 1234, "Cloud seed was not parsed.");
		requireVectorNear(parameters.offset, Vector3(1.0, 2.0, 3.0), "Cloud noise offset");
		require(parameters.detailOctaves >= 4, "Cinematic cloud quality did not raise detail octaves.");
		require(parameters.maxTrackingSteps == 1024, "Cinematic cloud quality did not raise tracking steps.");
		requireNear(parameters.metersPerUnit, 2.0, "Cloud did not inherit meters_per_unit.");
		require(cloud->getMaterial()->getType() == HENYEY_GREENSTEIN, "Cloud did not use a phase material.");
		const HenyeyGreenstein* cloudPhase = dynamic_cast<const HenyeyGreenstein*>(cloud->getMaterial());
		require(cloudPhase != nullptr, "Cloud phase has the wrong runtime type.");
		requireNear(cloudPhase->getSecondaryAnisotropy(), -0.3, "Cloud phase backscatter is wrong.");
		requireNear(cloudPhase->getPrimaryWeight(), 0.9, "Cloud phase mixture weight is wrong.");

		AABB boundingBox;
		require(cloud->createBoundingBox(boundingBox), "Cloud did not create a bounding box.");
		requireVectorNear(boundingBox.getMinimum(), Vector3(-1990.0, -3980.0, -2470.0), "Cloud minimum bounds");
		requireVectorNear(boundingBox.getMaximum(), Vector3(2010.0, 4020.0, 2530.0), "Cloud maximum bounds");
		std::filesystem::remove(scenePath);
	}

	void	testSceneFileCloudAliasesAndQualityPrecedence(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_cloud_alias_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "cloud layered {\n"
				<< "preset=strato-cumulus\n"
				<< "center=(1,2,3)\n"
				<< "dimensions=(100,200,300)\n"
				<< "width=400\n"
				<< "height=500\n"
				<< "depth=600\n"
				<< "sigma_t=0.006\n"
				<< "scattering_color=(0.91,0.92,0.93)\n"
				<< "g=0.7\n"
				<< "backscatter_anisotropy=-0.2\n"
				<< "phase_mix=0.8\n"
				<< "droplet_size_microns=20\n"
				<< "formation_scale=1.35\n"
				<< "noise_scale=90\n"
				<< "billowing=0.41\n"
				<< "convection=0.22\n"
				<< "hero=0.13\n"
				<< "crown=0.27\n"
				<< "micro_detail=0.61\n"
				<< "scatter_falloff=0.81\n"
				<< "scatter_compensation=0.63\n"
				<< "wind_direction=(0,0,1)\n"
				<< "noise_offset=(4,5,6)\n"
				<< "octaves=5\n"
				<< "tracking_steps=333\n"
				<< "quality=preview\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getHittables().size() == 1, "Cloud alias scene did not load one hittable.");
		const std::shared_ptr<CloudVolume> cloud = std::dynamic_pointer_cast<CloudVolume>(scene.getHittables()[0]);
		require(cloud != nullptr, "Cloud alias scene did not create a CloudVolume.");
		const CloudParameters& parameters = cloud->getParameters();
		require(parameters.type == CloudType::Stratocumulus, "Hyphenated stratocumulus alias was not parsed.");
		requireVectorNear(parameters.position, Vector3(1.0, 2.0, 3.0), "Cloud center alias");
		requireVectorNear(parameters.size, Vector3(400.0, 500.0, 600.0), "Cloud per-axis size overrides");
		requireNear(parameters.extinction, 0.006, "Cloud sigma_t alias was not parsed.");
		requireColorNear(parameters.albedo, Color(0.91, 0.92, 0.93), "Cloud scattering color alias");
		requireNear(parameters.anisotropy, 0.7, "Cloud g alias was not parsed.");
		requireNear(parameters.backscatter, -0.2, "Cloud backscatter alias was not parsed.");
		requireNear(parameters.forwardWeight, 0.8, "Cloud phase mix alias was not parsed.");
		requireNear(parameters.dropletSizeMicrons, 20.0, "Cloud droplet size alias was not parsed.");
		requireNear(parameters.macroScale, 1.35, "Cloud formation scale alias was not parsed.");
		requireNear(parameters.featureScale, 90.0, "Cloud noise scale alias was not parsed.");
		requireNear(parameters.puffiness, 0.41, "Cloud billowing alias was not parsed.");
		requireNear(parameters.towering, 0.22, "Cloud convection alias was not parsed.");
		requireNear(parameters.dominance, 0.13, "Cloud hero alias was not parsed.");
		requireNear(parameters.overhang, 0.27, "Cloud crown alias was not parsed.");
		requireNear(parameters.fineDetail, 0.61, "Cloud micro detail alias was not parsed.");
		requireNear(parameters.multipleScatteringFalloff, 0.81, "Cloud scattering falloff alias was not parsed.");
		requireNear(parameters.multipleScatteringCompensation, 0.63, "Cloud scattering compensation alias was not parsed.");
		const HenyeyGreenstein* phase = dynamic_cast<const HenyeyGreenstein*>(cloud->getMaterial());
		require(phase != nullptr && phase->usesDropletPhase(), "Cloud droplet phase was not enabled.");
		requireNear(phase->getDropletSizeMicrons(), 20.0, "Cloud droplet phase size is wrong.");
		requireVectorNear(parameters.shearDirection, Vector3(0.0, 0.0, 1.0), "Cloud wind direction alias");
		requireVectorNear(parameters.offset, Vector3(4.0, 5.0, 6.0), "Cloud noise offset alias");
		require(parameters.detailOctaves == 5, "Explicit cloud octaves did not override preview quality.");
		require(parameters.maxTrackingSteps == 333, "Explicit cloud tracking steps did not override preview quality.");
		require(CloudVolume::parseType("strato_cumulus") == CloudType::Stratocumulus, "Underscored stratocumulus alias was not parsed.");
		require(CloudVolume::parseType("storm") == CloudType::Cumulonimbus, "Storm cloud alias was not parsed.");
		std::filesystem::remove(scenePath);

		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "cloud cinematic_defaults {\n"
				<< "quality=cinematic\n"
				<< "}\n";
		}
		{
			Scene cinematicScene;
			SceneFile::read(cinematicScene, scenePath.string());
			require(cinematicScene.getHittables().size() == 1, "Cinematic cloud quality scene did not load one hittable.");
			const std::shared_ptr<CloudVolume> cinematicCloud = std::dynamic_pointer_cast<CloudVolume>(cinematicScene.getHittables()[0]);
			require(cinematicCloud != nullptr, "Cinematic cloud quality scene did not create a CloudVolume.");
			require(cinematicCloud->getParameters().detailOctaves == 4, "Cinematic quality did not promote cumulus detail octaves.");
			require(cinematicCloud->getParameters().maxTrackingSteps == 1024, "Cinematic quality did not promote the integration budget.");
		}
		std::filesystem::remove(scenePath);

		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "cloud invalid_quality {\n"
				<< "quality=ultra\n"
				<< "}\n";
		}
		Scene invalidScene;
		requireThrows(
			[&invalidScene, &scenePath]()
			{
				SceneFile::read(invalidScene, scenePath.string());
			},
			"Cloud parser accepted an unknown quality value."
		);
		requireThrows(
			[]()
			{
				CloudVolume::parseType("lenticular");
			},
			"Cloud parser accepted an unknown generation type."
		);
		std::filesystem::remove(scenePath);

		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "cloud malformed_number {\n"
				<< "coverage=0.5oops\n"
				<< "}\n";
		}
		Scene malformedDoubleScene;
		requireThrows(
			[&malformedDoubleScene, &scenePath]()
			{
				SceneFile::read(malformedDoubleScene, scenePath.string());
			},
			"Cloud parser accepted trailing junk in a floating-point value."
		);
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "cloud malformed_integer {\n"
				<< "detail_octaves=4.9\n"
				<< "}\n";
		}
		Scene malformedIntegerScene;
		requireThrows(
			[&malformedIntegerScene, &scenePath]()
			{
				SceneFile::read(malformedIntegerScene, scenePath.string());
			},
			"Cloud parser accepted a fractional integer value."
		);
		std::filesystem::remove(scenePath);
	}

	void	testProceduralCloudDensityAndDeltaTracking(void)
	{
		CloudParameters parameters = CloudVolume::preset(CloudType::Cumulus);
		parameters.position = Vector3(0.0, 0.0, 0.0);
		parameters.size = Vector3(20.0, 10.0, 20.0);
		parameters.featureScale = 4.0;
		parameters.coverage = 1.0;
		parameters.erosion = 0.0;
		parameters.extinction = 100.0;
		parameters.albedo = Color(0.05, 0.06, 0.07);
		parameters.seed = 77;
		CloudVolume cloud(parameters);

		double maximumDensity = 0.0;
		for (int y = -4; y <= 4; y += 2)
			for (int z = -8; z <= 8; z += 2)
				for (int x = -8; x <= 8; x += 2)
				{
					const Vector3 position(x, y, z);
					const double density = cloud.densityAt(position);
					require(density >= 0.0 && density <= 1.0, "Cloud density escaped its majorant range.");
					requireNear(density, cloud.densityAt(position), "Cloud density is not deterministic.");
					maximumDensity = std::max(maximumDensity, density);
				}
		require(maximumDensity > 0.1, "Procedural cloud generated no meaningful density.");
		requireNear(cloud.densityAt(Vector3(100.0, 0.0, 0.0)), 0.0, "Cloud density leaked outside its bounds.");

		Sampler::setRenderSeed(2024);
		Sampler::beginPixelSample(3, 5, 0);
		Ray ray(Vector3(0.0, 0.0, 15.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		require(cloud.hit(ray, hitRecord, T_MIN, T_MAX), "Dense procedural cloud was not delta-tracked.");
		Sampler::endPixelSample();
		require(hitRecord.t0 >= 5.0 && hitRecord.t0 <= 25.0, "Cloud collision was outside its bounds.");
		require(hitRecord.material != nullptr && hitRecord.material->getType() == HENYEY_GREENSTEIN, "Cloud collision did not use its phase function.");

		double stableFeatureT = -1.0;
		for (std::uint32_t sampleIndex = 0; sampleIndex < 3; sampleIndex++)
		{
			Sampler::beginPixelSample(3, 5, sampleIndex);
			Sampler::setBounce(0);
			Sampler::setFeatureSampling(true);
			Ray featureRay(Vector3(0.0, 0.0, 15.0), Vector3(0.0, 0.0, -1.0));
			HitRecord featureHit;
			require(cloud.hit(featureRay, featureHit, T_MIN, T_MAX), "Dense cloud produced no deterministic feature hit.");
			if (sampleIndex == 0)
			{
				stableFeatureT = featureHit.t0;
				require(
					featureHit.u > parameters.albedo.getRed() && featureHit.u <= 1.0,
					"Cloud feature hit stored albedo instead of deterministic density."
				);
			}
			else
				requireNear(featureHit.t0, stableFeatureT, "Cloud feature depth changed with the stochastic sample");
			Sampler::setFeatureSampling(false);
			Sampler::endPixelSample();
		}
	}

	void	testVolumeShadowTransmittance(void)
	{
		const auto boundary = std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0),
			2.0,
			std::make_shared<Lambertian>(Color(0.0, 0.0, 0.0))
		);
		auto volume = std::make_shared<ConstantVolume>(
			boundary,
			std::make_shared<Isotropic>(Color(1.0, 1.0, 1.0)),
			0.5
		);
		Ray volumeRay(Vector3(0.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0));
		const Color exact = volume->shadowTransmittance(volumeRay, T_MIN, T_MAX);
		requireNear(exact.getRed(), std::exp(-2.0), "Constant volume shadow transmittance");

		std::vector<std::shared_ptr<Hittable>> hittables = {volume};
		BVHNode bvh(hittables);
		Ray bvhRay(Vector3(0.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0));
		requireNear(
			bvh.shadowTransmittance(bvhRay, T_MIN, T_MAX).getRed(),
			std::exp(-2.0),
			"BVH volume shadow transmittance"
		);

		CloudParameters parameters = CloudVolume::preset(CloudType::Cumulus);
		parameters.position = Vector3(0.0, 0.0, 0.0);
		parameters.size = Vector3(20.0, 10.0, 20.0);
		parameters.featureScale = 4.0;
		parameters.coverage = 1.0;
		parameters.erosion = 0.0;
		parameters.extinction = 1.0;
		parameters.maxTrackingSteps = 128;
		CloudVolume cloud(parameters);
		CloudParameters invalidCompensation = parameters;
		invalidCompensation.multipleScatteringCompensation = 2.01;
		requireThrows(
			[&invalidCompensation]()
			{
				CloudVolume invalidCloud(invalidCompensation);
			},
			"Cloud accepted an out-of-range multiple-scattering compensation."
		);
		Sampler::beginPixelSample(9, 4, 0);
		Ray cloudRay(Vector3(0.0, 0.0, 15.0), Vector3(0.0, 0.0, -1.0));
		const Color cloudVisibility = cloud.shadowTransmittance(cloudRay, T_MIN, T_MAX);
		Sampler::endPixelSample();
		require(
			cloudVisibility.getRed() >= 0.0 && cloudVisibility.getRed() < 1.0,
			"Cloud shadow transmittance escaped its physical range or ignored cloud density."
		);
		double entryT = 0.0;
		double exitT = 0.0;
		require(
			cloud.integrationInterval(cloudRay, T_MIN, T_MAX, entryT, exitT)
			&& entryT < exitT,
			"Cloud exposed no valid deterministic integration interval."
		);
		const Vector3 samplePosition = cloudRay.pointAtRay((entryT + exitT) * 0.5);
		const double extinction = cloud.extinctionAt(samplePosition);
		require(
			extinction >= 0.0 && extinction <= parameters.extinction,
			"Cloud point extinction escaped its declared majorant."
		);
		const Color scattering = cloud.singleScatteringCoefficientAt(
			samplePosition,
			Vector3(0.0, 0.0, -1.0),
			Vector3(0.0, 1.0, 0.0)
		);
		require(
			scattering.getRed() >= 0.0
			&& scattering.getGreen() >= 0.0
			&& scattering.getBlue() >= 0.0,
			"Cloud point scattering coefficient became negative."
		);
	}

	void	testSceneFileMetersPerUnitScalesVolumeDensity(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_volume_density_scale_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "meters_per_unit=0.5\n\n"
				<< "[scene]\n"
				<< "volume mist {\n"
				<< "shape=sphere\n"
				<< "position=(0,0,0)\n"
				<< "radius=2\n"
				<< "density=2\n"
				<< "color=(1,1,1)\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "Scaled volume scene did not load one hittable.");
		auto volume = std::dynamic_pointer_cast<ConstantVolume>(scene.getHittables()[0]);
		require(volume != nullptr, "Scaled volume block did not create a ConstantVolume.");
		requireNear(volume->getDensity(), 1.0, "Volume density did not convert from inverse meters to inverse scene units.");

		std::filesystem::remove(scenePath);
	}

	void	testCausticPhotonMapBuildsAndEstimates(void)
	{
		Scene scene;
		auto lightMaterial = std::make_shared<Emissive>(Color(25.0, 25.0, 25.0));
		auto glassMaterial = std::make_shared<Dielectric>(Color(1.0, 1.0, 1.0), 1.5);
		auto floorMaterial = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));

		scene.setCausticsEnabled(true);
		scene.setCausticPhotonCount(3000);
		scene.setCausticPassCount(4);
		scene.setCausticRadiusMeters(3.0);
		scene.setCausticAlpha(0.8);
		scene.setMaxLightBounces(6);
		scene.addHittable(std::make_shared<Rectangle>(
			Transform(Vector3(0.0, 5.0, 0.0), Vector3(0.0, -1.0, 0.0), Vector3(1.0, 1.0, 1.0)),
			2.0,
			2.0,
			lightMaterial
		));
		scene.addHittable(std::make_shared<Sphere>(Vector3(0.0, 1.0, 0.0), 2.0, glassMaterial));
		scene.addHittable(std::make_shared<Plane>(-2.5, Vector3(0.0, 1.0, 0.0), floorMaterial));
		scene.updateLights();
		scene.updateAccelerationStructure();

		CausticPhotonMap map;
		map.build(scene, 424242u);
		require(map.photonCount() > 0, "Caustic photon map did not store photons after specular transport.");
		require(map.radiusMeters() < 3.0, "Caustic photon map did not apply progressive radius shrinkage.");
		requireNear(map.radiusSceneUnits(), map.radiusMeters(), "Caustic radius did not match unit scale.");

		HitRecord receiver;
		receiver.position = Vector3(0.0, -2.5, 0.0);
		receiver.normal = Vector3(0.0, 1.0, 0.0);
		receiver.frontFace = true;
		receiver.material = floorMaterial.get();

		Ray cameraRay(Vector3(0.0, 0.0, 4.0), Vector3(0.0, -1.0, -1.0));
		ScatterRecord scatterRecord;
		require(floorMaterial->scatter(cameraRay, receiver, scatterRecord), "Diffuse receiver did not scatter for caustic lookup.");
		const Color estimate = map.estimate(receiver, scatterRecord);
		require(Utilities::luminance(estimate) > 0.0, "Caustic photon map estimate did not contribute radiance.");

		scene.setCausticsEnabled(false);
		CausticPhotonMap disabledMap;
		disabledMap.build(scene, 424242u);
		require(disabledMap.photonCount() == 0, "Disabled caustic photon map still stored photons.");
	}

	void	testSceneFileLoadsNonMetallicPrincipledMaterial(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_principled_material_test.luz";
		const std::filesystem::path objectPath = directory / "luz_principled_material_test.obj";
		{
			std::ofstream objectStream(objectPath);
			objectStream
				<< "v 0.0 0.0 0.0\n"
				<< "v 1.0 0.0 0.0\n"
				<< "v 0.0 1.0 0.0\n"
				<< "f 1 2 3\n";
		}
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "resolution=2,2\n\n"
				<< "[materials]\n"
				<< "material plastic {\n"
				<< "type=principled\n"
				<< "base_color=(0.8,0.8,0.75)\n"
				<< "metallic=0\n"
				<< "roughness=0.35\n"
				<< "transmission=0.25\n"
				<< "ior=1.45\n"
				<< "clearcoat=0.6\n"
				<< "clearcoat_roughness=0.12\n"
				<< "sheen=0.4\n"
				<< "subsurface_profile=skin\n"
				<< "subsurface=0.35\n"
				<< "subsurface_method=thin\n"
				<< "subsurface_radius=(1.2,0.45,0.2)\n"
				<< "subsurface_scale=0.002\n"
				<< "subsurface_color=(0.95,0.5,0.3)\n"
				<< "transmittance=(0.8,0.9,1)\n"
				<< "attenuation_distance=2\n"
				<< "}\n\n"
				<< "[meshes]\n"
				<< "mesh triangle_mesh {\n"
				<< "file=" << objectPath.filename().string() << "\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,3)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "pinhole=1\n"
				<< "focus_distance=3\n"
				<< "}\n"
				<< "object triangle {\n"
				<< "mesh=triangle_mesh\n"
				<< "position=(0,0,-1)\n"
				<< "rotation=(0,0,0)\n"
				<< "scale=(1,1,1)\n"
				<< "material=plastic\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 1, "Principled scene did not load the sphere.");
		Principled* material = dynamic_cast<Principled*>(scene.getHittables()[0]->getMaterial());
		require(material != nullptr, "Non-metallic principled material did not remain principled.");
		requireNear(material->getRoughness(), 0.35, "Principled roughness was not parsed.");
		requireNear(material->getTransmission(), 0.25, "Principled transmission was not parsed.");
		requireNear(material->getRefractiveIndex(), 1.45, "Principled IOR was not parsed.");
		requireNear(material->getClearcoat(), 0.6, "Principled clearcoat was not parsed.");
		requireNear(material->getClearcoatRoughness(), 0.12, "Principled clearcoat roughness was not parsed.");
		requireNear(material->getSheen(), 0.4, "Principled sheen was not parsed.");
		requireNear(material->getSubsurface(), 0.35, "Principled subsurface was not parsed.");
		require(material->getSubsurfaceMethod() == SUBSURFACE_THIN, "Principled subsurface method was not parsed.");
		requireColorNear(material->getSubsurfaceRadius(), Color(1.2, 0.45, 0.2), "Principled subsurface radius was not parsed.");
		requireNear(material->getSubsurfaceScale(), 0.002, "Principled subsurface scale was not parsed.");
		requireColorNear(material->getSubsurfaceColor(), Color(0.95, 0.5, 0.3), "Principled subsurface color was not parsed.");
		const Color coefficient = material->getAbsorptionCoefficient();
		requireNear(coefficient.getRed(), -std::log(0.8) / 2.0, "Principled red transmittance was not parsed.");
		requireNear(coefficient.getGreen(), -std::log(0.9) / 2.0, "Principled green transmittance was not parsed.");
		requireNear(coefficient.getBlue(), 0.0, "Principled blue transmittance was not parsed.");

		std::filesystem::remove(scenePath);
		std::filesystem::remove(objectPath);
	}

	void	testSceneFileLoadsGlossyMaterials(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_glossy_materials_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[materials]\n"
				<< "material pure_gloss {\n"
				<< "type=glossy\n"
				<< "color=(0.8,0.7,0.6)\n"
				<< "roughness=0.2\n"
				<< "}\n"
				<< "material mixed_plastic {\n"
				<< "type=diffuse_glossy\n"
				<< "color=(1,1,1)\n"
				<< "glossy_color=(0.9,0.9,1)\n"
				<< "glossy_weight=0.6\n"
				<< "roughness=0.1\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "objects{\n"
				<< "sphere=(-1,0,-3),0.5,material=pure_gloss\n"
				<< "sphere=(1,0,-3),0.5,material=mixed_plastic\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());

		require(scene.getHittables().size() == 2, "Glossy material scene did not load both objects.");
		Glossy* glossy = dynamic_cast<Glossy*>(scene.getHittables()[0]->getMaterial());
		require(glossy != nullptr, "Glossy material did not parse as Glossy.");
		requireNear(glossy->getRoughness(), 0.2, "Glossy roughness was not parsed.");
		DiffuseGlossy* mixed = dynamic_cast<DiffuseGlossy*>(scene.getHittables()[1]->getMaterial());
		require(mixed != nullptr, "Diffuse-glossy material did not parse as DiffuseGlossy.");
		requireNear(mixed->getGlossyWeight(), 0.6, "Diffuse-glossy weight was not parsed.");
		requireNear(mixed->getGlossyRoughness(), 0.1, "Diffuse-glossy roughness was not parsed.");
		requireColorNear(mixed->getGlossyColor(), Color(0.9, 0.9, 1.0), "Diffuse-glossy color was not parsed.");

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileRejectsInvalidMaterial(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_invalid_material_test.luz";
		{
			std::ofstream stream(scenePath);
			stream
				<< "[settings]\n"
				<< "resolution=2,2\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,1)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "pinhole=1\n"
				<< "focus_distance=1\n"
				<< "}\n"
				<< "objects{\n"
				<< "sphere=(0,0,-1),0.5,material[\n"
				<< "plastic=(1.0,1.0,1.0)\n"
				<< "]\n"
				<< "}\n";
		}

		Scene scene;
		requireThrows(
			[&scene, &scenePath]()
			{
				SceneFile::read(scene, scenePath.string());
			},
			"Scene file accepted an unknown material."
		);

		std::filesystem::remove(scenePath);
	}

	void	testSceneFileRejectsAmbiguousMeasuredData(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_ambiguous_measured_data_test.luz";
		auto requireBadScene = [&scenePath](const std::string& contents, const std::string& message)
		{
			{
				std::ofstream stream(scenePath);
				stream << contents;
			}
			Scene scene;
			requireThrows(
				[&scene, &scenePath]()
				{
					SceneFile::read(scene, scenePath.string());
				},
				message
			);
			std::filesystem::remove(scenePath);
		};

		requireBadScene(
			"[materials]\n"
			"material bad_gold {\n"
			"type=metal\n"
			"preset=gold\n"
			"eta=(0.2,0.3,0.4)\n"
			"k=(2,3,4)\n"
			"}\n\n",
			"Scene file accepted metal preset mixed with explicit eta/k."
		);
		requireBadScene(
			"[materials]\n"
			"material bad_glass {\n"
			"type=dielectric\n"
			"glass_preset=bk7\n"
			"ior=1.4\n"
			"}\n\n",
			"Scene file accepted glass preset mixed with explicit IOR."
		);
		requireBadScene(
			"[scene]\n"
			"volume bad_fog {\n"
			"shape=sphere\n"
			"radius=1\n"
			"preset=fog\n"
			"density=0.1\n"
			"}\n",
			"Scene file accepted volume preset mixed with explicit density."
		);
	}

	void	testBVHReturnsClosestHit(void)
	{
		std::vector<std::shared_ptr<Hittable>> hittables;
		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		hittables.push_back(std::make_shared<Sphere>(Vector3(0.0, 0.0, -5.0), 1.0, material));
		hittables.push_back(std::make_shared<Sphere>(Vector3(0.0, 0.0, -2.0), 0.5, material));

		BVHNode bvh(hittables);
		Ray ray(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;

		require(bvh.hit(ray, hitRecord, 0.001, 100.0), "BVH did not report a hit.");
		requireNear(hitRecord.t0, 1.5, "BVH did not return the closest hit.");
	}

	void	testVolumeHitWorksFromInsideBoundary(void)
	{
		setRandomSeed(7);

		const auto boundary = std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0),
			10.0,
			std::make_shared<Lambertian>(Color(0.0, 0.0, 0.0))
		);
		ConstantVolume volume(
			boundary,
			std::make_shared<Isotropic>(Color(1.0, 1.0, 1.0)),
			1000.0
		);
		Ray ray(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;

		require(volume.hit(ray, hitRecord, T_MIN, T_MAX), "Volume missed when ray started inside its boundary.");
		require(hitRecord.t0 >= T_MIN, "Inside volume hit was behind the ray minimum.");
		require(hitRecord.t0 < 10.0, "Inside volume hit was beyond the boundary exit.");
		require(hitRecord.material != nullptr && hitRecord.material->getType() == ISOTROPIC, "Inside volume hit did not use the phase material.");
	}

	void	testBoxVolumeHitWorksFromInsideBoundary(void)
	{
		setRandomSeed(11);

		const auto boundary = std::make_shared<Cube>(
			Transform(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, -1.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			4.0,
			4.0,
			std::make_shared<Lambertian>(Color(0.0, 0.0, 0.0))
		);
		ConstantVolume volume(
			boundary,
			std::make_shared<Isotropic>(Color(1.0, 1.0, 1.0)),
			1000.0
		);
		Ray ray(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, 1.0));
		HitRecord hitRecord;

		require(volume.hit(ray, hitRecord, T_MIN, T_MAX), "Box volume missed when ray started inside its boundary.");
		require(hitRecord.t0 >= T_MIN, "Inside box volume hit was behind the ray minimum.");
		require(hitRecord.t0 < 2.0, "Inside box volume hit was beyond the boundary exit.");
		require(hitRecord.material != nullptr && hitRecord.material->getType() == ISOTROPIC, "Inside box volume hit did not use the phase material.");
	}

	void	testTinyTriangleHitAndNormal(void)
	{
		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		Triangle triangle(
			Vector3(0.0, 0.0, -1.0),
			Vector3(0.0001, 0.0, -1.0),
			Vector3(0.0, 0.0001, -1.0),
			material
		);
		Ray ray(Vector3(0.000025, 0.000025, 0.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;

		require(triangle.hit(ray, hitRecord, 0.001, 100.0), "Tiny valid triangle was rejected.");
		requireNear(hitRecord.t0, 1.0, "Tiny triangle returned the wrong hit distance.");
		requireNear(Utilities::vectorLength(hitRecord.normal), 1.0, "Triangle hit normal was not normalized.");
		require(hitRecord.normal.getZ() > 0.0, "Triangle hit normal was not oriented against the ray.");
	}

	void	testTrianglePDFAndRandomSampling(void)
	{
		setRandomSeed(123);

		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		Triangle triangle(
			Vector3(0.0, 0.0, -1.0),
			Vector3(1.0, 0.0, -1.0),
			Vector3(0.0, 1.0, -1.0),
			material
		);
		const Vector3 origin(0.25, 0.25, 0.0);
		const Vector3 direction(0.0, 0.0, -1.0);

		requireNear(triangle.area(), 0.5, "Triangle area is wrong.");
		requireNear(triangle.pdfValue(origin, direction), 2.0, "Triangle PDF is wrong.");

		for (int i = 0; i < 8; i++)
		{
			Ray ray(origin, triangle.random(origin));
			HitRecord hitRecord;

			require(triangle.hit(ray, hitRecord, 0.001, 100.0), "Triangle generated a random direction that missed.");
		}
	}

	void	testTriangleInterpolatesVertexNormals(void)
	{
		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		Triangle triangle(
			Vector3(0.0, 0.0, -1.0),
			Vector3(1.0, 0.0, -1.0),
			Vector3(0.0, 1.0, -1.0),
			Vector3(0.0, 1.0, 1.0),
			Vector3(0.0, 1.0, 1.0),
			Vector3(0.0, 1.0, 1.0),
			material
		);
		Ray ray(Vector3(0.25, 0.25, 0.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;

		require(triangle.hit(ray, hitRecord, 0.001, 100.0), "Triangle with vertex normals was not hit.");
		require(hitRecord.normal.getY() > 0.6, "Triangle did not use interpolated vertex normal Y.");
		require(hitRecord.normal.getZ() > 0.6, "Triangle did not use interpolated vertex normal Z.");
		requireVectorNear(hitRecord.geometricNormal, Vector3(0.0, 0.0, 1.0), "Triangle geometric normal");
	}

	void	testOpaqueSmoothNormalDoesNotReceiveBacksideLight(void)
	{
		auto material = std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0));
		auto light = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
		Scene scene;

		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.0, 0.0, 0.0));
		scene.setMaxLightBounces(1);
		scene.addHittable(std::make_shared<Triangle>(
			Vector3(-1.0, -1.0, 0.0),
			Vector3(1.0, -1.0, 0.0),
			Vector3(0.0, 1.0, 0.0),
			Vector3(0.0, 1.0, 0.0),
			Vector3(0.0, 1.0, 0.0),
			Vector3(0.0, 1.0, 0.0),
			material
		));
		scene.addHittable(std::make_shared<DirectionalLight>(Vector3(0.0, -1.0, 0.0), light));
		scene.updateLights();

		Ray ray(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0));
		const Color color = Renderer::internal::_calculateLightRaysColor(ray, scene);

		requireColorNear(color, Color(0.0, 0.0, 0.0), "Opaque smooth normal accepted backside light");
	}

	void	testOBJReaderLoadsVertexNormals(void)
	{
		const std::filesystem::path objectPath = std::filesystem::temp_directory_path() / "luz_obj_normals_test.obj";
		{
			std::ofstream objectStream(objectPath);
			objectStream
				<< "v 0.0 0.0 -1.0\n"
				<< "v 1.0 0.0 -1.0\n"
				<< "v 0.0 1.0 -1.0\n"
				<< "vn 0.0 1.0 1.0\n"
				<< "vn 0.0 1.0 1.0\n"
				<< "vn 0.0 1.0 1.0\n"
				<< "f 1//1 2//2 3//3\n";
		}

		ObjReadOptions options;
		options.quiet = true;
		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		Mesh mesh = readObj(
			objectPath.string(),
			Vector3(0.0, 0.0, 0.0),
			Vector3(0.0, 0.0, 0.0),
			Vector3(1.0, 1.0, 1.0),
			material,
			options
		);
		Ray ray(Vector3(0.25, 0.25, 0.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;

		require(mesh.hit(ray, hitRecord, 0.001, 100.0), "OBJ mesh with vertex normals was not hit.");
		require(hitRecord.normal.getY() > 0.6, "OBJ reader did not preserve vertex normal Y.");
		require(hitRecord.normal.getZ() > 0.6, "OBJ reader did not preserve vertex normal Z.");

		std::filesystem::remove(objectPath);
	}

	void	testSceneFileLoadsTexturedOBJUVs(void)
	{
		const std::filesystem::path directory = std::filesystem::temp_directory_path();
		const std::filesystem::path scenePath = directory / "luz_texture_uv_test.luz";
		const std::filesystem::path objectPath = directory / "luz_texture_uv_test.obj";
		const std::filesystem::path texturePath = directory / "luz_texture_uv_test.ppm";
		{
			std::ofstream textureStream(texturePath);
			textureStream
				<< "P3\n"
				<< "1 1\n"
				<< "255\n"
				<< "255 0 0\n";
		}
		{
			std::ofstream objectStream(objectPath);
			objectStream
				<< "v 0.0 0.0 -1.0\n"
				<< "v 1.0 0.0 -1.0\n"
				<< "v 0.0 1.0 -1.0\n"
				<< "vt 0.0 0.0\n"
				<< "vt 1.0 0.0\n"
				<< "vt 0.0 1.0\n"
				<< "f 1/1 2/2 3/3\n";
		}
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "resolution=2,2\n\n"
				<< "[materials]\n"
				<< "material textured {\n"
				<< "type=lambertian\n"
				<< "color=(1,1,1)\n"
				<< "texture=" << texturePath.filename().string() << "\n"
				<< "}\n\n"
				<< "[meshes]\n"
				<< "mesh triangle_mesh {\n"
				<< "file=" << objectPath.filename().string() << "\n"
				<< "}\n\n"
				<< "[scene]\n"
				<< "camera main {\n"
				<< "position=(0,0,2)\n"
				<< "direction=(0,0,-1)\n"
				<< "focal_length_mm=50\n"
				<< "sensor_width_mm=36\n"
				<< "sensor_height_mm=20.25\n"
				<< "pinhole=1\n"
				<< "focus_distance=2\n"
				<< "}\n"
				<< "object triangle {\n"
				<< "mesh=triangle_mesh\n"
				<< "position=(0,0,0)\n"
				<< "rotation=(0,0,0)\n"
				<< "scale=(1,1,1)\n"
				<< "material=textured\n"
				<< "}\n";
		}

		Scene scene;
		SceneFile::read(scene, scenePath.string());
		Ray ray(Vector3(0.25, 0.25, 0.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;

		require(scene.getHittables()[0]->hit(ray, hitRecord, 0.001, 100.0), "Textured OBJ triangle was not hit.");
		requireNear(hitRecord.u, 0.25, "OBJ reader did not interpolate texture coordinate U.");
		requireNear(hitRecord.v, 0.25, "OBJ reader did not interpolate texture coordinate V.");
		const Color texturedColor = hitRecord.material->colorAt(hitRecord);
		requireColorNear(
			texturedColor,
			ColorManagement::acescgFromSRGB(Color(1.0, 0.0, 0.0)),
			"OBJ texture did not convert sRGB input to ACEScg."
		);

		std::filesystem::remove(scenePath);
		std::filesystem::remove(objectPath);
		std::filesystem::remove(texturePath);
	}

	void	testMeshInstanceSharesGeometryAndTransformsHits(void)
	{
		auto geometryMaterial = std::make_shared<Emissive>(Color(1.0, 1.0, 1.0));
		std::vector<Triangle> triangles;
		triangles.emplace_back(
			Vector3(0.0, 0.0, 0.0),
			Vector3(1.0, 0.0, 0.0),
			Vector3(0.0, 1.0, 0.0),
			geometryMaterial
		);

		auto geometry = std::make_shared<Mesh>(Vector3(), geometryMaterial, std::move(triangles));
		auto lambertian = std::make_shared<Lambertian>(Color(0.8, 0.2, 0.1));
		auto metal = std::make_shared<Metal>(Color(0.8, 0.7, 0.6), 0.1);
		MeshInstance instanceA(
			geometry,
			Vector3(1.0, 2.0, 3.0),
			Vector3(0.0, 0.0, 90.0),
			Vector3(2.0, 1.0, 1.0),
			lambertian
		);
		MeshInstance instanceB(
			geometry,
			Vector3(-1.0, 0.0, 0.0),
			Vector3(0.0, 0.0, 0.0),
			Vector3(1.0, 1.0, 1.0),
			metal
		);

		require(
			instanceA.getGeometry().get() == instanceB.getGeometry().get(),
			"Mesh instances did not share geometry."
		);

		Ray ray(Vector3(0.75, 2.5, 4.0), Vector3(0.0, 0.0, -1.0));
		HitRecord hitRecord;
		require(instanceA.hit(ray, hitRecord, 0.001, 100.0), "Transformed mesh instance was not hit.");
		require(hitRecord.material == lambertian.get(), "Mesh instance did not override geometry material.");
		requireNear(hitRecord.t0, 1.0, "Mesh instance hit distance was wrong.");
		requireVectorNear(hitRecord.position, Vector3(0.75, 2.5, 3.0), "Mesh instance hit position was wrong.");
		requireNear(
			instanceA.pdfValue(ray.getOrigin(), ray.getDirection()),
			1.0,
			"Mesh instance PDF used the wrong world area."
		);
	}

	void	testMeshPDFAndRandomSampling(void)
	{
		setRandomSeed(123);

		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		std::vector<std::shared_ptr<Hittable>> triangles;
		triangles.push_back(std::make_shared<Triangle>(
			Vector3(0.0, 0.0, -1.0),
			Vector3(1.0, 0.0, -1.0),
			Vector3(0.0, 1.0, -1.0),
			material
		));
		triangles.push_back(std::make_shared<Triangle>(
			Vector3(2.0, 0.0, -1.0),
			Vector3(4.0, 0.0, -1.0),
			Vector3(2.0, 1.0, -1.0),
			material
		));
		Mesh mesh(Vector3(), material, triangles);
		const Vector3 origin(0.25, 0.25, 0.0);
		const Vector3 direction(0.0, 0.0, -1.0);

		requireNear(mesh.pdfValue(origin, direction), 2.0 / 3.0, "Mesh PDF did not use total mesh area.");

		for (int i = 0; i < 8; i++)
		{
			Ray ray(origin, mesh.random(origin));
			HitRecord hitRecord;

			require(mesh.hit(ray, hitRecord, 0.001, 100.0), "Mesh generated a random direction that missed.");
		}
	}

	void	testAABBDefaultBounds(void)
	{
		const AABB boundingBox;

		requireVectorNear(boundingBox.getMinimum(), Vector3(0.0, 0.0, 0.0), "Default AABB minimum");
		requireVectorNear(boundingBox.getMaximum(), Vector3(0.0, 0.0, 0.0), "Default AABB maximum");
	}

	void	testAABBHandlesAxisParallelBoundaryRays(void)
	{
		const AABB boundingBox(Vector3(0.0, 0.0, -2.0), Vector3(1.0, 1.0, -1.0));
		HitRecord hitRecord;
		Ray boundaryRay(Vector3(0.0, 0.5, 0.0), Vector3(0.0, 0.0, -1.0));
		Ray outsideRay(Vector3(2.0, 0.5, 0.0), Vector3(0.0, 0.0, -1.0));
		Ray zeroDirectionRay(Vector3(0.5, 0.5, -1.5), Vector3(0.0, 0.0, 0.0));

		require(boundingBox.hit(boundaryRay, hitRecord, 100.0), "Axis-parallel boundary ray missed the AABB.");
		require(!boundingBox.hit(outsideRay, hitRecord, 100.0), "Axis-parallel outside ray hit the AABB.");
		require(!boundingBox.hit(zeroDirectionRay, hitRecord, 100.0), "Zero-direction ray hit the AABB.");
	}

	void	testRectangleBoundingBoxes(void)
	{
		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		AABB boundingBox;

		Rectangle yRectangle(
			Transform(Vector3(1.0, 2.0, 3.0), Vector3(0.0, 1.0, 0.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			6.0,
			material
		);
		require(yRectangle.createBoundingBox(boundingBox), "Y-aligned rectangle did not create a bounding box.");
		requireVectorNear(boundingBox.getMinimum(), Vector3(-1.0, 2.0 - T_MIN, 0.0), "Y-aligned rectangle minimum");
		requireVectorNear(boundingBox.getMaximum(), Vector3(3.0, 2.0 + T_MIN, 6.0), "Y-aligned rectangle maximum");

		Rectangle customAxesYRectangle(
			Transform(Vector3(1.0, 2.0, 3.0), Vector3(0.0, 1.0, 0.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			2.0,
			material
		);
		customAxesYRectangle.setAxes(Vector3(0.0, 0.0, 1.0), Vector3(1.0, 0.0, 0.0));
		require(customAxesYRectangle.createBoundingBox(boundingBox), "Custom-axis Y rectangle did not create a bounding box.");
		requireVectorNear(boundingBox.getMinimum(), Vector3(0.0, 2.0 - T_MIN, 1.0), "Custom-axis Y rectangle minimum");
		requireVectorNear(boundingBox.getMaximum(), Vector3(2.0, 2.0 + T_MIN, 5.0), "Custom-axis Y rectangle maximum");

		Rectangle zRectangle(
			Transform(Vector3(1.0, 2.0, 3.0), Vector3(0.0, 0.0, 1.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			6.0,
			material
		);
		require(zRectangle.createBoundingBox(boundingBox), "Z-aligned rectangle did not create a bounding box.");
		requireVectorNear(boundingBox.getMinimum(), Vector3(-1.0, -1.0, 3.0 - T_MIN), "Z-aligned rectangle minimum");
		requireVectorNear(boundingBox.getMaximum(), Vector3(3.0, 5.0, 3.0 + T_MIN), "Z-aligned rectangle maximum");

		Rectangle xRectangle(
			Transform(Vector3(1.0, 2.0, 3.0), Vector3(1.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			6.0,
			material
		);
		require(xRectangle.createBoundingBox(boundingBox), "X-aligned rectangle did not create a bounding box.");
		requireVectorNear(boundingBox.getMinimum(), Vector3(1.0 - T_MIN, -1.0, 1.0), "X-aligned rectangle minimum");
		requireVectorNear(boundingBox.getMaximum(), Vector3(1.0 + T_MIN, 5.0, 5.0), "X-aligned rectangle maximum");

		Rectangle zeroNormalRectangle(
			Transform(Vector3(1.0, 2.0, 3.0), Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			6.0,
			material
		);
		require(!zeroNormalRectangle.createBoundingBox(boundingBox), "Zero-normal rectangle created a bounding box.");
	}

	void	requireRectangleRandomSamplesHit(Rectangle& rectangle, const Vector3& origin, const std::string& message)
	{
		for (int i = 0; i < 8; i++)
		{
			Ray ray(origin, rectangle.random(origin));
			HitRecord hitRecord;

			require(rectangle.hit(ray, hitRecord, 0.001, 100.0), message);
		}
	}

	void	testRectangleRandomSamplesSupportedAxes(void)
	{
		setRandomSeed(123);

		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		Rectangle yRectangle(
			Transform(Vector3(0.0, 5.0, 0.0), Vector3(0.0, -1.0, 0.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			6.0,
			material
		);
		Rectangle zRectangle(
			Transform(Vector3(0.0, 0.0, 5.0), Vector3(0.0, 0.0, -1.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			6.0,
			material
		);
		Rectangle xRectangle(
			Transform(Vector3(5.0, 0.0, 0.0), Vector3(-1.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			6.0,
			material
		);

		requireRectangleRandomSamplesHit(yRectangle, Vector3(0.0, 0.0, 0.0), "Y-aligned rectangle generated a direction that missed.");
		requireRectangleRandomSamplesHit(zRectangle, Vector3(0.0, 0.0, 0.0), "Z-aligned rectangle generated a direction that missed.");
		requireRectangleRandomSamplesHit(xRectangle, Vector3(0.0, 0.0, 0.0), "X-aligned rectangle generated a direction that missed.");
	}

	void	testRectangleTextureCoordinates(void)
	{
		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		Rectangle defaultUVRectangle(
			Transform(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, 1.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			2.0,
			material
		);
		Ray defaultRay(Vector3(1.0, 0.5, 2.0), Vector3(0.0, 0.0, -1.0));
		HitRecord defaultHit;
		require(defaultUVRectangle.hit(defaultRay, defaultHit, 0.001, 100.0), "Rectangle with default UVs was not hit.");
		requireNear(defaultHit.u, 0.75, "Rectangle default U coordinate is wrong.");
		requireNear(defaultHit.v, 0.75, "Rectangle default V coordinate is wrong.");

		Rectangle customUVRectangle(
			Transform(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 0.0, 1.0), Vector3(1.0, 1.0, 1.0)),
			2.0,
			2.0,
			material
		);
		customUVRectangle.setTextureCoordinates(
			Vector3(0.1, 0.2, 0.0),
			Vector3(0.9, 0.2, 0.0),
			Vector3(0.7, 0.8, 0.0),
			Vector3(0.3, 0.8, 0.0)
		);
		Ray centerRay(Vector3(0.0, 0.0, 2.0), Vector3(0.0, 0.0, -1.0));
		HitRecord centerHit;
		require(customUVRectangle.hit(centerRay, centerHit, 0.001, 100.0), "Rectangle with custom UVs was not hit.");
		requireNear(centerHit.u, 0.5, "Rectangle custom U coordinate was not bilinear-interpolated.");
		requireNear(centerHit.v, 0.5, "Rectangle custom V coordinate was not bilinear-interpolated.");

		Rectangle customAxesRectangle(
			Transform(Vector3(0.0, 0.0, 0.0), Vector3(0.0, 1.0, 0.0), Vector3(1.0, 1.0, 1.0)),
			4.0,
			2.0,
			material
		);
		customAxesRectangle.setAxes(Vector3(0.0, 0.0, 1.0), Vector3(1.0, 0.0, 0.0));
		Ray customAxesRay(Vector3(0.5, 2.0, 1.0), Vector3(0.0, -1.0, 0.0));
		HitRecord customAxesHit;
		require(customAxesRectangle.hit(customAxesRay, customAxesHit, 0.001, 100.0), "Rectangle with custom axes was not hit.");
		requireNear(customAxesHit.u, 0.75, "Rectangle custom-axis U coordinate is wrong.");
		requireNear(customAxesHit.v, 0.75, "Rectangle custom-axis V coordinate is wrong.");
	}

	void	testCubeBoundingBoxAndSetters(void)
	{
		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		Cube cube;
		cube.setTransform(Transform(Vector3(10.0, 20.0, 30.0), Vector3(0.0, 0.0, 1.0), Vector3(1.0, 1.0, 1.0)));
		cube.setWidth(4.0);
		cube.setHeight(6.0);
		cube.setDepth(8.0);
		cube.setMaterial(material);

		AABB boundingBox;
		require(cube.createBoundingBox(boundingBox), "Cube did not create a bounding box.");
		requireVectorNear(boundingBox.getMinimum(), Vector3(8.0, 17.0, 26.0), "Cube minimum");
		requireVectorNear(boundingBox.getMaximum(), Vector3(12.0, 23.0, 34.0), "Cube maximum");

		HitRecord hitRecord;
		Ray backFaceRay(Vector3(10.0, 20.0, 40.0), Vector3(0.0, 0.0, -1.0));
		require(cube.hit(backFaceRay, hitRecord, 0.001, 100.0), "Cube setters did not regenerate back face geometry.");
		requireNear(hitRecord.t0, 6.0, "Cube back face hit distance is wrong.");

		Ray rightFaceRay(Vector3(20.0, 20.0, 34.0), Vector3(-1.0, 0.0, 0.0));
		require(cube.hit(rightFaceRay, hitRecord, 0.001, 100.0), "Cube side face did not span the cube depth.");
		requireNear(hitRecord.t0, 8.0, "Cube side face hit distance is wrong.");
	}

	void	testHittablePDFAveragesMultipleLights(void)
	{
		auto material = std::make_shared<Lambertian>(Color(0.8, 0.8, 0.8));
		auto visibleLight = std::make_shared<Sphere>(Vector3(0.0, 0.0, -2.0), 0.5, material);
		auto offAxisLight = std::make_shared<Sphere>(Vector3(10.0, 0.0, -2.0), 0.5, material);
		const Vector3 origin(0.0, 0.0, 0.0);
		const Vector3 direction(0.0, 0.0, -1.0);

		HittablePDF singleLightPDF(visibleLight, origin);
		HittablePDF multiLightPDF(
			std::vector<std::shared_ptr<Hittable>>{visibleLight, offAxisLight},
			origin
		);

		requireNear(
			multiLightPDF.value(direction),
			singleLightPDF.value(direction) / 2.0,
			"Multi-light HittablePDF did not average light densities."
		);
	}

	void	requireFlagParseThrows(std::vector<std::string> arguments, const std::string& message)
	{
		std::vector<char*> argv;
		argv.push_back(const_cast<char*>("luz"));
		for (std::string& argument : arguments)
		{
			argv.push_back(argument.data());
		}

		requireThrows([&]()
		{
			Scene scene;
			FlagsParser(static_cast<int>(argv.size()), argv.data()).parse(scene);
		}, message);
	}

	std::unique_ptr<Scene>	parseFlags(std::vector<std::string> arguments)
	{
		std::vector<char*> argv;
		argv.push_back(const_cast<char*>("luz"));
		for (std::string& argument : arguments)
		{
			argv.push_back(argument.data());
		}

		auto scene = std::make_unique<Scene>();
		FlagsParser(static_cast<int>(argv.size()), argv.data()).parse(*scene);
		return (scene);
	}

	void	testFlagsParseBenchmarkFileOptions(void)
	{
		std::unique_ptr<Scene> bounceScene = parseFlags({"--max-light-bounces", "5"});
		require(bounceScene->getMaxLightBounces() == 5, "--max-light-bounces was not parsed.");

		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_cli_file_benchmark_test.luz";
		std::ofstream sceneFile(scenePath);

		sceneFile
			<< "[settings]\n"
			<< "resolution=3,2\n"
			<< "samples=7\n"
			<< "maxlightbounces=4\n\n";
		sceneFile.close();

		std::unique_ptr<Scene> fileBenchmarkScene = parseFlags({"--file", scenePath.string(), "--benchmark"});
		require(fileBenchmarkScene->getBenchmarkMode(), "--benchmark did not enable benchmark mode for a file scene.");
		require(fileBenchmarkScene->getSampleCount() == 7, "--benchmark replaced the loaded file scene samples.");
		require(fileBenchmarkScene->getMaxLightBounces() == 4, "--benchmark replaced the loaded file scene bounces.");
		require(fileBenchmarkScene->getImage()->getWidth() == 3, "--benchmark replaced the loaded file scene width.");
		require(fileBenchmarkScene->getImage()->getHeight() == 2, "--benchmark replaced the loaded file scene height.");

		std::unique_ptr<Scene> defaultBenchmarkScene = parseFlags({"--benchmark"});
		require(defaultBenchmarkScene->getRenderSky() == SKY_NONE, "Default benchmark should use the cheap black background.");

		std::unique_ptr<Scene> atmosphereBenchmarkScene = parseFlags({"--benchmark", "--benchmark-case", "atmosphere"});
		require(atmosphereBenchmarkScene->getRenderSky() == SKY_ATMOSPHERE, "Atmosphere benchmark disabled atmospheric sky.");

		std::filesystem::remove(scenePath);
	}

	void	testFlagsParsePositionalSceneFile(void)
	{
		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_cli_positional_file_test.luz";
		std::ofstream sceneFile(scenePath);

		sceneFile
			<< "[settings]\n"
			<< "resolution=3,2\n"
			<< "samples=7\n"
			<< "maxlightbounces=4\n\n";
		sceneFile.close();

		std::unique_ptr<Scene> scene = parseFlags({scenePath.string()});
		require(scene->getIsFromFile(), "Positional scene path was not loaded.");
		require(scene->getSampleCount() == 7, "Positional scene path did not load scene samples.");
		require(scene->getMaxLightBounces() == 4, "Positional scene path did not load scene bounces.");
		require(scene->getImage()->getWidth() == 3, "Positional scene path did not load scene width.");
		require(scene->getImage()->getHeight() == 2, "Positional scene path did not load scene height.");

		std::unique_ptr<Scene> overrideScene = parseFlags({
			scenePath.string(),
			"--samples", "9",
			"--resolution", "4x5"
		});
		require(overrideScene->getSampleCount() == 9, "CLI samples did not override positional scene file.");
		require(overrideScene->getImage()->getWidth() == 4, "CLI width did not override positional scene file.");
		require(overrideScene->getImage()->getHeight() == 5, "CLI height did not override positional scene file.");

		std::filesystem::remove(scenePath);
	}

	void	testFlagsParsePostProcessOptions(void)
	{
		std::unique_ptr<Scene> scene = parseFlags({
			"--view-transform", "raw",
			"--bloom", "false",
			"--exposure", "1.25",
			"--contrast", "1.5"
		});

		require(scene->getViewTransform() == ViewTransform::Raw, "--view-transform raw was not parsed.");
		require(!scene->getBloom(), "--bloom false was not parsed.");
		requireNear(scene->getExposure(), 1.25, "--exposure was not parsed.");
		requireNear(scene->getContrast(), 1.5, "--contrast was not parsed.");
	}

	void	testFlagsParseDenoiseOptions(void)
	{
		std::unique_ptr<Scene> enabledScene = parseFlags({"--denoise"});
		require(enabledScene->getDenoise(), "Bare --denoise did not enable denoising.");

		std::unique_ptr<Scene> falseScene = parseFlags({"--denoise", "false"});
		require(!falseScene->getDenoise(), "--denoise false did not disable denoising.");

		std::unique_ptr<Scene> disabledScene = parseFlags({"--denoise", "--no-denoise"});
		require(!disabledScene->getDenoise(), "--no-denoise did not override --denoise.");

		std::unique_ptr<Scene> reenabledScene = parseFlags({"--no-denoise", "--denoise"});
		require(reenabledScene->getDenoise(), "Later --denoise did not override --no-denoise.");

		std::unique_ptr<Scene> outputScene = parseFlags({"--denoise-output", "custom_denoised.tiff"});
		require(
			outputScene->getDenoiseOutputFileName() == "custom_denoised.tiff",
			"--denoise-output was not parsed."
		);
		std::unique_ptr<Scene> rawOutputScene = parseFlags({"--output", "custom_render.tiff"});
		require(
			rawOutputScene->getDefaultRenderOutputFileName() == "custom_render.tiff",
			"--output was not parsed."
		);
		std::unique_ptr<Scene> shortOutputScene = parseFlags({"-o", "short_render.bmp"});
		require(
			shortOutputScene->getDefaultRenderOutputFileName() == "short_render.bmp",
			"-o was not parsed."
		);

		const std::filesystem::path scenePath = std::filesystem::temp_directory_path() / "luz_cli_denoise_override_test.luz";
		std::ofstream sceneFile(scenePath);

		sceneFile
			<< "[settings]\n"
			<< "resolution=1,1\n"
			<< "denoise=0\n\n";
		sceneFile.close();

		std::unique_ptr<Scene> fileOverrideScene = parseFlags({"--file", scenePath.string(), "--denoise"});
		require(fileOverrideScene->getDenoise(), "--denoise did not override scene file denoise setting.");
	}

	void	testFlagsParseCausticOptions(void)
	{
		std::unique_ptr<Scene> enabledScene = parseFlags({"--caustics"});
		require(enabledScene->getCausticsEnabled(), "Bare --caustics did not enable caustics.");

		std::unique_ptr<Scene> falseScene = parseFlags({"--caustics", "false"});
		require(!falseScene->getCausticsEnabled(), "--caustics false did not disable caustics.");

		std::unique_ptr<Scene> disabledScene = parseFlags({"--caustics", "--no-caustics"});
		require(!disabledScene->getCausticsEnabled(), "--no-caustics did not override --caustics.");

		std::unique_ptr<Scene> tunedScene = parseFlags({
			"--caustics",
			"--caustic-photons", "1234",
			"--caustic-passes", "6",
			"--caustic-radius", "0.125",
			"--caustic-alpha", "0.8"
		});
		require(tunedScene->getCausticsEnabled(), "Caustics were not enabled.");
		require(tunedScene->getCausticPhotonCount() == 1234, "--caustic-photons was not parsed.");
		require(tunedScene->getCausticPassCount() == 6, "--caustic-passes was not parsed.");
		requireNear(tunedScene->getCausticRadiusMeters(), 0.125, "--caustic-radius was not parsed.");
		requireNear(tunedScene->getCausticAlpha(), 0.8, "--caustic-alpha was not parsed.");
	}

	void	testFlagsParsePNGOutput(void)
	{
		std::unique_ptr<Scene> outputScene = parseFlags({"--output", "custom_render.png"});
		require(
			outputScene->getDefaultRenderOutputFileName() == "custom_render.png",
			"PNG --output was not parsed."
		);
	}

	void	testFlagsParseAdaptiveOptions(void)
	{
		std::unique_ptr<Scene> enabledScene = parseFlags({"--adaptive"});
		require(enabledScene->getAdaptiveSampling(), "Bare --adaptive did not enable adaptive sampling.");

		std::unique_ptr<Scene> falseScene = parseFlags({"--adaptive", "false"});
		require(!falseScene->getAdaptiveSampling(), "--adaptive false did not disable adaptive sampling.");

		std::unique_ptr<Scene> disabledScene = parseFlags({"--adaptive", "--no-adaptive"});
		require(!disabledScene->getAdaptiveSampling(), "--no-adaptive did not override --adaptive.");

		std::unique_ptr<Scene> tunedScene = parseFlags({
			"--adaptive",
			"--adaptive-min-samples", "24",
			"--adaptive-background-min-samples", "6",
			"--adaptive-volume-min-samples", "40",
			"--adaptive-threshold", "0.03",
			"--adaptive-check-interval", "6"
		});
		require(tunedScene->getAdaptiveSampling(), "Adaptive sampling was not enabled.");
		require(tunedScene->getAdaptiveMinSamples() == 24, "--adaptive-min-samples was not parsed.");
		require(
			tunedScene->getAdaptiveBackgroundMinSamples() == 6,
			"--adaptive-background-min-samples was not parsed."
		);
		require(
			tunedScene->getAdaptiveVolumeMinSamples() == 40,
			"--adaptive-volume-min-samples was not parsed."
		);
		requireNear(tunedScene->getAdaptiveThreshold(), 0.03, "--adaptive-threshold was not parsed.");
		require(tunedScene->getAdaptiveCheckInterval() == 6, "--adaptive-check-interval was not parsed.");
	}

	void	testFlagsRejectInvalidValues(void)
	{
		requireFlagParseThrows({"--samples", "0"}, "CLI accepted zero samples.");
		requireFlagParseThrows({"--adaptive", "maybe"}, "CLI accepted invalid adaptive value.");
		requireFlagParseThrows({"--adaptive-min-samples", "0"}, "CLI accepted zero adaptive minimum samples.");
		requireFlagParseThrows(
			{"--adaptive-background-min-samples", "-1"},
			"CLI accepted negative adaptive background minimum samples."
		);
		requireFlagParseThrows(
			{"--adaptive-volume-min-samples", "-1"},
			"CLI accepted negative adaptive volume minimum samples."
		);
		requireFlagParseThrows({"--adaptive-threshold", "0"}, "CLI accepted zero adaptive threshold.");
		requireFlagParseThrows({"--adaptive-check-interval", "0"}, "CLI accepted zero adaptive check interval.");
		requireFlagParseThrows({"--maxLightBounces", "-1"}, "CLI accepted negative max light bounces.");
		requireFlagParseThrows({"--resolution", "-1x100"}, "CLI accepted negative width.");
		requireFlagParseThrows({"--threads", "0"}, "CLI accepted zero threads.");
		requireFlagParseThrows({"--view-transform"}, "CLI accepted missing view transform.");
		requireFlagParseThrows({"--view-transform", "filmic"}, "CLI accepted invalid view transform.");
		requireFlagParseThrows({"--gamma", "false"}, "CLI accepted removed gamma flag.");
		requireFlagParseThrows({"--tonemapping", "false"}, "CLI accepted removed tone mapping flag.");
		requireFlagParseThrows({"--exposure"}, "CLI accepted missing exposure.");
		requireFlagParseThrows({"--exposure", "nan"}, "CLI accepted non-finite exposure.");
		requireFlagParseThrows({"--contrast"}, "CLI accepted missing contrast.");
		requireFlagParseThrows({"--contrast", "-1"}, "CLI accepted negative contrast.");
		requireFlagParseThrows({"--caustics", "maybe"}, "CLI accepted invalid caustics value.");
		requireFlagParseThrows({"--caustic-photons"}, "CLI accepted missing caustic photon count.");
		requireFlagParseThrows({"--caustic-photons", "-1"}, "CLI accepted negative caustic photon count.");
		requireFlagParseThrows({"--caustic-passes", "0"}, "CLI accepted zero caustic pass count.");
		requireFlagParseThrows({"--caustic-radius", "0"}, "CLI accepted zero caustic radius.");
		requireFlagParseThrows({"--caustic-alpha", "1.2"}, "CLI accepted caustic alpha above one.");
		requireFlagParseThrows({"--denoise", "maybe"}, "CLI accepted invalid denoise value.");
		requireFlagParseThrows({"--compression", "75"}, "CLI accepted removed compression flag.");
		requireFlagParseThrows({"--compression=75"}, "CLI accepted removed compression assignment.");
		requireFlagParseThrows({"--output-file", "render.tiff"}, "CLI accepted removed output-file flag.");
		requireFlagParseThrows({"--output-file=render.tiff"}, "CLI accepted removed output-file assignment.");
		requireFlagParseThrows({"--output"}, "CLI accepted missing output path.");
		requireFlagParseThrows({"--output", "tiff"}, "CLI accepted output format instead of output path.");
		requireFlagParseThrows({"--output", "render"}, "CLI accepted output path without an extension.");
		requireFlagParseThrows({"--output", "render.tif"}, "CLI accepted .tif output extension.");
		requireFlagParseThrows({"--output", "render.jpg"}, "CLI accepted unsupported output extension.");
		requireFlagParseThrows({"--denoise-output"}, "CLI accepted missing denoise output path.");
		requireFlagParseThrows({"--denoise-output", "render"}, "CLI accepted denoise output path without an extension.");
		requireFlagParseThrows({"--denoise-output", "render_denoised.tif"}, "CLI accepted .tif denoise output extension.");
		requireFlagParseThrows({"--denoise-output", "render.jpg"}, "CLI accepted unsupported denoise output extension.");
	}

	void	testSettersRejectInvalidValues(void)
	{
		Scene scene;
		Image image;

		requireThrows([&]() { scene.setSampleCount(0); }, "Scene accepted zero samples.");
		requireThrows([&]() { scene.setAdaptiveMinSamples(0); }, "Scene accepted zero adaptive minimum samples.");
		requireThrows(
			[&]() { scene.setAdaptiveBackgroundMinSamples(-1); },
			"Scene accepted negative adaptive background minimum samples."
		);
		requireThrows(
			[&]() { scene.setAdaptiveVolumeMinSamples(-1); },
			"Scene accepted negative adaptive volume minimum samples."
		);
		requireThrows([&]() { scene.setAdaptiveThreshold(0.0); }, "Scene accepted zero adaptive threshold.");
		requireThrows([&]() { scene.setAdaptiveCheckInterval(0); }, "Scene accepted zero adaptive check interval.");
		requireThrows([&]() { scene.setMaxLightBounces(-1); }, "Scene accepted negative max light bounces.");
		requireThrows([&]() { scene.setRenderingThreads(0); }, "Scene accepted zero rendering threads.");
		requireThrows([&]() { scene.setMetersPerUnit(0.0); }, "Scene accepted zero meters per unit.");
		requireThrows([&]() { scene.setMetersPerUnit(std::numeric_limits<double>::quiet_NaN()); }, "Scene accepted non-finite meters per unit.");
		requireThrows([&]() { scene.setExposure(std::numeric_limits<double>::quiet_NaN()); }, "Scene accepted non-finite exposure.");
		requireThrows([&]() { scene.setPhotographicExposure(0.0, 1.0, 100.0); }, "Scene accepted zero photographic f-number.");
		requireThrows([&]() { scene.setPhotographicExposure(2.0, 0.0, 100.0); }, "Scene accepted zero photographic shutter.");
		requireThrows([&]() { scene.setPhotographicExposure(2.0, 1.0, 0.0); }, "Scene accepted zero photographic ISO.");
		requireThrows([&]() { scene.setPhotographicExposure(std::numeric_limits<double>::quiet_NaN(), 1.0, 100.0); }, "Scene accepted non-finite photographic f-number.");
		requireThrows([&]() { scene.setContrast(-1.0); }, "Scene accepted negative contrast.");
		requireThrows([&]() { scene.setContrast(std::numeric_limits<double>::quiet_NaN()); }, "Scene accepted non-finite contrast.");
		requireThrows([&]() { scene.setCausticPhotonCount(-1); }, "Scene accepted negative caustic photon count.");
		requireThrows([&]() { scene.setCausticPassCount(0); }, "Scene accepted zero caustic pass count.");
		requireThrows([&]() { scene.setCausticRadiusMeters(0.0); }, "Scene accepted zero caustic radius.");
		requireThrows([&]() { scene.setCausticRadiusMeters(std::numeric_limits<double>::quiet_NaN()); }, "Scene accepted non-finite caustic radius.");
		requireThrows([&]() { scene.setCausticAlpha(0.0); }, "Scene accepted zero caustic alpha.");
		requireThrows([&]() { scene.setCausticAlpha(1.1); }, "Scene accepted caustic alpha above one.");
		requireThrows([&]() { scene.setCausticAlpha(std::numeric_limits<double>::quiet_NaN()); }, "Scene accepted non-finite caustic alpha.");
		requireThrows([&]() { scene.setEnvironmentStrength(-1.0); }, "Scene accepted negative environment strength.");
		requireThrows([&]() { scene.setEnvironmentStrength(std::numeric_limits<double>::quiet_NaN()); }, "Scene accepted non-finite environment strength.");
		requireThrows([&]() { scene.setEnvironmentRotation(std::numeric_limits<double>::quiet_NaN()); }, "Scene accepted non-finite environment rotation.");
		requireThrows([&]() { scene.setDefaultRenderOutputFileName("render"); }, "Scene accepted output file without an extension.");
		requireThrows([&]() { scene.setDefaultRenderOutputFileName("render.tif"); }, "Scene accepted .tif output extension.");
		requireThrows([&]() { scene.setDefaultRenderOutputFileName("render.jpg"); }, "Scene accepted unsupported output extension.");
		requireThrows([&]() { scene.setDenoiseOutputFileName("render_denoised"); }, "Scene accepted denoise output without an extension.");
		requireThrows([&]() { scene.setDenoiseOutputFileName("render_denoised.tif"); }, "Scene accepted .tif denoise output extension.");
		requireThrows([&]() { scene.setDenoiseOutputFileName("render_denoised.jpg"); }, "Scene accepted unsupported denoise output extension.");
		requireThrows([&]() { image.setWidth(0); }, "Image accepted zero width.");
		requireThrows([&]() { image.setHeight(0); }, "Image accepted zero height.");
		requireThrows([&]() { Atmosphere().setSamples(0); }, "Atmosphere accepted zero samples.");
		requireThrows([&]() { Atmosphere().setLightSamples(0); }, "Atmosphere accepted zero light samples.");
		requireThrows([&]() { Atmosphere().setSunAngle(std::numeric_limits<double>::quiet_NaN()); }, "Atmosphere accepted non-finite sun angle.");
		requireThrows([&]() { Atmosphere().setSunDirection(Vector3(0.0, 0.0, 0.0)); }, "Atmosphere accepted zero sun direction.");
		requireThrows([&]() { Atmosphere().setSunDirection(Vector3(1.0, std::numeric_limits<double>::quiet_NaN(), 0.0)); }, "Atmosphere accepted non-finite sun direction.");
		requireThrows([&]() { Atmosphere().setSunRadiance(Color(-1.0, 1.0, 1.0)); }, "Atmosphere accepted negative sun radiance.");
		requireThrows([&]() { Atmosphere().setSunRadiance(Color(1.0, std::numeric_limits<double>::quiet_NaN(), 1.0)); }, "Atmosphere accepted non-finite sun radiance.");
		requireThrows([&]() { Atmosphere().setSunRadianceScale(-1.0); }, "Atmosphere accepted negative sun radiance scale.");
		requireThrows([&]() { Atmosphere().setSunRadianceScale(std::numeric_limits<double>::quiet_NaN()); }, "Atmosphere accepted non-finite sun radiance scale.");
		requireThrows([&]() { Atmosphere().setMetersPerUnit(0.0); }, "Atmosphere accepted zero meters per unit.");
		requireThrows([&]() { Atmosphere().setMetersPerUnit(std::numeric_limits<double>::quiet_NaN()); }, "Atmosphere accepted non-finite meters per unit.");
		requireThrows([&]() { Atmosphere().setEarthRadius(D_ATMOSPHERE_RADIUS); }, "Atmosphere accepted Earth radius at atmosphere radius.");
		requireThrows([&]() { Atmosphere().setAtmosphereRadius(D_EARTH_RADIUS); }, "Atmosphere accepted atmosphere radius at Earth radius.");
		requireThrows([&]() { Atmosphere().setHR(std::numeric_limits<double>::quiet_NaN()); }, "Atmosphere accepted non-finite HR.");
		requireThrows([&]() { Atmosphere().setHM(std::numeric_limits<double>::quiet_NaN()); }, "Atmosphere accepted non-finite HM.");
		requireThrows([&]() { Atmosphere().setStarsBrightness(std::numeric_limits<double>::quiet_NaN()); }, "Atmosphere accepted non-finite star brightness.");
		requireThrows([&]() { Atmosphere(0.0, 2.0, 1.0, 1.0, 1.0, 1, 1, 0.0); }, "Atmosphere accepted atmosphere radius smaller than Earth radius.");
		Atmosphere(0.0, D_ATMOSPHERE_RADIUS * 2.0, D_ATMOSPHERE_RADIUS * 2.0 + 1000.0, D_HR, D_HM, 1, 1, 0.0);
		requireThrows([&]() { Dielectric(Color(1.0, 1.0, 1.0), 0.0); }, "Dielectric accepted zero refractive index.");
		Dielectric glass(Color(1.0, 1.0, 1.0));
		requireThrows([&]() { glass.setAbsorptionCoefficient(Color(-1.0, 0.0, 0.0)); }, "Dielectric accepted negative absorption.");
		requireThrows([&]() { glass.setTransmittance(Color(1.2, 1.0, 1.0), 1.0); }, "Dielectric accepted transmittance above one.");
		requireThrows([&]() { glass.setTransmittance(Color(1.0, 1.0, 1.0), 0.0); }, "Dielectric accepted zero transmittance distance.");
		Metal conductor;
		requireThrows([&]() { conductor.setConductorFresnel(Color(0.0, 1.0, 1.0), Color(1.0, 1.0, 1.0)); }, "Metal accepted zero conductor eta.");
		requireThrows([&]() { conductor.setConductorFresnel(Color(1.0, 1.0, 1.0), Color(-1.0, 1.0, 1.0)); }, "Metal accepted negative conductor k.");
		requireThrows([&]() { HenyeyGreenstein(Color(1.0, 1.0, 1.0), 1.0); }, "Henyey-Greenstein material accepted invalid anisotropy.");
		HenyeyGreenstein cloudPhase;
		requireThrows([&]() { cloudPhase.setDropletPhase(4.0); }, "Cloud phase accepted an undersized droplet.");
		requireThrows([&]() {
			ConstantVolume(
				std::make_shared<Sphere>(Vector3(0.0, 0.0, 0.0), 1.0, std::make_shared<Lambertian>(Color(0.0, 0.0, 0.0))),
				std::make_shared<Isotropic>(Color(1.0, 1.0, 1.0)),
				0.0
			);
		}, "ConstantVolume accepted zero density.");
	}

	void	testPlanetaryHitRobustIntersections(void)
	{
		HitRecord hitRecord;
		Ray perpendicularMiss(Vector3(2.0, 0.0, 0.0), Vector3(0.0, 1.0, 0.0));
		require(!planetaryHit(1.0, perpendicularMiss, hitRecord), "Planetary hit accepted a perpendicular miss.");

		Ray tangentRay(Vector3(1.0, 0.0, 0.0), Vector3(0.0, 1.0, 0.0));
		require(planetaryHit(1.0, tangentRay, hitRecord), "Planetary hit rejected a tangent ray.");
		requireNear(hitRecord.t0, 0.0, "Planetary tangent t0 is wrong.");
		requireNear(hitRecord.t1, 0.0, "Planetary tangent t1 is wrong.");

		Ray throughSphere(Vector3(0.0, 0.0, 2.0), Vector3(0.0, 0.0, -1.0));
		require(planetaryHit(1.0, throughSphere, hitRecord), "Planetary hit rejected a direct hit.");
		requireNear(hitRecord.t0, 1.0, "Planetary direct hit t0 is wrong.");
		requireNear(hitRecord.t1, 3.0, "Planetary direct hit t1 is wrong.");
	}

	void	testAtmosphereIncidentLightInsideOutsideAndSunPosition(void)
	{
		const double earthRadius = 6360000.0;
		const double atmosphereRadius = 6420000.0;
		const double hR = 7994.0;
		const double hM = 1200.0;
		Atmosphere dayAtmosphere(0.0, earthRadius, atmosphereRadius, hR, hM, 16, 8, 0.0);
		Atmosphere nightAtmosphere(1.0, earthRadius, atmosphereRadius, hR, hM, 16, 8, 0.0);
		HitRecord hitRecord;

		Ray groundUp(Vector3(0.0, earthRadius + 120.0, 0.0), Vector3(0.0, 1.0, 0.0));
		const Color groundSky = dayAtmosphere.computeIncidentLight(groundUp, hitRecord, T_MAX);
		requireFiniteNonNegativeColor(groundSky, "Ground atmosphere sky");
		require(Utilities::luminance(groundSky) > 0.0, "Ground atmosphere sky is black.");
		const Color groundSkyLinearSRGB = ColorManagement::linearSRGBFromACEScg(groundSky);
		require(
			groundSkyLinearSRGB.getRed() > 0.0
			&& groundSkyLinearSRGB.getBlue() > groundSkyLinearSRGB.getGreen()
			&& groundSkyLinearSRGB.getGreen() > groundSkyLinearSRGB.getRed(),
			"Day atmosphere lost its positive red channel or physical blue-sky ordering."
		);

		const Color nightSky = nightAtmosphere.computeIncidentLight(groundUp, hitRecord, T_MAX);
		requireFiniteNonNegativeColor(nightSky, "Night atmosphere sky");
		require(Utilities::luminance(groundSky) > Utilities::luminance(nightSky), "Sun angle did not affect atmosphere brightness.");

		Ray spaceLimb(Vector3(0.0, 0.0, atmosphereRadius + 120000.0), Vector3(0.0, 0.98, -0.20));
		const Color limbSky = dayAtmosphere.computeIncidentLight(spaceLimb, hitRecord, T_MAX);
		requireFiniteNonNegativeColor(limbSky, "Space limb atmosphere sky");
		require(Utilities::luminance(limbSky) > 0.0, "Space limb atmosphere sky is black.");

		Ray spaceAway(Vector3(0.0, 0.0, atmosphereRadius + 120000.0), Vector3(0.0, 0.0, 1.0));
		const Color spaceSky = dayAtmosphere.computeIncidentLight(spaceAway, hitRecord, T_MAX);
		requireFiniteNonNegativeColor(spaceSky, "Space miss atmosphere sky");
		requireNear(spaceSky.getRed(), 0.0, "Space miss atmosphere red should be black.");
		requireNear(spaceSky.getGreen(), 0.0, "Space miss atmosphere green should be black.");
		requireNear(spaceSky.getBlue(), 0.0, "Space miss atmosphere blue should be black.");

		Ray planetRay(Vector3(0.0, 0.0, atmosphereRadius + 120000.0), Vector3(0.0, 0.0, -1.0));
		HitRecord earthHitRecord;
		double tMax = T_MAX;
		if (planetaryHit(dayAtmosphere.getEarthRadius(), planetRay, earthHitRecord) && earthHitRecord.t1 > 0.0)
		{
			tMax = std::max(0.0, earthHitRecord.t0);
		}
		const Color clippedAtmosphere = dayAtmosphere.computeIncidentLight(planetRay, hitRecord, tMax);
		requireFiniteNonNegativeColor(clippedAtmosphere, "Planet-clipped atmosphere sky");

		const Color negativeIntervalSky = dayAtmosphere.computeIncidentLight(groundUp, hitRecord, 0.0);
		requireFiniteNonNegativeColor(negativeIntervalSky, "Negative interval atmosphere sky");
		requireNear(negativeIntervalSky.getRed(), 0.0, "Negative interval atmosphere red should be black.");
		requireNear(negativeIntervalSky.getGreen(), 0.0, "Negative interval atmosphere green should be black.");
		requireNear(negativeIntervalSky.getBlue(), 0.0, "Negative interval atmosphere blue should be black.");
	}

	void	testAtmosphereSegmentTransmittance(void)
	{
		const Atmosphere atmosphere(0.0, 1.0, 2.0, 1.0, 1.0, 8, 4, 0.0);

		Ray surfaceRay(Vector3(0.0, 0.0, -4.0), Vector3(0.0, 0.0, 1.0));
		const AtmosphereSample surfaceSample = atmosphere.sampleSegment(surfaceRay, 3.0);
		const Color transmittanceOnly = atmosphere.sampleTransmittance(surfaceRay, 3.0);
		requireFiniteNonNegativeColor(surfaceSample.inScattering, "Atmosphere segment in-scattering");
		requireFiniteNonNegativeColor(surfaceSample.transmittance, "Atmosphere segment transmittance");
		require(Utilities::luminance(surfaceSample.inScattering) > 0.0, "Atmosphere segment did not add in-scattering.");
		require(surfaceSample.transmittance.getRed() <= 1.0, "Atmosphere red transmittance exceeds one.");
		require(surfaceSample.transmittance.getGreen() <= 1.0, "Atmosphere green transmittance exceeds one.");
		require(surfaceSample.transmittance.getBlue() <= 1.0, "Atmosphere blue transmittance exceeds one.");
		require(surfaceSample.transmittance.getBlue() < 1.0, "Atmosphere segment did not attenuate the view ray.");
		requireColorNear(transmittanceOnly, surfaceSample.transmittance, "Fast atmosphere transmittance");

		HitRecord hitRecord;
		requireColorNear(
			atmosphere.computeIncidentLight(surfaceRay, hitRecord, 3.0),
			surfaceSample.inScattering,
			"Legacy atmosphere incident-light wrapper"
		);

		Ray spaceAway(Vector3(0.0, 0.0, -4.0), Vector3(0.0, 0.0, -1.0));
		const AtmosphereSample emptySample = atmosphere.sampleSegment(spaceAway, T_MAX);
		requireNear(emptySample.inScattering.getRed(), 0.0, "Empty atmosphere segment red should be black.");
		requireNear(emptySample.inScattering.getGreen(), 0.0, "Empty atmosphere segment green should be black.");
		requireNear(emptySample.inScattering.getBlue(), 0.0, "Empty atmosphere segment blue should be black.");
		requireNear(emptySample.transmittance.getRed(), 1.0, "Empty atmosphere segment red transmittance should be one.");
		requireNear(emptySample.transmittance.getGreen(), 1.0, "Empty atmosphere segment green transmittance should be one.");
		requireNear(emptySample.transmittance.getBlue(), 1.0, "Empty atmosphere segment blue transmittance should be one.");
	}

	void	testAtmosphereDiffuseSkyRadianceCache(void)
	{
		Atmosphere atmosphere(0.0, 6360000.0, 6420000.0, 7994.0, 1200.0, 8, 4, 0.0);
		const Vector3 cameraPosition(0.0, 6360120.0, 0.0);
		const Color first = atmosphere.sampleDiffuseSkyRadiance(cameraPosition);
		const Color cached = atmosphere.sampleDiffuseSkyRadiance(cameraPosition);
		requireFiniteNonNegativeColor(first, "Diffuse atmosphere control");
		require(Utilities::luminance(first) > 0.0, "Diffuse atmosphere control is black during daytime.");
		requireColorNear(cached, first, "Diffuse atmosphere cache changed a repeated query");

		atmosphere.setSunRadianceScale(0.0);
		const Color dark = atmosphere.sampleDiffuseSkyRadiance(cameraPosition);
		requireColorNear(dark, Color(0.0, 0.0, 0.0), "Diffuse atmosphere cache was not invalidated");

		Atmosphere original(0.0, 6360000.0, 6420000.0, 7994.0, 1200.0, 8, 4, 0.0);
		const Color originalSky = original.sampleDiffuseSkyRadiance(cameraPosition);
		Atmosphere darkCopy = original;
		darkCopy.setSunRadianceScale(0.0);
		requireColorNear(
			darkCopy.sampleDiffuseSkyRadiance(cameraPosition),
			Color(0.0, 0.0, 0.0),
			"A copied atmosphere retained a stale diffuse-sky cache"
		);
		requireColorNear(
			original.sampleDiffuseSkyRadiance(cameraPosition),
			originalSky,
			"Mutating an atmosphere copy changed the original diffuse-sky cache"
		);
	}

	void	testAtmosphereMetersPerUnitPreservesPhysicalScale(void)
	{
		const Atmosphere meterAtmosphere(0.0, 1.0, 2.0, 1.0, 1.0, 8, 4, 0.0);
		Atmosphere scaledAtmosphere(0.0, 1.0, 2.0, 1.0, 1.0, 8, 4, 0.0);
		scaledAtmosphere.setMetersPerUnit(0.5);

		const Ray meterRay(Vector3(0.0, 0.0, -4.0), Vector3(0.0, 0.0, 1.0));
		const Ray scaledRay(Vector3(0.0, 0.0, -8.0), Vector3(0.0, 0.0, 1.0));
		const AtmosphereSample meterSample = meterAtmosphere.sampleSegment(meterRay, 3.0);
		const AtmosphereSample scaledSample = scaledAtmosphere.sampleSegment(scaledRay, 6.0);

		requireColorNear(scaledSample.inScattering, meterSample.inScattering, "Scaled atmosphere in-scattering");
		requireColorNear(scaledSample.transmittance, meterSample.transmittance, "Scaled atmosphere transmittance");

		Atmosphere largeUnitAtmosphere(0.0, 1.0, 2.0, 1.0, 1.0, 8, 4, 0.0);
		largeUnitAtmosphere.setMetersPerUnit(2.0);
		const Ray largeUnitRay(Vector3(0.0, 0.0, -2.0), Vector3(0.0, 0.0, 1.0));
		const AtmosphereSample largeUnitSample = largeUnitAtmosphere.sampleSegment(largeUnitRay, T_MAX);

		requireFiniteNonNegativeColor(largeUnitSample.inScattering, "Large-unit atmosphere in-scattering");
		require(Utilities::luminance(largeUnitSample.inScattering) > 0.0, "Large-unit atmosphere T_MAX overflowed to an empty sample.");
	}

	void	testAtmospherePrimaryHitCompositesSurface(void)
	{
		const Atmosphere atmosphere(0.0, 1.0, 2.0, 1.0, 1.0, 8, 4, 0.0);
		const Color surfaceEmission(0.2, 0.2, 0.2);
		Scene scene;

		scene.setRenderSky(SKY_ATMOSPHERE);
		scene.setAtmosphere(atmosphere);
		scene.setMaxLightBounces(0);
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0),
			1.0,
			std::make_shared<Emissive>(surfaceEmission)
		));

		Ray ray(Vector3(0.0, 0.0, -4.0), Vector3(0.0, 0.0, 1.0));
		const AtmosphereSample atmosphereSample = atmosphere.sampleSegment(ray, 3.0);
		const Color expected = atmosphereSample.inScattering + (atmosphereSample.transmittance * surfaceEmission);
		const Color actual = Renderer::internal::_calculateLightRaysColor(ray, scene);

		requireFiniteNonNegativeColor(actual, "Atmosphere primary-hit composite");
		requireColorNear(actual, expected, "Atmosphere primary-hit composite");
		require(
			std::abs(actual.getRed() - surfaceEmission.getRed()) > 1e-8
			|| std::abs(actual.getGreen() - surfaceEmission.getGreen()) > 1e-8
			|| std::abs(actual.getBlue() - surfaceEmission.getBlue()) > 1e-8,
			"Atmosphere primary-hit composite returned the bare surface color."
		);

		Atmosphere scaledAtmosphere(0.0, 1.0, 2.0, 1.0, 1.0, 8, 4, 0.0);
		Scene scaledScene;
		scaledScene.setMetersPerUnit(0.5);
		scaledScene.setRenderSky(SKY_ATMOSPHERE);
		scaledScene.setAtmosphere(scaledAtmosphere);
		scaledScene.setMaxLightBounces(0);
		scaledScene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0),
			2.0,
			std::make_shared<Emissive>(surfaceEmission)
		));

		Ray scaledRay(Vector3(0.0, 0.0, -8.0), Vector3(0.0, 0.0, 1.0));
		const AtmosphereSample scaledAtmosphereSample = scaledScene.getAtmosphere().sampleSegment(scaledRay, 6.0);
		const Color scaledExpected = scaledAtmosphereSample.inScattering + (scaledAtmosphereSample.transmittance * surfaceEmission);
		const Color scaledActual = Renderer::internal::_calculateLightRaysColor(scaledRay, scaledScene);

		requireFiniteNonNegativeColor(scaledActual, "Scaled atmosphere primary-hit composite");
		requireColorNear(scaledActual, scaledExpected, "Scaled atmosphere primary-hit composite");
		requireColorNear(scaledActual, actual, "Scaled atmosphere primary-hit physical equivalence");

		Scene protrudingScene;
		protrudingScene.setRenderSky(SKY_ATMOSPHERE);
		protrudingScene.setAtmosphere(atmosphere);
		protrudingScene.setMaxLightBounces(0);
		protrudingScene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0),
			1.5,
			std::make_shared<Emissive>(surfaceEmission)
		));

		const Color protrudingExpected = atmosphereSample.inScattering + (atmosphereSample.transmittance * surfaceEmission);
		const Color protrudingActual = Renderer::internal::_calculateLightRaysColor(ray, protrudingScene);
		requireFiniteNonNegativeColor(protrudingActual, "Atmosphere protruding primary-hit composite");
		requireColorNear(
			protrudingActual,
			protrudingExpected,
			"Atmosphere protruding primary-hit should use analytic ground depth"
		);
	}

	void	testSceneDefaultsEnableAdaptiveAndDenoise(void)
	{
		Scene scene;

		require(scene.getAdaptiveSampling(), "Adaptive sampling is not enabled by default.");
		require(
			scene.getAdaptiveBackgroundMinSamples() == 0,
			"Adaptive background minimum should inherit by default."
		);
		require(
			scene.getAdaptiveVolumeMinSamples() == 0,
			"Adaptive volume minimum should inherit by default."
		);
		require(scene.getDenoise(), "Denoising is not enabled by default.");
	}

	void	testRenderedSceneHasBasicVisualStructure(void)
	{
		setRandomSeed(42);

		Scene scene;
		configureVisualRenderScene(scene, 9, 9);

		require(Renderer::render(scene), "Visual structure render failed.");
		requireImageIsFinite(*scene.getImage(), "Visual structure render");

		std::size_t foregroundPixels = 0;
		std::size_t backgroundPixels = 0;
		bool centerRegionHasForeground = false;

		for (std::size_t y = 0; y < scene.getImage()->getHeight(); y++)
		{
			for (std::size_t x = 0; x < scene.getImage()->getWidth(); x++)
			{
				const Color pixel = scene.getImage()->getPixel(x, y);

				if (isVisualForegroundPixel(pixel))
				{
					foregroundPixels++;
					if (x >= 3 && x <= 5 && y >= 3 && y <= 5)
					{
						centerRegionHasForeground = true;
					}
				}
				if (isVisualBackgroundPixel(pixel))
				{
					backgroundPixels++;
				}
			}
		}

		require(foregroundPixels > 0, "Visual render did not produce the emissive foreground object.");
		require(centerRegionHasForeground, "Visual render foreground object is not near the image center.");
		require(backgroundPixels >= 40, "Visual render did not preserve enough background pixels.");
	}

	void	testRenderedScenePostProcessingProducesDisplayableImage(void)
	{
		setRandomSeed(42);

		Scene scene;
		configureVisualRenderScene(scene, 9, 9);
		scene.setViewTransform(ViewTransform::AgX);
		scene.setBloom(true);
		scene.setExposure(0.5);
		scene.setContrast(1.1);

		require(Renderer::render(scene), "Post-processed visual render failed.");
		requireImageIsDisplayable(*scene.getImage(), "Post-processed visual render");
		const SceneRenderStats& stats = scene.getRenderStats();
		require(stats.displayDiagnosticsValid, "Display render did not produce exposure diagnostics.");
		require(
			stats.displayLuminanceP01 <= stats.displayLuminanceP50
			&& stats.displayLuminanceP50 <= stats.displayLuminanceP99,
			"Display luminance diagnostics are not ordered."
		);
		require(
			stats.displayClippedPixelFraction >= 0.0
			&& stats.displayClippedPixelFraction <= 1.0,
			"Display clipped-pixel diagnostic is outside its valid range."
		);

		bool redForegroundSurvived = false;
		for (std::size_t y = 3; y <= 5; y++)
		{
			for (std::size_t x = 3; x <= 5; x++)
			{
				const Color pixel = scene.getImage()->getPixel(x, y);
				if (
					pixel.getRed() > 0.55
					&& pixel.getRed() > pixel.getGreen()
					&& pixel.getRed() > pixel.getBlue()
				)
				{
					redForegroundSurvived = true;
				}
			}
		}

		require(redForegroundSurvived, "Post-processing lost the red foreground visual signal.");
	}

	void	testRenderedBMPContainsVisualSignal(void)
	{
		setRandomSeed(42);

		Scene scene;
		configureVisualRenderScene(scene, 9, 9);

		require(Renderer::render(scene), "Rendered BMP visual render failed.");

		std::size_t brightestX = 0;
		std::size_t brightestY = 0;
		double brightestRed = -1.0;
		for (std::size_t y = 0; y < scene.getImage()->getHeight(); y++)
		{
			for (std::size_t x = 0; x < scene.getImage()->getWidth(); x++)
			{
				const Color pixel = scene.getImage()->getPixel(x, y);
				if (pixel.getRed() > brightestRed)
				{
					brightestRed = pixel.getRed();
					brightestX = x;
					brightestY = y;
				}
			}
		}
		require(brightestRed > 0.85, "Rendered image did not contain a bright red pixel before BMP output.");

		const std::filesystem::path outputPath = std::filesystem::temp_directory_path() / "luz_visual_render_test";
		const std::filesystem::path bmpPath = outputPath.string() + ".bmp";

		scene.getImage()->saveToBMP(outputPath.string());
		const std::vector<unsigned char> bytes = readFile(bmpPath);
		const std::size_t foregroundOffset = bmpPixelOffset(
			scene.getImage()->getWidth(),
			scene.getImage()->getHeight(),
			brightestX,
			brightestY
		);
		const std::size_t backgroundOffset = bmpPixelOffset(scene.getImage()->getWidth(), scene.getImage()->getHeight(), 0, 0);

		require(bytes.size() > foregroundOffset + 2, "Rendered BMP file is too small for foreground pixel.");
		require(bytes.size() > backgroundOffset + 2, "Rendered BMP file is too small for background pixel.");
		require(bytes[foregroundOffset + 2] > 200, "Rendered BMP foreground red channel is too dark.");
		require(bytes[foregroundOffset + 2] > bytes[foregroundOffset + 1] * 4, "Rendered BMP foreground is not red-dominant.");
		require(bytes[backgroundOffset] > bytes[backgroundOffset + 2], "Rendered BMP background did not preserve blue dominance.");
		require(bytes[backgroundOffset + 2] < 20, "Rendered BMP background red channel is too bright.");

		std::filesystem::remove(bmpPath);
	}

	void	testGlossySurfaceReflectsEmissiveLight(void)
	{
		setRandomSeed(2026);

		Scene scene;
		scene.getImage()->setWidth(9);
		scene.getImage()->setHeight(9);
		scene.getImage()->initialize();
		scene.setSampleCount(16);
		scene.setAdaptiveSampling(false);
		scene.setMaxLightBounces(2);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(false);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.0, 0.0, 0.0));
		scene.setRenderingThreads(1);
		scene.setBenchmarkMode(true);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 1.0, 4.0), Vector3(0.0, -0.35, -1.0), 4.0));
		scene.addHittable(std::make_shared<Plane>(
			0.0,
			Vector3(0.0, 1.0, 0.0),
			std::make_shared<Principled>(
				Color(1.0, 1.0, 1.0),
				0.0,
				0.08,
				0.0,
				1.5,
				1.0,
				0.08,
				0.0
			)
		));
		scene.addHittable(std::make_shared<Rectangle>(
			Transform(Vector3(0.0, 3.0, -7.5), Vector3(0.0, -1.0, 0.0), Vector3(1.0, 0.0, 0.0)),
			5.0,
			5.0,
			std::make_shared<Emissive>(Color(4.0, 4.0, 4.0))
		));

		require(Renderer::render(scene), "Glossy reflection render failed.");
		requireImageIsFinite(*scene.getImage(), "Glossy reflection render");

		double brightestLuminance = 0.0;
		for (std::size_t y = 0; y < scene.getImage()->getHeight(); y++)
		{
			for (std::size_t x = 0; x < scene.getImage()->getWidth(); x++)
			{
				brightestLuminance = std::max(
					brightestLuminance,
					Utilities::luminance(scene.getImage()->getPixel(x, y))
				);
			}
		}

		require(brightestLuminance > 0.05, "Glossy reflected light path lost too much energy.");
	}

	void	testConstantBackgroundDirectlyLightsSurfaces(void)
	{
		setRandomSeed(2027);

		Scene scene;
		scene.getImage()->setWidth(3);
		scene.getImage()->setHeight(3);
		scene.getImage()->initialize();
		scene.setSampleCount(128);
		scene.setAdaptiveSampling(false);
		scene.setMaxLightBounces(0);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(false);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.4, 0.4, 0.4));
		scene.setRenderingThreads(1);
		scene.setBenchmarkMode(true);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 1.0, 3.0), Vector3(0.0, -0.5, -1.0), 3.0));
		scene.addHittable(std::make_shared<Plane>(
			0.0,
			Vector3(0.0, 1.0, 0.0),
			std::make_shared<Lambertian>(Color(1.0, 1.0, 1.0))
		));

		require(Renderer::render(scene), "Constant background direct-light render failed.");
		requireImageIsFinite(*scene.getImage(), "Constant background direct-light render");
		const double centerLuminance = Utilities::luminance(scene.getImage()->getPixel(1, 1));

		require(
			centerLuminance > 0.01,
			"Constant background was not sampled as direct infinite light at zero bounce depth."
		);
	}

	void	testConstantBackgroundDoesNotDirectlySampleTransmissiveBSDF(void)
	{
		setRandomSeed(2028);

		Scene scene;
		scene.getImage()->setWidth(3);
		scene.getImage()->setHeight(3);
		scene.getImage()->initialize();
		scene.setSampleCount(128);
		scene.setAdaptiveSampling(false);
		scene.setMaxLightBounces(0);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(false);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.4, 0.4, 0.4));
		scene.setRenderingThreads(1);
		scene.setBenchmarkMode(true);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 0.0, 3.0), Vector3(0.0, 0.0, -1.0), 3.0));
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, 0.0),
			1.0,
			std::make_shared<Principled>(
				Color(0.9, 0.8, 0.7),
				0.0,
				0.5,
				1.0,
				1.1,
				0.0,
				0.03,
				0.0
			)
		));

		require(Renderer::render(scene), "Constant background transmissive BSDF render failed.");
		requireImageIsFinite(*scene.getImage(), "Constant background transmissive BSDF render");
		const double centerLuminance = Utilities::luminance(scene.getImage()->getPixel(1, 1));

		require(
			centerLuminance < 0.01,
			"Constant background was directly sampled by a transmissive BSDF at zero bounce depth."
		);
	}

	void	testTinyRender(void)
	{
		setRandomSeed(42);

		Scene scene;
		scene.getImage()->setWidth(4);
		scene.getImage()->setHeight(3);
		scene.getImage()->initialize();
		scene.setSampleCount(1);
		scene.setAdaptiveSampling(false);
		scene.setMaxLightBounces(1);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(false);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.1, 0.2, 0.3));
		scene.setRenderingThreads(1);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0), 1.0));
		scene.addHittable(std::make_shared<Sphere>(
			Vector3(0.0, 0.0, -1.0),
			0.5,
			std::make_shared<Lambertian>(Color(0.8, 0.2, 0.2))
		));

		require(Renderer::render(scene), "Tiny render failed.");
		for (std::size_t y = 0; y < scene.getImage()->getHeight(); y++)
		{
			for (std::size_t x = 0; x < scene.getImage()->getWidth(); x++)
			{
				const Color pixel = scene.getImage()->getPixel(x, y);
				require(std::isfinite(pixel.getRed()), "Tiny render produced non-finite red value.");
				require(std::isfinite(pixel.getGreen()), "Tiny render produced non-finite green value.");
				require(std::isfinite(pixel.getBlue()), "Tiny render produced non-finite blue value.");
			}
		}
	}

	void	testEnvironmentBackgroundRender(void)
	{
		setRandomSeed(42);

		const std::filesystem::path environmentPath = std::filesystem::temp_directory_path() / "luz_environment_render_test.ppm";
		writeSinglePixelPPM(environmentPath, 2, 4, 6);

		Scene scene;
		scene.getImage()->setWidth(2);
		scene.getImage()->setHeight(2);
		scene.getImage()->initialize();
		scene.setSampleCount(1);
		scene.setMaxLightBounces(0);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setRenderSky(SKY_ENVIRONMENT);
		scene.setEnvironmentMap(std::make_shared<EnvironmentMap>(EnvironmentMap::load(environmentPath.string())));
		scene.setEnvironmentStrength(1.5);
		scene.setRenderingThreads(1);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0), 1.0));

		require(Renderer::render(scene), "Environment background render failed.");
		const Color expected = ColorManagement::acescgFromSRGB(Color(0.2, 0.4, 0.6)) * 1.5;
		for (std::size_t y = 0; y < scene.getImage()->getHeight(); y++)
		{
			for (std::size_t x = 0; x < scene.getImage()->getWidth(); x++)
			{
				const Color pixel = scene.getImage()->getPixel(x, y);
				requireColorNear(pixel, expected, "Environment render did not use ACEScg environment radiance.");
			}
		}

		std::filesystem::remove(environmentPath);
	}

	void	testTinyAdaptiveRender(void)
	{
		setRandomSeed(42);

		Scene scene;
		scene.getImage()->setWidth(2);
		scene.getImage()->setHeight(2);
		scene.getImage()->initialize();
		scene.setSampleCount(8);
		scene.setAdaptiveSampling(true);
		scene.setAdaptiveMinSamples(6);
		scene.setAdaptiveBackgroundMinSamples(2);
		scene.setAdaptiveThreshold(0.5);
		scene.setAdaptiveCheckInterval(1);
		scene.setMaxLightBounces(1);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(false);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.1, 0.2, 0.3));
		scene.setRenderingThreads(1);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0), 1.0));

		require(Renderer::render(scene), "Tiny adaptive render failed.");
		require(
			!scene.getRenderStats().displayDiagnosticsValid,
			"Raw render incorrectly reported display exposure diagnostics."
		);
		require(
			scene.getRenderStats().renderedSamples == 8,
			"Stable background did not stop at its two-sample adaptive floor."
		);
		for (std::size_t y = 0; y < scene.getImage()->getHeight(); y++)
		{
			for (std::size_t x = 0; x < scene.getImage()->getWidth(); x++)
			{
				const Color pixel = scene.getImage()->getPixel(x, y);
				require(std::isfinite(pixel.getRed()), "Tiny adaptive render produced non-finite red value.");
				requireNear(pixel.getRed(), 0.1, "Tiny adaptive render changed stable background red.");
			}
		}
	}

	void	testAdaptiveVolumeMinimumSamples(void)
	{
		setRandomSeed(4242);

		Scene scene;
		scene.getImage()->setWidth(1);
		scene.getImage()->setHeight(1);
		scene.getImage()->initialize();
		scene.setSampleCount(8);
		scene.setAdaptiveSampling(true);
		scene.setAdaptiveMinSamples(2);
		scene.setAdaptiveBackgroundMinSamples(2);
		scene.setAdaptiveVolumeMinSamples(8);
		scene.setAdaptiveThreshold(0.5);
		scene.setAdaptiveCheckInterval(1);
		scene.setMaxLightBounces(1);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(false);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.1, 0.2, 0.3));
		scene.setRenderingThreads(1);
		scene.setBenchmarkMode(true);
		scene.setVolumeGuidingTrainingSamples(16);
		scene.setVolumeGuidingResolution(2);
		scene.setVolumeGuidingLobes(8);
		scene.setVolumeGuidingStrength(0.25);
		scene.addCamera(testPinholeCamera(
			Vector3(0.0, 0.0, 5.0),
			Vector3(0.0, 0.0, -1.0),
			5.0
		));

		CloudParameters parameters = CloudVolume::preset(CloudType::Stratus);
		parameters.position = Vector3(0.0, 0.0, 0.0);
		parameters.size = Vector3(8.0, 8.0, 8.0);
		parameters.coverage = 1.0;
		parameters.extinction = 2.0;
		parameters.featureScale = 2.0;
		parameters.detail = 0.0;
		parameters.erosion = 0.0;
		parameters.maxTrackingSteps = 64;
		scene.addHittable(std::make_shared<CloudVolume>(parameters));

		require(Renderer::render(scene), "Adaptive volume-floor render failed.");
		require(
			scene.getVolumeGuidingField() && scene.getVolumeGuidingField()->isFrozen(),
			"Volume guiding prepass did not produce a frozen render-time field."
		);
		require(scene.getRenderStats().volumeGuideMS > 0.0,
			"Volume guiding prepass time was not reported.");
		require(
			scene.getRenderStats().renderedSamples == 8,
			"Cloud pixel did not retain the configured eight-sample volume floor."
		);
	}

	void	testNFORDoesNotSpreadIsolatedFirefly(void)
	{
		Denoise::NFORBuffers buffers;

		buffers.initialize(7, 7);
		for (std::size_t y = 0; y < buffers.height; y++)
		{
			for (std::size_t x = 0; x < buffers.width; x++)
			{
				const std::size_t index = buffers.index(x, y);

				buffers.colorA[index] = Color(0.12, 0.13, 0.14);
				buffers.colorB[index] = Color(0.12, 0.13, 0.14);
				buffers.colorVariance[index] = 0.0001;
				buffers.featuresA[index][0] = static_cast<double>(x) / 6.0;
				buffers.featuresA[index][1] = static_cast<double>(y) / 6.0;
				buffers.featuresB[index] = buffers.featuresA[index];
			}
		}
		const std::size_t center = buffers.index(3, 3);
		buffers.colorA[center] = Color(8.0, 8.0, 8.0);
		buffers.colorB[center] = Color(8.0, 8.0, 8.0);

		Denoise::NFORSettings settings;
		settings.threadCount = 1;
		std::unique_ptr<Image> denoised = Denoise::applyNFOR(buffers, settings);
		const Color centerPixel = denoised->getPixel(3, 3);
		const Color neighborPixel = denoised->getPixel(3, 2);

		require(
			Utilities::luminance(centerPixel) < 0.5,
			"NFOR kept an isolated firefly at the source pixel."
		);
		require(
			Utilities::luminance(neighborPixel) < 0.5,
			"NFOR spread an isolated firefly into a neighbor."
		);
	}

	void	testNFORVolumeResidualReconstructionRemovesCheckerVariance(void)
	{
		Denoise::NFORBuffers buffers;

		buffers.initialize(9, 9);
		for (std::size_t y = 0; y < buffers.height; y++)
		{
			for (std::size_t x = 0; x < buffers.width; x++)
			{
				const std::size_t index = buffers.index(x, y);
				const double checker = ((x + y) % 2 == 0) ? 1.0 : 0.0;
				buffers.colorA[index] = Color(checker, checker, checker);
				buffers.colorB[index] = Color(checker, checker, checker);
				buffers.colorVariance[index] = 0.25;
				buffers.deterministicColor[index] = Color(0.2, 0.2, 0.2);
				buffers.featuresA[index][0] = static_cast<double>(x) / 8.0;
				buffers.featuresA[index][1] = static_cast<double>(y) / 8.0;
				buffers.featuresA[index][2] = 1.0;
				buffers.featuresA[index][3] = 0.5;
				buffers.featuresA[index][4] = 0.5;
				buffers.featuresA[index][5] = 1.0;
				buffers.featuresA[index][6] = 0.5;
				buffers.featuresA[index][10] = static_cast<double>(HENYEY_GREENSTEIN)
					/ static_cast<double>(PRINCIPLED);
				buffers.featuresB[index] = buffers.featuresA[index];
			}
		}

		Denoise::NFORSettings settings;
		settings.threadCount = 1;
		std::unique_ptr<Image> denoised = Denoise::applyNFOR(buffers, settings);
		const double center = Utilities::luminance(denoised->getPixel(4, 4));
		const double neighbor = Utilities::luminance(denoised->getPixel(5, 4));

		require(std::isfinite(center) && std::isfinite(neighbor), "Volume residual reconstruction produced non-finite radiance.");
		require(
			std::fabs(center - neighbor) < 0.12,
			"Volume residual reconstruction retained coherent checker variance."
		);
		require(
			center > 0.25 && center < 0.75,
			"Volume residual reconstruction did not preserve the diffuse mean."
		);
	}

	void	testNFORVolumeDensityGuidePreservesBillowBoundary(void)
	{
		Denoise::NFORBuffers buffers;

		buffers.initialize(15, 9);
		for (std::size_t y = 0; y < buffers.height; y++)
		{
			for (std::size_t x = 0; x < buffers.width; x++)
			{
				const std::size_t index = buffers.index(x, y);
				const bool denseBillow = x >= 7;
				const double target = denseBillow ? 0.78 : 0.22;
				const double noise = ((x + y) % 2 == 0) ? 0.18 : -0.18;
				const double sample = target + noise;

				buffers.colorA[index] = Color(sample, sample, sample);
				buffers.colorB[index] = Color(sample, sample, sample);
				buffers.colorVariance[index] = 0.08;
				buffers.deterministicColor[index] = Color(0.1, 0.1, 0.1);
				buffers.volumeOpacity[index] = denseBillow ? 0.92 : 0.18;
				buffers.featuresA[index][0] = static_cast<double>(x) / 14.0;
				buffers.featuresA[index][1] = static_cast<double>(y) / 8.0;
				buffers.featuresA[index][2] = 1.0;
				buffers.featuresA[index][3] = 0.5;
				buffers.featuresA[index][4] = 0.5;
				buffers.featuresA[index][5] = 1.0;
				buffers.featuresA[index][6] = 0.5;
				buffers.featuresA[index][7] = denseBillow ? 0.85 : 0.15;
				buffers.featuresA[index][10] = static_cast<double>(HENYEY_GREENSTEIN)
					/ static_cast<double>(PRINCIPLED);
				buffers.featuresB[index] = buffers.featuresA[index];
			}
		}

		Denoise::NFORSettings settings;
		settings.threadCount = 1;
		std::unique_ptr<Image> denoised = Denoise::applyNFOR(buffers, settings);
		double thinMean = 0.0;
		double thickMean = 0.0;
		for (std::size_t y = 2; y <= 6; y++)
		{
			for (std::size_t x = 4; x <= 6; x++)
				thinMean += Utilities::luminance(denoised->getPixel(x, y));
			for (std::size_t x = 7; x <= 9; x++)
				thickMean += Utilities::luminance(denoised->getPixel(x, y));
		}
		thinMean /= 15.0;
		thickMean /= 15.0;
		require(
			thickMean - thinMean > 0.38,
			"Volume density guide blurred a thin-to-thick billow boundary."
		);
		require(
			std::fabs(denoised->getPixel(5, 4).getRed()
				- denoised->getPixel(5, 5).getRed()) < 0.16,
			"Volume density guide preserved checker variance inside a billow."
		);
	}

	void	testNFORPreservesConvergedVolumeDetail(void)
	{
		Denoise::NFORBuffers buffers;

		buffers.initialize(9, 9);
		for (std::size_t y = 0; y < buffers.height; y++)
		{
			for (std::size_t x = 0; x < buffers.width; x++)
			{
				const std::size_t index = buffers.index(x, y);
				const double detail = x % 2 == 0 ? 0.20 : 0.40;

				buffers.colorA[index] = Color(detail, detail, detail);
				buffers.colorB[index] = Color(detail, detail, detail);
				buffers.colorVariance[index] = 1e-8;
				buffers.sampleCount[index] = 128u;
				buffers.deterministicColor[index] = Color(0.1, 0.1, 0.1);
				buffers.volumeOpacity[index] = 0.6;
				buffers.featuresA[index][0] = static_cast<double>(x) / 8.0;
				buffers.featuresA[index][1] = static_cast<double>(y) / 8.0;
				buffers.featuresA[index][2] = 1.0;
				buffers.featuresA[index][3] = 0.5;
				buffers.featuresA[index][4] = 0.5;
				buffers.featuresA[index][5] = 1.0;
				buffers.featuresA[index][6] = 0.5;
				buffers.featuresA[index][7] = 0.6;
				buffers.featuresA[index][10] = static_cast<double>(HENYEY_GREENSTEIN)
					/ static_cast<double>(PRINCIPLED);
				buffers.featuresB[index] = buffers.featuresA[index];
			}
		}

		Denoise::NFORSettings settings;
		settings.threadCount = 1;
		std::unique_ptr<Image> denoised = Denoise::applyNFOR(buffers, settings);
		const double darkDetail = denoised->getPixel(4, 4).getRed();
		const double brightDetail = denoised->getPixel(5, 4).getRed();

		requireNearTolerance(
			darkDetail,
			0.20,
			0.02,
			"Volume denoiser blurred a converged dark density feature."
		);
		requireNearTolerance(
			brightDetail,
			0.40,
			0.02,
			"Volume denoiser blurred a converged bright density feature."
		);
	}

	void	testTinyAdaptiveDenoisedRender(void)
	{
		setRandomSeed(42);

		Scene scene;
		scene.getImage()->setWidth(2);
		scene.getImage()->setHeight(2);
		scene.getImage()->initialize();
		scene.setSampleCount(4);
		scene.setAdaptiveSampling(true);
		scene.setAdaptiveMinSamples(2);
		scene.setAdaptiveThreshold(0.5);
		scene.setAdaptiveCheckInterval(1);
		scene.setMaxLightBounces(1);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(true);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.1, 0.2, 0.3));
		scene.setRenderingThreads(1);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0), 1.0));

		require(Renderer::render(scene), "Tiny adaptive denoised render failed.");
		require(scene.hasDenoisedImage(), "Adaptive denoised render did not create a companion image.");
		const Color denoisedPixel = scene.getDenoisedImage()->getPixel(0, 0);
		require(std::isfinite(denoisedPixel.getRed()), "Adaptive denoised companion red value is non-finite.");
	}

	void	testTinyDenoisedRenderProducesCompanionImage(void)
	{
		setRandomSeed(42);

		Scene scene;
		scene.getImage()->setWidth(2);
		scene.getImage()->setHeight(2);
		scene.getImage()->initialize();
		scene.setSampleCount(1);
		scene.setAdaptiveSampling(false);
		scene.setMaxLightBounces(1);
		scene.setViewTransform(ViewTransform::Raw);
		scene.setBloom(false);
		scene.setDenoise(true);
		scene.setRenderSky(SKY_NONE);
		scene.setBackgroundColor(Color(0.1, 0.2, 0.3));
		scene.setRenderingThreads(1);
		scene.addCamera(testPinholeCamera(Vector3(0.0, 0.0, 1.0), Vector3(0.0, 0.0, -1.0), 1.0));

		require(Renderer::render(scene), "Tiny denoised render failed.");
		require(scene.hasDenoisedImage(), "Denoised render did not create a companion image.");
		require(scene.getDenoisedImage()->getWidth() == scene.getImage()->getWidth(), "Denoised image width is wrong.");
		require(scene.getDenoisedImage()->getHeight() == scene.getImage()->getHeight(), "Denoised image height is wrong.");
		requireNear(scene.getImage()->getPixel(0, 0).getRed(), 0.1, "Original render image was changed by denoising.");
		const Color denoisedPixel = scene.getDenoisedImage()->getPixel(0, 0);
		require(std::isfinite(denoisedPixel.getRed()), "Denoised companion red value is non-finite.");
		require(std::abs(denoisedPixel.getRed() - 0.1) < 0.02, "Denoised companion image value drifted too far.");
	}

	void	testCameraRejectsInvalidPhysicalOptics(void)
	{
		Camera camera;

		requireThrows([&]() { camera.setFocalLengthMeters(0.0); }, "Camera accepted zero focal length.");
		requireThrows([&]() { camera.setSensorWidthMeters(0.0); }, "Camera accepted zero sensor width.");
		requireThrows([&]() { camera.setSensorHeightMeters(0.0); }, "Camera accepted zero sensor height.");
		requireThrows([&]() { camera.setFNumber(0.0); }, "Camera accepted zero f-number.");
		requireThrows([&]() { camera.setApertureDiameterMeters(0.0); }, "Camera accepted zero aperture diameter.");
		requireThrows([&]() { camera.setFocusDistanceMeters(0.0); }, "Camera accepted zero focus distance.");
	}

	void	testSparseVolumeGridRoundTripAndInterpolation(void)
	{
		const std::filesystem::path path = std::filesystem::temp_directory_path()
			/ "luz_sparse_volume_grid_test.luzvol";
		SparseVolumeGrid::SourceBrick left;
		left.x = 0;
		left.density.fill(1.0f);
		SparseVolumeGrid::SourceBrick right;
		right.x = 1;
		right.density.fill(0.5f);
		SparseVolumeGrid::write(
			path.string(),
			{16u, 8u, 8u},
			{-4, 7, 11},
			{0.25f, 0.5f, 1.0f},
			{left, right}
		);

		const SparseVolumeGrid grid = SparseVolumeGrid::load(path.string());
		require(grid.brickCount() == 2, "Sparse volume did not preserve its nonempty bricks.");
		require(grid.dimensions() == std::array<std::uint32_t, 3>{16u, 8u, 8u}, "Sparse volume dimensions changed.");
		require(grid.indexMinimum() == std::array<std::int32_t, 3>{-4, 7, 11}, "Sparse volume index origin changed.");
		requireNear(grid.maximumDensity(), 1.0, "Sparse volume maximum density changed.");
		requireNear(
			grid.sample(Vector3(3.5 / 16.0, 3.5 / 8.0, 3.5 / 8.0)),
			1.0,
			"Sparse volume did not reconstruct a brick sample."
		);
		requireNearTolerance(
			grid.sample(Vector3(8.0 / 16.0, 3.5 / 8.0, 3.5 / 8.0)),
			0.75,
			2e-5,
			"Sparse volume interpolation did not cross a brick boundary."
		);
		requireNear(grid.interpolationBrickMaximum(0, 0, 0), 1.0, "Sparse volume interpolation majorant is wrong.");
		requireNear(grid.interpolationBrickMinimum(0, 0, 0), 0.0, "Sparse volume interpolation minimum is wrong.");
		float cachedMinimum = -1.0f;
		float cachedMaximum = -1.0f;
		grid.interpolationBrickBounds(0, 0, 0, cachedMinimum, cachedMaximum);
		requireNear(cachedMinimum, 0.0, "Combined sparse-volume interpolation minimum is wrong.");
		requireNear(cachedMaximum, 1.0, "Combined sparse-volume interpolation maximum is wrong.");
		grid.interpolationBrickBounds(-1, 0, 0, cachedMinimum, cachedMaximum);
		requireNear(cachedMinimum, 0.0, "Out-of-bounds interpolation minimum is not conservative.");
		requireNear(cachedMaximum, 1.0, "Out-of-bounds interpolation maximum lost its adjacent brick halo.");
		requireNear(grid.sample(Vector3(-0.1, 0.5, 0.5)), 0.0, "Sparse volume sampled outside its bounds.");

		GridVolumeParameters parameters;
		parameters.fileName = path.string();
		parameters.size = Vector3(16.0, 8.0, 8.0);
		parameters.extinction = 0.5;
		parameters.albedo = Color(0.2, 0.3, 0.4);
		parameters.dropletSizeMicrons = 0.0;
		SparseGridVolume volume(parameters);
		requireNear(
			volume.extinctionAt(Vector3(-4.5, -0.5, -0.5)),
			0.5,
			"Grid volume extinction did not sample the sparse field."
		);
		GridVolumeParameters remappedParameters = parameters;
		remappedParameters.densityThreshold = 0.5;
		remappedParameters.densityGamma = 2.0;
		SparseGridVolume remappedVolume(remappedParameters);
		requireNear(
			remappedVolume.densityAt(Vector3(-4.5, -0.5, -0.5)),
			1.0,
			"Grid density remap changed the asset peak."
		);
		requireNear(
			remappedVolume.densityAt(Vector3(4.5, -0.5, -0.5)),
			0.0,
			"Grid density threshold did not remove material at the cutoff."
		);
		requireNearTolerance(
			remappedVolume.densityAt(Vector3(0.0, -0.5, -0.5)),
			0.25,
			1e-6,
			"Grid density gamma did not reshape an interpolated density."
		);
		GridVolumeParameters invalidThresholdParameters = parameters;
		invalidThresholdParameters.densityThreshold = 1.0;
		requireThrows(
			[&invalidThresholdParameters]() {
				SparseGridVolume invalid(invalidThresholdParameters);
				(void)invalid;
			},
			"Grid volume accepted a unit density threshold."
		);
		GridVolumeParameters invalidGammaParameters = parameters;
		invalidGammaParameters.densityGamma = 0.0;
		requireThrows(
			[&invalidGammaParameters]() {
				SparseGridVolume invalid(invalidGammaParameters);
				(void)invalid;
			},
			"Grid volume accepted zero density gamma."
		);
		Sampler::beginPixelSample(8, 3, 0);
		Sampler::setBounce(0);
		Sampler::setFeatureSampling(true);
		Ray gridFeatureRay(Vector3(-10.0, -0.5, -0.5), Vector3(1.0, 0.0, 0.0));
		HitRecord gridFeatureHit;
		require(
			volume.hit(gridFeatureRay, gridFeatureHit, T_MIN, T_MAX),
			"Sparse grid produced no deterministic feature hit."
		);
		require(
			gridFeatureHit.u > parameters.albedo.getRed() && gridFeatureHit.u <= 1.0,
			"Sparse-grid feature hit stored albedo instead of deterministic density."
		);
		Sampler::setFeatureSampling(false);
		Sampler::endPixelSample();
		Ray shadowRay(Vector3(-10.0, -0.5, -0.5), Vector3(1.0, 0.0, 0.0));
		const Color transmittance = volume.shadowTransmittance(shadowRay, 0.0, 100.0);
		requireNearTolerance(
			transmittance.getRed(),
			std::exp(-6.0),
			2e-5,
			"Grid volume brick traversal integrated the wrong optical depth."
		);
		Ray insideRightRay(Vector3(0.0, -0.5, -0.5), Vector3(1.0, 0.0, 0.0));
		double insideEntry = -1.0;
		double insideExit = -1.0;
		require(
			volume.integrationInterval(insideRightRay, 0.0, 100.0, insideEntry, insideExit),
			"Grid volume rejected a ray whose origin is inside the medium."
		);
		requireNear(insideEntry, 0.0, "Inside-volume integration did not begin at the camera.");
		requireNear(insideExit, 8.0, "Inside-volume integration ended at the wrong boundary.");
		const Color insideRightTransmittance = volume.shadowTransmittance(
			insideRightRay,
			0.0,
			100.0
		);
		requireNearTolerance(
			insideRightTransmittance.getRed(),
			std::exp(-2.0),
			2e-5,
			"Inside-volume traversal integrated the wrong forward optical depth."
		);
		Ray insideLeftRay(Vector3(0.0, -0.5, -0.5), Vector3(-1.0, 0.0, 0.0));
		const Color insideLeftTransmittance = volume.shadowTransmittance(
			insideLeftRay,
			0.0,
			100.0
		);
		requireNearTolerance(
			insideLeftTransmittance.getRed(),
			std::exp(-4.0),
			2e-5,
			"Inside-volume traversal integrated the wrong reverse optical depth."
		);
		Ray outsideMissRay(Vector3(20.0, -0.5, -0.5), Vector3(1.0, 0.0, 0.0));
		requireNear(
			volume.shadowTransmittance(outsideMissRay, 0.0, 100.0).getRed(),
			1.0,
			"A ray pointing away from an authored volume was attenuated."
		);
		GridVolumeParameters rotatedParameters = parameters;
		rotatedParameters.rotationDegrees = Vector3(0.0, 0.0, 90.0);
		SparseGridVolume rotatedVolume(rotatedParameters);
		GridVolumeParameters invalidRotationParameters = parameters;
		invalidRotationParameters.rotationDegrees = Vector3(
			std::numeric_limits<double>::quiet_NaN(),
			0.0,
			0.0
		);
		requireThrows(
			[&invalidRotationParameters]() {
				SparseGridVolume invalid(invalidRotationParameters);
				(void)invalid;
			},
			"Grid volume accepted a non-finite rotation."
		);
		requireNear(
			rotatedVolume.extinctionAt(Vector3(0.5, -4.5, -0.5)),
			0.5,
			"Rotated grid volume did not transform world-space density lookup."
		);
		AABB rotatedBounds;
		require(rotatedVolume.createBoundingBox(rotatedBounds), "Rotated grid volume has no bounds.");
		requireVectorNear(rotatedBounds.getMinimum(), Vector3(-4.0, -8.0, -4.0), "Rotated grid minimum bounds");
		requireVectorNear(rotatedBounds.getMaximum(), Vector3(4.0, 8.0, 4.0), "Rotated grid maximum bounds");
		Ray rotatedShadowRay(Vector3(0.5, -10.0, -0.5), Vector3(0.0, 1.0, 0.0));
		requireNearTolerance(
			rotatedVolume.shadowTransmittance(rotatedShadowRay, 0.0, 100.0).getRed(),
			std::exp(-6.0),
			2e-5,
			"Rotated grid brick traversal changed optical depth."
		);
		double originalDepth = 0.0;
		double rotatedDepth = 0.0;
		require(
			volume.directionalOpticalDepth(
				Vector3(-4.5, -0.5, -0.5),
				Vector3(1.0, 0.0, 0.0),
				originalDepth
			),
			"Unrotated grid did not build directional optical depth."
		);
		require(
			rotatedVolume.directionalOpticalDepth(
				Vector3(0.5, -4.5, -0.5),
				Vector3(0.0, 1.0, 0.0),
				rotatedDepth
			),
			"Rotated grid did not build directional optical depth."
		);
		requireNearTolerance(
			rotatedDepth,
			originalDepth,
			1e-6,
			"Rotation changed equivalent cached directional optical depth."
		);
		GridVolumeParameters ratioParameters = parameters;
		ratioParameters.extinction = 1.0 / 12.0;
		SparseGridVolume ratioVolume(ratioParameters);
		double ratioMean = 0.0;
		constexpr std::uint32_t ratioSampleCount = 4096;
		for (std::uint32_t sample = 0; sample < ratioSampleCount; sample++)
		{
			Sampler::beginPixelSample(17, 23, sample);
			Ray ratioRay(Vector3(-10.0, -0.5, -0.5), Vector3(1.0, 0.0, 0.0));
			ratioMean += ratioVolume.shadowTransmittance(ratioRay, 0.0, 100.0).getRed();
			Sampler::endPixelSample();
		}
		ratioMean /= static_cast<double>(ratioSampleCount);
		requireNearTolerance(
			ratioMean,
			std::exp(-1.0),
			0.03,
			"Grid volume residual ratio tracking did not converge to Beer-Lambert transmittance."
		);

		const std::filesystem::path uniformPath = std::filesystem::temp_directory_path()
			/ "luz_sparse_volume_uniform_control_test.luzvol";
		std::vector<SparseVolumeGrid::SourceBrick> uniformBricks;
		for (std::int32_t z = 0; z < 3; z++)
			for (std::int32_t y = 0; y < 3; y++)
				for (std::int32_t x = 0; x < 3; x++)
				{
					SparseVolumeGrid::SourceBrick brick;
					brick.x = x;
					brick.y = y;
					brick.z = z;
					brick.density.fill(1.0f);
					uniformBricks.push_back(brick);
				}
		SparseVolumeGrid::write(
			uniformPath.string(),
			{24u, 24u, 24u},
			{0, 0, 0},
			{1.0f, 1.0f, 1.0f},
			uniformBricks
		);
		const SparseVolumeGrid uniformGrid = SparseVolumeGrid::load(uniformPath.string());
		requireNear(
			uniformGrid.interpolationBrickMinimum(1, 1, 1),
			1.0,
			"Uniform interior brick did not preserve its conservative density minimum."
		);
		GridVolumeParameters uniformParameters;
		uniformParameters.fileName = uniformPath.string();
		uniformParameters.size = Vector3(24.0, 24.0, 24.0);
		uniformParameters.extinction = 0.25;
		uniformParameters.dropletSizeMicrons = 0.0;
		uniformParameters.primaryDetail = 4.0;
		SparseGridVolume uniformVolume(uniformParameters);
		for (std::uint32_t sample = 0; sample < 32; sample++)
		{
			Sampler::beginPixelSample(5, 7, sample);
			Ray uniformRay(Vector3(-2.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0));
			const double estimate = uniformVolume.shadowTransmittance(
				uniformRay,
				0.0,
				4.0
			).getRed();
			Sampler::endPixelSample();
			requireNearTolerance(
				estimate,
				std::exp(-1.0),
				1e-12,
				"Residual ratio tracking was not zero-variance in a uniform interior."
			);
		}
		auto numericalFullShadow = [&uniformVolume](const Ray& sourceRay)
		{
			double entry = 0.0;
			double exit = 0.0;
			require(
				uniformVolume.integrationInterval(sourceRay, 0.0, 100.0, entry, exit),
				"Directional cache reference ray missed the uniform medium."
			);
			entry = std::max(0.0, entry);
			constexpr int integrationSteps = 32768;
			const double stepT = (exit - entry) / static_cast<double>(integrationSteps);
			const double rayLength = Utilities::vectorLength(sourceRay.getDirection());
			double opticalDepth = 0.0;
			for (int step = 0; step < integrationSteps; step++)
			{
				const double t = entry + (static_cast<double>(step) + 0.5) * stepT;
				opticalDepth += uniformVolume.extinctionAt(sourceRay.pointAtRay(t))
					* stepT * rayLength;
			}
			return (std::exp(-opticalDepth));
		};
		auto cachedFullShadow = [&uniformVolume](Ray ray, std::uint32_t sample)
		{
			Sampler::beginPixelSample(211, 19, sample);
			Sampler::setVolumeControlSampling(true);
			const double transmittance = uniformVolume.shadowTransmittance(
				ray,
				0.0,
				100.0
			).getRed();
			Sampler::setVolumeControlSampling(false);
			Sampler::endPixelSample();
			return (transmittance);
		};
		auto cachedDirectionalFullShadow = [&uniformVolume](Ray ray, std::uint32_t sample)
		{
			Sampler::beginPixelSample(212, 19, sample);
			require(
				!Sampler::isDirectionalShadowSampling(),
				"Directional shadow state leaked between pixel samples."
			);
			Sampler::setDirectionalShadowSampling(true);
			const double transmittance = uniformVolume.shadowTransmittance(
				ray,
				0.0,
				100.0
			).getRed();
			Sampler::setDirectionalShadowSampling(false);
			Sampler::endPixelSample();
			return (transmittance);
		};
		Ray cachedPositiveRay(Vector3(-2.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0));
		const double cachedPositive = cachedFullShadow(cachedPositiveRay, 0);
		double exposedPositiveDepth = 0.0;
		require(
			uniformVolume.directionalOpticalDepth(
				Vector3(-2.0, 0.0, 0.0),
				Vector3(1.0, 0.0, 0.0),
				exposedPositiveDepth
			),
			"Sparse volume did not expose its directional optical depth."
		);
		requireNearTolerance(
			exposedPositiveDepth,
			3.5,
			0.02,
			"Exposed directional optical depth changed the cached integral."
		);
		requireNearTolerance(
			cachedPositive,
			numericalFullShadow(cachedPositiveRay),
			0.006,
			"Directional optical-depth cache changed a positive-axis shadow."
		);
		requireNearTolerance(
			cachedFullShadow(cachedPositiveRay, 1),
			cachedPositive,
			1e-12,
			"Directional optical-depth cache was not stable across repeated queries."
		);
		requireNearTolerance(
			cachedDirectionalFullShadow(cachedPositiveRay, 17),
			cachedPositive,
			1e-12,
			"Directional-light shadow sampling did not use the optical-depth cache."
		);
		requireNearTolerance(
			cachedDirectionalFullShadow(cachedPositiveRay, 29),
			cachedPositive,
			1e-12,
			"Directional-light cache result changed with the stochastic sample."
		);
		Ray cachedNegativeRay(Vector3(2.0, 0.0, 0.0), Vector3(-1.0, 0.0, 0.0));
		requireNearTolerance(
			cachedFullShadow(cachedNegativeRay, 2),
			numericalFullShadow(cachedNegativeRay),
			0.006,
			"Directional optical-depth cache changed an opposite-direction shadow."
		);
		const Vector3 diagonalDirection(1.0, 0.37, -0.22);
		Ray cachedDiagonalRay(
			Vector3(0.0, 0.0, 0.0),
			diagonalDirection / Utilities::vectorLength(diagonalDirection)
		);
		requireNearTolerance(
			cachedFullShadow(cachedDiagonalRay, 3),
			numericalFullShadow(cachedDiagonalRay),
			0.012,
			"Directional optical-depth cache changed a diagonal inside-volume shadow."
		);
		Sampler::beginPixelSample(213, 19, 0);
		Sampler::setDirectionalShadowSampling(true);
		Ray truncatedControlRay(Vector3(-2.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0));
		const double truncatedControl = uniformVolume.shadowTransmittance(
			truncatedControlRay,
			0.0,
			4.0
		).getRed();
		Sampler::setDirectionalShadowSampling(false);
		Sampler::endPixelSample();
		requireNearTolerance(
			truncatedControl,
			std::exp(-1.0),
			1e-12,
			"Directional cache incorrectly replaced a finite directional-light segment."
		);

		const std::filesystem::path smoothPath = std::filesystem::temp_directory_path()
			/ "luz_sparse_volume_smooth_control_test.luzvol";
		std::vector<SparseVolumeGrid::SourceBrick> smoothBricks;
		for (std::int32_t z = 0; z < 3; z++)
			for (std::int32_t y = 0; y < 3; y++)
				for (std::int32_t x = 0; x < 3; x++)
				{
					SparseVolumeGrid::SourceBrick brick;
					brick.x = x;
					brick.y = y;
					brick.z = z;
					for (std::uint32_t localZ = 0; localZ < SparseVolumeGrid::BRICK_EDGE; localZ++)
						for (std::uint32_t localY = 0; localY < SparseVolumeGrid::BRICK_EDGE; localY++)
							for (std::uint32_t localX = 0; localX < SparseVolumeGrid::BRICK_EDGE; localX++)
							{
								const std::size_t index = static_cast<std::size_t>(
									(localZ * SparseVolumeGrid::BRICK_EDGE + localY)
										* SparseVolumeGrid::BRICK_EDGE + localX
								);
								brick.density[index] = 0.75f + 0.10f
									* static_cast<float>(localX)
									/ static_cast<float>(SparseVolumeGrid::BRICK_EDGE - 1u);
							}
					smoothBricks.push_back(brick);
				}
		SparseVolumeGrid::write(
			smoothPath.string(),
			{24u, 24u, 24u},
			{0, 0, 0},
			{1.0f, 1.0f, 1.0f},
			smoothBricks
		);
		GridVolumeParameters smoothParameters = uniformParameters;
		smoothParameters.fileName = smoothPath.string();
		SparseGridVolume smoothVolume(smoothParameters);
		constexpr std::uint32_t varianceSampleCount = 4096;
		double residualSum = 0.0;
		double residualSquareSum = 0.0;
		double ordinarySum = 0.0;
		double ordinarySquareSum = 0.0;
		for (std::uint32_t sample = 0; sample < varianceSampleCount; sample++)
		{
			Sampler::beginPixelSample(101, 7, sample);
			Ray residualRay(Vector3(-2.0, 0.0, 0.0), Vector3(1.0, 0.0, 0.0));
			const double residualEstimate = smoothVolume.shadowTransmittance(
				residualRay,
				0.0,
				4.0
			).getRed();
			Sampler::endPixelSample();
			residualSum += residualEstimate;
			residualSquareSum += residualEstimate * residualEstimate;

			Sampler::beginPixelSample(103, 7, sample);
			constexpr double densityMajorant = 0.85;
			const double rate = smoothParameters.extinction * densityMajorant;
			double eventT = 0.0;
			double ordinaryEstimate = 1.0;
			for (std::uint32_t event = 0; event < 1024; event++)
			{
				eventT += -std::log(std::max(
					1e-12,
					1.0 - Sampler::sample1D(0x72000000u + event)
				)) / rate;
				if (eventT >= 4.0)
					break;
				ordinaryEstimate *= std::clamp(
					1.0 - smoothVolume.densityAt(Vector3(-2.0 + eventT, 0.0, 0.0))
						/ densityMajorant,
					0.0,
					1.0
				);
			}
			Sampler::endPixelSample();
			ordinarySum += ordinaryEstimate;
			ordinarySquareSum += ordinaryEstimate * ordinaryEstimate;
		}
		const double residualMean = residualSum / varianceSampleCount;
		const double ordinaryMean = ordinarySum / varianceSampleCount;
		const double residualVariance = residualSquareSum / varianceSampleCount
			- residualMean * residualMean;
		const double ordinaryVariance = ordinarySquareSum / varianceSampleCount
			- ordinaryMean * ordinaryMean;
		double opticalDepth = 0.0;
		constexpr int referenceSteps = 4096;
		for (int step = 0; step < referenceSteps; step++)
		{
			const double t = (static_cast<double>(step) + 0.5)
				* 4.0 / static_cast<double>(referenceSteps);
			opticalDepth += smoothParameters.extinction
				* smoothVolume.densityAt(Vector3(-2.0 + t, 0.0, 0.0))
				* 4.0 / static_cast<double>(referenceSteps);
		}
		const double smoothExpected = std::exp(-opticalDepth);
		requireNearTolerance(
			residualMean,
			smoothExpected,
			0.01,
			"Residual ratio tracking changed the expected smooth-medium transmittance."
		);
		requireNearTolerance(
			ordinaryMean,
			smoothExpected,
			0.025,
			"Ordinary ratio-tracking comparison did not converge."
		);
		require(
			residualVariance < ordinaryVariance * 0.1,
			"Residual ratio tracking did not materially reduce smooth-medium variance."
		);

		const std::filesystem::path scenePath = std::filesystem::temp_directory_path()
			/ "luz_sparse_volume_grid_scene_test.luz";
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[settings]\n"
				<< "meters_per_unit=2\n\n"
				<< "[scene]\n"
				<< "volume_grid authored_cloud {\n"
				<< "file=" << path.filename().string() << "\n"
				<< "position=(1,2,3)\n"
				<< "rotation=(0,0,90)\n"
				<< "size=(16,8,8)\n"
				<< "extinction=0.25\n"
				<< "density_threshold=0.1\n"
				<< "density_contrast=1.25\n"
				<< "albedo=(0.98,0.99,1.0)\n"
				<< "droplet_size=18\n"
				<< "scatter_falloff=0.8\n"
				<< "scatter_compensation=0.6\n"
				<< "quality=final\n"
				<< "}\n";
		}
		Scene scene;
		SceneFile::read(scene, scenePath.string());
		require(scene.getHittables().size() == 1, "Grid volume scene did not load one volume.");
		const std::shared_ptr<SparseGridVolume> parsed = std::dynamic_pointer_cast<SparseGridVolume>(scene.getHittables()[0]);
		require(parsed != nullptr, "volume_grid did not create a SparseGridVolume.");
		requireVectorNear(parsed->getParameters().position, Vector3(1.0, 2.0, 3.0), "Grid volume position");
		requireVectorNear(parsed->getParameters().rotationDegrees, Vector3(0.0, 0.0, 90.0), "Grid volume rotation");
		require(parsed->getParameters().shadowSamplesPerBrick == 12, "Final grid volume shadow quality was not applied.");
		requireNear(parsed->getParameters().primaryDetail, 8.0, "Final grid volume primary detail was not applied.");
		requireNear(parsed->getParameters().densityThreshold, 0.1, "Grid volume density threshold was not parsed.");
		requireNear(parsed->getParameters().densityGamma, 1.25, "Grid volume density contrast alias was not parsed.");
		requireNear(
			parsed->extinctionAt(Vector3(1.5, -2.5, 2.5)),
			0.5,
			"Remapped grid volume did not inherit meters_per_unit."
		);
		const HenyeyGreenstein* phase = dynamic_cast<const HenyeyGreenstein*>(parsed->getMaterial());
		require(phase != nullptr && phase->usesDropletPhase(), "Grid volume did not enable the droplet phase model.");
		requireNear(phase->getDropletSizeMicrons(), 18.0, "Grid volume droplet size was not parsed.");
		{
			std::ofstream sceneStream(scenePath);
			sceneStream
				<< "[scene]\n"
				<< "volume_grid reference_cloud {\n"
				<< "file=" << path.filename().string() << "\n"
				<< "quality=reference\n"
				<< "}\n";
		}
		Scene referenceScene;
		SceneFile::read(referenceScene, scenePath.string());
		const std::shared_ptr<SparseGridVolume> referenceVolume
			= std::dynamic_pointer_cast<SparseGridVolume>(referenceScene.getHittables()[0]);
		require(referenceVolume != nullptr, "Reference grid quality did not create a sparse volume.");
		require(
			referenceVolume->getParameters().shadowSamplesPerBrick == 16,
			"Reference grid volume shadow quality was not applied."
		);
		requireNear(
			referenceVolume->getParameters().primaryDetail,
			16.0,
			"Reference grid volume primary detail was not applied."
		);
		std::filesystem::remove(scenePath);
		std::filesystem::remove(smoothPath);
		std::filesystem::remove(uniformPath);
		std::filesystem::remove(path);
	}

	void	testVolumeGuidingFieldTrainingAndSampling(void)
	{
		requireThrows(
			[]() {
				VolumeGuidingField invalid(
					Vector3(0.0, 0.0, 0.0),
					Vector3(0.0, 1.0, 1.0),
					2
				);
			},
			"Volume guiding accepted empty spatial bounds."
		);
		requireThrows(
			[]() {
				VolumeGuidingField invalid(
					Vector3(-1.0, -1.0, -1.0),
					Vector3(1.0, 1.0, 1.0),
					0
				);
			},
			"Volume guiding accepted zero spatial resolution."
		);

		const Vector3 minimum(-1.0, -1.0, -1.0);
		const Vector3 maximum(1.0, 1.0, 1.0);
		const Vector3 position(0.5, 0.5, 0.5);
		const Vector3 brightDirection(1.0, 0.0, 0.0);
		const Vector3 dimDirection(-1.0, 0.0, 0.0);
		VolumeGuidingField forward(minimum, maximum, 2, 16, 0.8);
		VolumeGuidingField reverse(minimum, maximum, 2, 16, 0.8);
		require(!forward.isFrozen(), "A new volume guide was unexpectedly frozen.");
		require(forward.cellCount() == 8, "Volume guide allocated the wrong number of cells.");
		require(forward.memoryBytes() > 8 * 16 * sizeof(double),
			"Volume guide memory accounting is too small.");

		for (int sample = 0; sample < 100; sample++)
			forward.record(position, brightDirection, 1.0);
		for (int sample = 0; sample < 5; sample++)
			forward.record(position, dimDirection, 1.0);
		for (int sample = 0; sample < 5; sample++)
			reverse.record(position, dimDirection, 1.0);
		for (int sample = 0; sample < 100; sample++)
			reverse.record(position, brightDirection, 1.0);
		forward.record(Vector3(4.0, 4.0, 4.0), dimDirection, 100000.0);
		forward.freeze(0.01);
		reverse.freeze(0.01);
		require(forward.isFrozen(), "Volume guide did not freeze after training.");
		requireThrows(
			[&]() { VolumeGuidingField copy(minimum, maximum, 1); copy.freeze(0.0); },
			"Volume guide accepted a zero training prior."
		);

		const double brightPDF = forward.pdf(position, brightDirection);
		const double dimPDF = forward.pdf(position, dimDirection);
		require(std::isfinite(brightPDF) && brightPDF > 0.0,
			"Learned volume guide returned an invalid PDF.");
		require(brightPDF > dimPDF * 2.0,
			"Volume guide did not learn the dominant incident direction.");
		requireNearTolerance(
			brightPDF,
			reverse.pdf(position, brightDirection),
			1e-12,
			"Volume guide training changed with record order."
		);
		requireNear(
			forward.pdf(Vector3(4.0, 4.0, 4.0), brightDirection),
			0.0,
			"Volume guide evaluated outside its bounds."
		);

		const VolumeGuidingField::Sample guided = forward.sample(
			position,
			0.5,
			Sampler::Sample2D{0.25f, 0.75f}
		);
		require(guided.valid, "Frozen volume guide did not produce a direction sample.");
		requireNearTolerance(
			Utilities::vectorLengthSquared(guided.direction),
			1.0,
			1e-10,
			"Volume guide sampled a non-unit direction."
		);
		requireNearTolerance(
			guided.pdf,
			forward.pdf(position, guided.direction),
			1e-12,
			"Volume guide sample and evaluation PDFs disagree."
		);
		require(
			!forward.sample(Vector3(4.0, 4.0, 4.0), 0.5, {0.5f, 0.5f}).valid,
			"Volume guide sampled outside its bounds."
		);

		const double frozenPDF = forward.pdf(position, brightDirection);
		forward.record(position, dimDirection, 100000.0);
		forward.freeze(10.0);
		requireNearTolerance(
			forward.pdf(position, brightDirection),
			frozenPDF,
			1e-12,
			"Frozen volume guide changed after additional training."
		);
	}
}

int	main(void)
{
	try
	{
		testColorMath();
		testViewTransformBlackPixel();
		testACESViewTransformCompressesHighlights();
		testStandardViewTransformClipsForDisplay();
		testRawViewTransformKeepsSceneLinearData();
		testSRGBGammaCorrection();
		testColorManagementACEScgContract();
		testACEScgSceneImageEncodesToSRGB();
		testExposureAndContrast();
		testPhotographicExposureConversion();
		testBloomExtractionPreservesHighlightColor();
		testBloomExtractionSuppressesIsolatedFireflies();
		testDisplayFireflySuppressionRemovesIsolatedWhitePixels();
		testPhysicalLightUnitConversions();
		testIESProfileLoadsPhysicalLampData();
		testSpectralColorConversions();
		testSpectralReflectanceCurveConversion();
		testGaussianBlurSupportsInPlaceSmallImages();
		testGaussianBlurPreservesCenteredEnergyAndEdges();
		testTerminalFilePath();
		testNonSquareBMP();
		testNonSquareTIFF();
		testNonSquarePNG();
		testSceneFileOutputName();
		testSceneFileTiffOutputName();
		testSceneFilePNGOutputSettings();
		testSceneFileDenoiseOutputName();
		testSceneFileUsesSceneDefaults();
		testSceneFileAdaptiveSettings();
		testSceneFileVolumeGuidingSettings();
		testSceneFilePostProcessSettings();
		testSceneFileCausticSettings();
		testSceneFilePhotographicExposureSetting();
		testSceneFileCameraPhotographicExposure();
		testSceneFileRejectsInvalidSettings();
		testSceneFileAtmosphereSetting();
		testSceneFileDirectionalSunDrivesAtmosphere();
		testSceneFileBackgroundSetting();
		testEnvironmentMapLoadsPPM();
		testEnvironmentMapPhysicalMetrics();
		testSceneEnvironmentPhysicalCalibration();
		testEnvironmentMapLoadsHDR();
		testEnvironmentMapLoadsRLEHDR();
		testSceneFileEnvironmentSetting();
		testSceneFileAtmosphereCombinesWithEnvironment();
		testSceneFileLoadsPhysicalCamera();
		testRenderCameraFitsSensorGateToImageAspect();
		testSceneFileRejectsCompactCameraSyntax();
		testSceneFileLoadsRelativeObject();
		testSceneFileCachesRepeatedMeshFileLoads();
		testAssetPathUsesProvidedScenePaths();
		testSceneFileLoadsTransformedObject();
		testSceneFileLoadsNamedBlocks();
		testSceneFileCompactPrimitivesUseNamedMaterials();
		testSceneFileRectangleAxesAndUVs();
		testSceneFilePhysicalLightUnits();
		testSceneFileSolarDirectionalLightPreset();
		testSceneFileLoadsIESPointLight();
		testSceneFileMetersPerUnitScalesPhysicalLightArea();
		testSceneFileSpectralColorValues();
		testSceneFileLoadsNamedReflectanceSpectra();
		testSceneFileExplicitColorSpaces();
		testSceneFileLoadsAbsorbingDielectricMaterial();
		testSceneFileLoadsConductorMetalMaterial();
		testSceneFileLoadsMeasuredMaterialPresets();
		testSceneFileLoadsMeasuredVolumeData();
		testSphereUVProjectionModes();
		testSphereHitTracksFrontFace();
		testDielectricUsesExitRefractionRatio();
		testDielectricBeerLambertAbsorption();
		testMetalConductorFresnel();
		testRoughMetalUsesGGXBSDF();
		testGlossyUsesGGXBSDF();
		testDiffuseGlossyMixPreservesStrongGlossyComponent();
		testRoughDielectricUsesGGXBSDF();
		testPrincipledLayeredBSDF();
		testSmoothTransmissivePrincipledUsesDeltaScatter();
		testPrincipledSubsurfaceControls();
		testSceneFileLoadsNamedTexturedSphere();
		testSceneFileLoadsVolumeBlock();
		testSceneFileLoadsProceduralCloudBlock();
		testSceneFileCloudAliasesAndQualityPrecedence();
		testSceneFileMetersPerUnitScalesVolumeDensity();
		testCausticPhotonMapBuildsAndEstimates();
		testSceneFileLoadsNonMetallicPrincipledMaterial();
		testSceneFileLoadsGlossyMaterials();
		testSceneFileRejectsInvalidMaterial();
		testSceneFileRejectsAmbiguousMeasuredData();
		testBVHReturnsClosestHit();
		testVolumeHitWorksFromInsideBoundary();
		testBoxVolumeHitWorksFromInsideBoundary();
		testProceduralCloudDensityAndDeltaTracking();
		testVolumeShadowTransmittance();
		testTinyTriangleHitAndNormal();
		testTrianglePDFAndRandomSampling();
		testTriangleInterpolatesVertexNormals();
		testOpaqueSmoothNormalDoesNotReceiveBacksideLight();
		testOBJReaderLoadsVertexNormals();
		testSceneFileLoadsTexturedOBJUVs();
		testMeshInstanceSharesGeometryAndTransformsHits();
		testMeshPDFAndRandomSampling();
		testAABBDefaultBounds();
		testAABBHandlesAxisParallelBoundaryRays();
		testRectangleBoundingBoxes();
		testRectangleRandomSamplesSupportedAxes();
		testRectangleTextureCoordinates();
		testCubeBoundingBoxAndSetters();
		testHittablePDFAveragesMultipleLights();
		testFlagsParsePostProcessOptions();
		testFlagsParseDenoiseOptions();
		testFlagsParseCausticOptions();
		testFlagsParsePNGOutput();
		testFlagsParseBenchmarkFileOptions();
		testFlagsParsePositionalSceneFile();
		testFlagsParseAdaptiveOptions();
		testFlagsRejectInvalidValues();
		testSettersRejectInvalidValues();
		testPlanetaryHitRobustIntersections();
		testAtmosphereIncidentLightInsideOutsideAndSunPosition();
		testAtmosphereSegmentTransmittance();
		testAtmosphereDiffuseSkyRadianceCache();
		testAtmosphereMetersPerUnitPreservesPhysicalScale();
		testAtmospherePrimaryHitCompositesSurface();
		testSceneDefaultsEnableAdaptiveAndDenoise();
		testRenderedSceneHasBasicVisualStructure();
		testRenderedScenePostProcessingProducesDisplayableImage();
		testRenderedBMPContainsVisualSignal();
		testGlossySurfaceReflectsEmissiveLight();
		testConstantBackgroundDirectlyLightsSurfaces();
		testConstantBackgroundDoesNotDirectlySampleTransmissiveBSDF();
		testTinyRender();
		testAtmosphereCompositesEnvironmentBackground();
		testTinyAdaptiveRender();
		testAdaptiveVolumeMinimumSamples();
		testNFORDoesNotSpreadIsolatedFirefly();
		testNFORVolumeResidualReconstructionRemovesCheckerVariance();
		testNFORVolumeDensityGuidePreservesBillowBoundary();
		testNFORPreservesConvergedVolumeDetail();
		testTinyAdaptiveDenoisedRender();
		testTinyDenoisedRenderProducesCompanionImage();
		testCameraRejectsInvalidPhysicalOptics();
		testSparseVolumeGridRoundTripAndInterpolation();
		testVolumeGuidingFieldTrainingAndSampling();
		testEnvironmentBackgroundRender();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Test failed: " << exception.what() << std::endl;
		return (1);
	}

	std::cout << "All tests passed." << std::endl;
	return (0);
}
