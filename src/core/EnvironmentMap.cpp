#include "EnvironmentMap.hpp"
#include "ColorManagement.hpp"
#include "Defaults.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace
{
	const double	FULL_SPHERE_PDF = 1.0 / (4.0 * D_PI);

	std::string	readPPMToken(std::istream& stream)
	{
		std::string token;
		char c;

		while (stream.get(c))
		{
			if (std::isspace(static_cast<unsigned char>(c)))
			{
				continue;
			}
			if (c == '#')
			{
				stream.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
				continue;
			}
			token.push_back(c);
			break;
		}
		while (stream.get(c))
		{
			if (std::isspace(static_cast<unsigned char>(c)))
			{
				break;
			}
			token.push_back(c);
		}
		return (token);
	}

	double	clamp01(double value)
	{
		return (std::max(0.0, std::min(1.0, value)));
	}

	double	clampSample(double value)
	{
		if (!std::isfinite(value) || value <= 0.0)
		{
			return (0.0);
		}
		if (value >= 1.0)
		{
			return (std::nextafter(1.0, 0.0));
		}
		return (value);
	}

	double	wrap01(double value)
	{
		value = value - std::floor(value);
		if (value < 0.0)
		{
			value += 1.0;
		}
		return (value);
	}

	std::size_t	wrapIndex(long long index, std::size_t count)
	{
		const long long signedCount = static_cast<long long>(count);

		index %= signedCount;
		if (index < 0)
		{
			index += signedCount;
		}
		return (static_cast<std::size_t>(index));
	}

	std::string	lowerCopy(std::string value)
	{
		Utilities::toLower(value);

		return (value);
	}

	bool	endsWith(std::string value, const std::string& ending)
	{
		value = lowerCopy(value);

		return (Utilities::stringEndsWith(value, ending));
	}

	Color	sanitizeRadiance(const Color& color)
	{
		return (Color(
			std::isfinite(color.getRed()) ? std::max(0.0, color.getRed()) : 0.0,
			std::isfinite(color.getGreen()) ? std::max(0.0, color.getGreen()) : 0.0,
			std::isfinite(color.getBlue()) ? std::max(0.0, color.getBlue()) : 0.0
		));
	}

	Color	rgbeToColor(unsigned char red, unsigned char green, unsigned char blue, unsigned char exponent)
	{
		if (exponent == 0)
		{
			return (Color(0.0, 0.0, 0.0));
		}

		const double scale = std::ldexp(1.0, static_cast<int>(exponent) - (128 + 8));

		return (sanitizeRadiance(ColorManagement::acescgFromLinearSRGB(Color(red * scale, green * scale, blue * scale))));
	}

	bool	parseResolutionToken(
		const std::string& axis,
		int value,
		std::size_t& width,
		std::size_t& height,
		bool& xPositive,
		bool& yPositive
	)
	{
		if (axis.size() != 2 || value <= 0 || (axis[0] != '+' && axis[0] != '-'))
		{
			return (false);
		}
		const char coordinate = static_cast<char>(std::toupper(static_cast<unsigned char>(axis[1])));
		if (coordinate == 'X')
		{
			width = static_cast<std::size_t>(value);
			xPositive = axis[0] == '+';
			return (true);
		}
		if (coordinate == 'Y')
		{
			height = static_cast<std::size_t>(value);
			yPositive = axis[0] == '+';
			return (true);
		}
		return (false);
	}

	void	parseHDRResolution(
		const std::string& line,
		std::size_t& width,
		std::size_t& height,
		bool& xPositive,
		bool& yPositive
	)
	{
		std::istringstream stream(line);
		std::string axisA;
		std::string axisB;
		int valueA = 0;
		int valueB = 0;

		width = 0;
		height = 0;
		xPositive = true;
		yPositive = false;
		if (!(stream >> axisA >> valueA >> axisB >> valueB))
		{
			throw std::runtime_error("Invalid HDR environment resolution line: " + line);
		}
		if (
			!parseResolutionToken(axisA, valueA, width, height, xPositive, yPositive)
			|| !parseResolutionToken(axisB, valueB, width, height, xPositive, yPositive)
			|| width == 0
			|| height == 0
		)
		{
			throw std::runtime_error("Unsupported HDR environment resolution line: " + line);
		}
	}

	void	readExact(std::istream& stream, unsigned char* buffer, std::size_t count, const std::string& message)
	{
		if (!stream.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(count)))
		{
			throw std::runtime_error(message);
		}
	}

	std::vector<Color>	readOldHDRScanline(
		std::istream& stream,
		std::size_t width,
		const unsigned char* firstPixel,
		bool hasFirstPixel
	)
	{
		std::vector<Color> scanline;
		scanline.reserve(width);

		if (hasFirstPixel)
		{
			scanline.push_back(rgbeToColor(firstPixel[0], firstPixel[1], firstPixel[2], firstPixel[3]));
		}
		while (scanline.size() < width)
		{
			unsigned char rgbe[4];
			readExact(stream, rgbe, 4, "Truncated HDR environment scanline.");
			scanline.push_back(rgbeToColor(rgbe[0], rgbe[1], rgbe[2], rgbe[3]));
		}
		return (scanline);
	}

	std::vector<Color>	readRLEHDRScanline(std::istream& stream, std::size_t width)
	{
		std::vector<unsigned char> channels(width * 4);

		for (std::size_t channel = 0; channel < 4; channel++)
		{
			std::size_t position = 0;
			while (position < width)
			{
				unsigned char code;
				readExact(stream, &code, 1, "Truncated HDR environment RLE packet.");
				if (code > 128)
				{
					const std::size_t count = static_cast<std::size_t>(code - 128);
					unsigned char value;
					if (count == 0 || position + count > width)
					{
						throw std::runtime_error("Invalid HDR environment RLE run.");
					}
					readExact(stream, &value, 1, "Truncated HDR environment RLE run.");
					std::fill(
						channels.begin() + static_cast<std::ptrdiff_t>(channel * width + position),
						channels.begin() + static_cast<std::ptrdiff_t>(channel * width + position + count),
						value
					);
					position += count;
				}
				else
				{
					const std::size_t count = static_cast<std::size_t>(code);
					if (count == 0 || position + count > width)
					{
						throw std::runtime_error("Invalid HDR environment RLE literal.");
					}
					readExact(
						stream,
						channels.data() + channel * width + position,
						count,
						"Truncated HDR environment RLE literal."
					);
					position += count;
				}
			}
		}

		std::vector<Color> scanline;
		scanline.reserve(width);
		for (std::size_t x = 0; x < width; x++)
		{
			scanline.push_back(rgbeToColor(
				channels[x],
				channels[width + x],
				channels[(2 * width) + x],
				channels[(3 * width) + x]
			));
		}
		return (scanline);
	}

	double	colorLuminance(const Color& color)
	{
		return (Utilities::luminance(sanitizeRadiance(color)));
	}

	double	horizontalCosine(std::size_t y, std::size_t height)
	{
		const double v = 1.0 - ((static_cast<double>(y) + 0.5) / static_cast<double>(height));

		return (std::max(0.0, std::sin((v - 0.5) * D_PI)));
	}
}

EnvironmentMap::EnvironmentMap(void)
{
	this->_width = 0;
	this->_height = 0;
	this->_averageRadiance = Color(0.0, 0.0, 0.0);
	this->_totalWeight = 0.0;
	this->_horizontalIrradiance = 0.0;
}

EnvironmentMap::EnvironmentMap(std::size_t width, std::size_t height, std::vector<Color> pixels)
{
	if (width == 0 || height == 0 || pixels.size() != width * height)
	{
		throw std::invalid_argument("Environment map dimensions must match the pixel count.");
	}
	this->_width = width;
	this->_height = height;
	this->_pixels = std::move(pixels);
	this->_averageRadiance = Color(0.0, 0.0, 0.0);
	this->_totalWeight = 0.0;
	this->_horizontalIrradiance = 0.0;
	this->buildDistribution();
}

EnvironmentMap	EnvironmentMap::load(const std::string& fileName)
{
	if (endsWith(fileName, ".hdr") || endsWith(fileName, ".pic"))
	{
		return (loadHDR(fileName));
	}
	if (endsWith(fileName, ".ppm"))
	{
		return (loadPPM(fileName));
	}
	throw std::runtime_error("Unsupported environment map format. Use PPM or Radiance HDR: " + fileName);
}

EnvironmentMap	EnvironmentMap::loadPPM(const std::string& fileName)
{
	std::ifstream stream(fileName, std::ios::binary);

	if (!stream)
	{
		throw std::runtime_error("Environment map could not be opened: " + fileName);
	}

	const std::string magic = readPPMToken(stream);
	if (magic != "P6" && magic != "P3")
	{
		throw std::runtime_error("Unsupported PPM environment format: " + fileName);
	}

	const std::size_t width = static_cast<std::size_t>(std::stoul(readPPMToken(stream)));
	const std::size_t height = static_cast<std::size_t>(std::stoul(readPPMToken(stream)));
	const double maxValue = std::stod(readPPMToken(stream));
	if (width == 0 || height == 0 || maxValue <= 0.0 || !std::isfinite(maxValue))
	{
		throw std::runtime_error("Invalid PPM environment header: " + fileName);
	}

	std::vector<Color> pixels;
	pixels.reserve(width * height);
	if (magic == "P6")
	{
		for (std::size_t i = 0; i < width * height; i++)
		{
			unsigned char rgb[3];
			readExact(stream, rgb, 3, "Truncated PPM environment: " + fileName);
			pixels.push_back(sanitizeRadiance(ColorManagement::acescgFromSRGB(Color(
				static_cast<double>(rgb[0]) / maxValue,
				static_cast<double>(rgb[1]) / maxValue,
				static_cast<double>(rgb[2]) / maxValue
			))));
		}
	}
	else
	{
		for (std::size_t i = 0; i < width * height; i++)
		{
			pixels.push_back(sanitizeRadiance(ColorManagement::acescgFromSRGB(Color(
				std::stod(readPPMToken(stream)) / maxValue,
				std::stod(readPPMToken(stream)) / maxValue,
				std::stod(readPPMToken(stream)) / maxValue
			))));
		}
	}

	return (EnvironmentMap(width, height, std::move(pixels)));
}

EnvironmentMap	EnvironmentMap::loadHDR(const std::string& fileName)
{
	std::ifstream stream(fileName, std::ios::binary);

	if (!stream)
	{
		throw std::runtime_error("HDR environment map could not be opened: " + fileName);
	}

	std::string line;
	if (!std::getline(stream, line))
	{
		throw std::runtime_error("Empty HDR environment map: " + fileName);
	}
	if (line.rfind("#?RADIANCE", 0) != 0 && line.rfind("#?RGBE", 0) != 0)
	{
		throw std::runtime_error("Unsupported HDR environment header: " + fileName);
	}

	bool hasRGBEFormat = false;
	while (std::getline(stream, line))
	{
		if (line.empty() || line == "\r")
		{
			break;
		}
		if (line.rfind("FORMAT=", 0) == 0 && line.find("32-bit_rle_rgbe") != std::string::npos)
		{
			hasRGBEFormat = true;
		}
	}
	if (!hasRGBEFormat)
	{
		throw std::runtime_error("HDR environment map must use FORMAT=32-bit_rle_rgbe: " + fileName);
	}
	if (!std::getline(stream, line))
	{
		throw std::runtime_error("HDR environment map is missing its resolution line: " + fileName);
	}

	std::size_t width = 0;
	std::size_t height = 0;
	bool xPositive = true;
	bool yPositive = false;
	parseHDRResolution(line, width, height, xPositive, yPositive);

	std::vector<Color> pixels(width * height);
	for (std::size_t scanlineIndex = 0; scanlineIndex < height; scanlineIndex++)
	{
		std::vector<Color> scanline;
		if (width >= 8 && width <= 32767)
		{
			unsigned char header[4];
			readExact(stream, header, 4, "Truncated HDR environment scanline header.");
			const std::size_t encodedWidth = (static_cast<std::size_t>(header[2]) << 8) | header[3];
			if (header[0] == 2 && header[1] == 2 && (header[2] & 0x80) == 0 && encodedWidth == width)
			{
				scanline = readRLEHDRScanline(stream, width);
			}
			else
			{
				scanline = readOldHDRScanline(stream, width, header, true);
			}
		}
		else
		{
			scanline = readOldHDRScanline(stream, width, nullptr, false);
		}

		const std::size_t targetY = yPositive ? (height - 1 - scanlineIndex) : scanlineIndex;
		for (std::size_t x = 0; x < width; x++)
		{
			const std::size_t targetX = xPositive ? x : (width - 1 - x);
			pixels[targetY * width + targetX] = scanline[x];
		}
	}

	return (EnvironmentMap(width, height, std::move(pixels)));
}

Color	EnvironmentMap::sampleDirection(const Vector3& direction, double rotationDegrees) const
{
	if (this->empty())
	{
		return (Color(0.0, 0.0, 0.0));
	}

	const UV uv = this->directionToUV(direction, rotationDegrees);

	return (this->sampleUV(uv.u, uv.v));
}

EnvironmentMap::Sample	EnvironmentMap::sample(double selection, Sampler::Sample2D jitter, double rotationDegrees) const
{
	Sample sample;

	if (this->empty())
	{
		return (sample);
	}

	jitter.x = clampSample(jitter.x);
	jitter.y = clampSample(jitter.y);
	if (this->_totalWeight <= 0.0 || this->_cdf.empty())
	{
		const double z = 1.0 - (2.0 * jitter.y);
		const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
		const double phi = 2.0 * D_PI * jitter.x;

		sample.direction = Vector3(radius * std::cos(phi), z, radius * std::sin(phi));
		sample.radiance = this->sampleDirection(sample.direction, rotationDegrees);
		sample.pdf = FULL_SPHERE_PDF;
		sample.valid = true;
		return (sample);
	}

	selection = clampSample(selection);
	const double target = selection * this->_totalWeight;
	auto cdfIt = std::lower_bound(this->_cdf.begin(), this->_cdf.end(), target);
	if (cdfIt == this->_cdf.end())
	{
		cdfIt = this->_cdf.end() - 1;
	}

	const std::size_t index = static_cast<std::size_t>(cdfIt - this->_cdf.begin());
	const std::size_t y = index / this->_width;
	const std::size_t x = index % this->_width;
	const double u = (static_cast<double>(x) + jitter.x) / static_cast<double>(this->_width);
	const double theta0 = D_PI * static_cast<double>(y) / static_cast<double>(this->_height);
	const double theta1 = D_PI * static_cast<double>(y + 1) / static_cast<double>(this->_height);
	const double cosTheta0 = std::cos(theta0);
	const double cosTheta1 = std::cos(theta1);
	const double cosTheta = cosTheta0 + jitter.y * (cosTheta1 - cosTheta0);
	const double radius = std::sqrt(std::max(0.0, 1.0 - cosTheta * cosTheta));
	const double phi = (
		wrap01(u - (rotationDegrees / 360.0)) - 0.5
	) * 2.0 * D_PI;

	// A lat-long row spans unequal latitude angles per unit solid angle. Once a
	// texel is selected, interpolate cos(theta), not theta, so its conditional
	// sample density is constant over the texel's solid angle and matches pdf().
	sample.direction = Vector3(radius * std::cos(phi), cosTheta, radius * std::sin(phi));
	sample.radiance = this->sampleDirection(sample.direction, rotationDegrees);
	sample.pdf = this->pdf(sample.direction, rotationDegrees);
	sample.valid = sample.pdf > 0.0 && std::isfinite(sample.pdf);
	return (sample);
}

double	EnvironmentMap::pdf(const Vector3& direction, double rotationDegrees) const
{
	if (this->empty())
	{
		return (0.0);
	}
	if (this->_totalWeight <= 0.0 || this->_weights.empty() || this->_solidAngles.empty())
	{
		return (FULL_SPHERE_PDF);
	}

	const UV uv = this->directionToUV(direction, rotationDegrees);
	const std::size_t x = std::min(
		static_cast<std::size_t>(std::floor(wrap01(uv.u) * static_cast<double>(this->_width))),
		this->_width - 1
	);
	const std::size_t y = std::min(
		static_cast<std::size_t>(std::floor((1.0 - clamp01(uv.v)) * static_cast<double>(this->_height))),
		this->_height - 1
	);
	const std::size_t index = y * this->_width + x;
	if (this->_weights[index] <= 0.0 || this->_solidAngles[index] <= 0.0)
	{
		return (0.0);
	}
	return ((this->_weights[index] / this->_totalWeight) / this->_solidAngles[index]);
}

double	EnvironmentMap::averageLuminance(void) const
{
	if (this->empty() || this->_totalWeight <= 0.0 || !std::isfinite(this->_totalWeight))
	{
		return (0.0);
	}
	return (this->_totalWeight / (4.0 * D_PI));
}

Color	EnvironmentMap::averageRadiance(void) const
{
	if (this->empty())
	{
		return (Color(0.0, 0.0, 0.0));
	}
	return (this->_averageRadiance);
}

double	EnvironmentMap::horizontalIrradiance(void) const
{
	if (this->empty() || this->_horizontalIrradiance <= 0.0 || !std::isfinite(this->_horizontalIrradiance))
	{
		return (0.0);
	}
	return (this->_horizontalIrradiance);
}

std::size_t	EnvironmentMap::getWidth(void) const
{
	return (this->_width);
}

std::size_t	EnvironmentMap::getHeight(void) const
{
	return (this->_height);
}

bool	EnvironmentMap::empty(void) const
{
	return (this->_width == 0 || this->_height == 0 || this->_pixels.empty());
}

Color	EnvironmentMap::sampleUV(double u, double v) const
{
	u = wrap01(u);
	v = clamp01(v);

	const double x = (u * static_cast<double>(this->_width)) - 0.5;
	const double y = ((1.0 - v) * static_cast<double>(this->_height)) - 0.5;
	const long long xFloor = static_cast<long long>(std::floor(x));
	const long long yFloor = static_cast<long long>(std::floor(y));
	const std::size_t x0 = wrapIndex(xFloor, this->_width);
	const std::size_t x1 = wrapIndex(xFloor + 1, this->_width);
	const std::size_t y0 = static_cast<std::size_t>(std::clamp<long long>(yFloor, 0, static_cast<long long>(this->_height - 1)));
	const std::size_t y1 = static_cast<std::size_t>(std::clamp<long long>(yFloor + 1, 0, static_cast<long long>(this->_height - 1)));
	const double tx = x - std::floor(x);
	const double ty = y - std::floor(y);

	const Color c00 = this->_pixels[y0 * this->_width + x0];
	const Color c10 = this->_pixels[y0 * this->_width + x1];
	const Color c01 = this->_pixels[y1 * this->_width + x0];
	const Color c11 = this->_pixels[y1 * this->_width + x1];
	const Color top = c00 * (1.0 - tx) + c10 * tx;
	const Color bottom = c01 * (1.0 - tx) + c11 * tx;

	return (sanitizeRadiance(top * (1.0 - ty) + bottom * ty));
}

EnvironmentMap::UV	EnvironmentMap::directionToUV(const Vector3& direction, double rotationDegrees) const
{
	const double lengthSquared = Utilities::vectorLengthSquared(direction);
	if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
	{
		return (UV{0.0, 0.5});
	}

	const Vector3 normalized = direction / std::sqrt(lengthSquared);
	const double phi = std::atan2(normalized.getZ(), normalized.getX());
	const double y = std::clamp(normalized.getY(), -1.0, 1.0);

	return (UV{
		wrap01(0.5 + (phi / (2.0 * D_PI)) + (rotationDegrees / 360.0)),
		clamp01(0.5 + (std::asin(y) / D_PI))
	});
}

Vector3	EnvironmentMap::uvToDirection(double u, double v, double rotationDegrees) const
{
	const double phi = (wrap01(u - (rotationDegrees / 360.0)) - 0.5) * 2.0 * D_PI;
	const double y = std::sin((clamp01(v) - 0.5) * D_PI);
	const double radius = std::sqrt(std::max(0.0, 1.0 - y * y));

	return (Vector3(radius * std::cos(phi), y, radius * std::sin(phi)));
}

void	EnvironmentMap::buildDistribution(void)
{
	this->_weights.assign(this->_width * this->_height, 0.0);
	this->_solidAngles.assign(this->_width * this->_height, 0.0);
	this->_cdf.clear();
	this->_cdf.reserve(this->_width * this->_height);
	this->_averageRadiance = Color(0.0, 0.0, 0.0);
	this->_totalWeight = 0.0;
	this->_horizontalIrradiance = 0.0;

	const double deltaPhi = 2.0 * D_PI / static_cast<double>(this->_width);
	for (std::size_t y = 0; y < this->_height; y++)
	{
		const double theta0 = D_PI * static_cast<double>(y) / static_cast<double>(this->_height);
		const double theta1 = D_PI * static_cast<double>(y + 1) / static_cast<double>(this->_height);
		const double solidAngle = deltaPhi * std::max(0.0, std::cos(theta0) - std::cos(theta1));
		const double horizonCosine = horizontalCosine(y, this->_height);

		for (std::size_t x = 0; x < this->_width; x++)
		{
			const std::size_t index = y * this->_width + x;
			const Color radiance = sanitizeRadiance(this->_pixels[index]);
			const double luminance = colorLuminance(radiance);
			const double weight = (std::isfinite(luminance) && luminance > 0.0)
				? luminance * solidAngle
				: 0.0;

			this->_solidAngles[index] = solidAngle;
			this->_weights[index] = weight;
			this->_averageRadiance += radiance * (solidAngle / (4.0 * D_PI));
			this->_totalWeight += weight;
			this->_horizontalIrradiance += luminance * horizonCosine * solidAngle;
			this->_cdf.push_back(this->_totalWeight);
		}
	}

	if (!std::isfinite(this->_totalWeight) || this->_totalWeight <= 0.0)
	{
		this->_totalWeight = 0.0;
		this->_horizontalIrradiance = 0.0;
		this->_cdf.clear();
		this->_weights.assign(this->_width * this->_height, 0.0);
	}
	if (!std::isfinite(this->_horizontalIrradiance) || this->_horizontalIrradiance <= 0.0)
	{
		this->_horizontalIrradiance = 0.0;
	}
	this->_averageRadiance = sanitizeRadiance(this->_averageRadiance);
}
