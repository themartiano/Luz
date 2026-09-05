#include "Hittables/CloudVolume.hpp"
#include "Materials/HenyeyGreenstein.hpp"
#include "Sampler.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
	constexpr double CLOUD_EPSILON = 1e-12;
	constexpr std::uint32_t CLOUD_TRACKING_DIMENSION = 0x20000u + Sampler::DIM_VOLUME_DISTANCE;
	constexpr std::uint32_t CLOUD_ACCEPTANCE_OFFSET =
		Sampler::DIM_VOLUME_ACCEPTANCE - Sampler::DIM_VOLUME_DISTANCE;
	constexpr double CLOUD_FEATURE_OPACITY_THRESHOLD = 0.04;

	double clamp01(double value)
	{
		return (std::max(0.0, std::min(1.0, value)));
	}

	double smoothstep(double edge0, double edge1, double value)
	{
		if (edge1 <= edge0)
		{
			return (value >= edge1 ? 1.0 : 0.0);
		}
		const double t = clamp01((value - edge0) / (edge1 - edge0));
		return (t * t * (3.0 - 2.0 * t));
	}

	std::uint32_t mixBits(std::uint32_t value)
	{
		value ^= value >> 16;
		value *= 0x7feb352du;
		value ^= value >> 15;
		value *= 0x846ca68bu;
		value ^= value >> 16;
		return (value);
	}

	std::uint32_t nextCloudSamplingStream(std::uint32_t seed)
	{
		static std::atomic<std::uint64_t> nextInstance{0};
		const std::uint64_t instance = nextInstance.fetch_add(1, std::memory_order_relaxed);
		const std::uint32_t foldedInstance = static_cast<std::uint32_t>(instance)
			^ static_cast<std::uint32_t>(instance >> 32u);
		return (mixBits(seed ^ mixBits(foldedInstance + 0x9e3779b9u)));
	}

	std::uint32_t latticeHash(int x, int y, int z, std::uint32_t seed, std::uint32_t salt)
	{
		std::uint32_t hash = seed ^ salt;
		hash ^= mixBits(static_cast<std::uint32_t>(x) + 0x9e3779b9u);
		hash ^= mixBits(static_cast<std::uint32_t>(y) + 0x85ebca6bu);
		hash ^= mixBits(static_cast<std::uint32_t>(z) + 0xc2b2ae35u);
		return (mixBits(hash));
	}

	double hashUnit(std::uint32_t value)
	{
		return (static_cast<double>(value >> 8) * (1.0 / 16777216.0));
	}

	double fade(double value)
	{
		return (value * value * value * (value * (value * 6.0 - 15.0) + 10.0));
	}

	double lerp(double a, double b, double t)
	{
		return (a + (b - a) * t);
	}

	int clampedCeilToInt(double value, int minimum, int maximum)
	{
		if (!std::isfinite(value))
			return (maximum);
		return (static_cast<int>(std::clamp(
			std::ceil(value),
			static_cast<double>(minimum),
			static_cast<double>(maximum)
		)));
	}

	void requireFinitePositive(double value, const char* name)
	{
		if (!std::isfinite(value) || value <= 0.0)
		{
			throw std::invalid_argument(std::string("Cloud ") + name + " must be finite and positive.");
		}
	}

	void requireUnitInterval(double value, const char* name)
	{
		if (!std::isfinite(value) || value < 0.0 || value > 1.0)
		{
			throw std::invalid_argument(std::string("Cloud ") + name + " must be between zero and one.");
		}
	}

	void requireFiniteColor(const Color& color)
	{
		requireUnitInterval(color.getRed(), "albedo red");
		requireUnitInterval(color.getGreen(), "albedo green");
		requireUnitInterval(color.getBlue(), "albedo blue");
	}

	bool finiteVector(const Vector3& vector)
	{
		return (
			std::isfinite(vector.getX())
			&& std::isfinite(vector.getY())
			&& std::isfinite(vector.getZ())
		);
	}
}

CloudParameters CloudVolume::preset(CloudType type)
{
	CloudParameters parameters;
	parameters.type = type;

	switch (type)
	{
		case CloudType::Stratocumulus:
			parameters.size = Vector3(8000.0, 1200.0, 8000.0);
			parameters.coverage = 0.68;
			parameters.extinction = 0.009;
			parameters.anisotropy = 0.80;
			parameters.forwardWeight = 0.90;
			parameters.featureScale = 1300.0;
			parameters.detail = 0.58;
			parameters.erosion = 0.38;
			parameters.puffiness = 0.68;
			parameters.towering = 0.28;
			parameters.dominance = 0.24;
			parameters.overhang = 0.16;
			parameters.fineDetail = 0.58;
			break;
		case CloudType::Stratus:
			parameters.size = Vector3(12000.0, 650.0, 12000.0);
			parameters.coverage = 0.88;
			parameters.extinction = 0.0045;
			parameters.anisotropy = 0.76;
			parameters.forwardWeight = 0.88;
			parameters.featureScale = 2600.0;
			parameters.detail = 0.28;
			parameters.erosion = 0.18;
			parameters.puffiness = 0.30;
			parameters.towering = 0.08;
			parameters.dominance = 0.05;
			parameters.overhang = 0.05;
			parameters.fineDetail = 0.28;
			parameters.detailOctaves = 2;
			break;
		case CloudType::Cirrus:
			parameters.size = Vector3(14000.0, 500.0, 14000.0);
			parameters.coverage = 0.34;
			parameters.extinction = 0.0014;
			parameters.albedo = Color(0.995, 0.997, 1.0);
			parameters.anisotropy = 0.87;
			parameters.backscatter = -0.15;
			parameters.forwardWeight = 0.96;
			parameters.featureScale = 1800.0;
			parameters.detail = 0.78;
			parameters.erosion = 0.62;
			parameters.puffiness = 0.18;
			parameters.towering = 0.05;
			parameters.dominance = 0.08;
			parameters.overhang = 0.82;
			parameters.fineDetail = 0.86;
			parameters.detailOctaves = 4;
			break;
		case CloudType::Cumulonimbus:
			parameters.size = Vector3(7000.0, 9000.0, 7000.0);
			parameters.coverage = 0.38;
			parameters.extinction = 0.016;
			parameters.anisotropy = 0.84;
			parameters.backscatter = -0.32;
			parameters.forwardWeight = 0.91;
			parameters.featureScale = 1500.0;
			parameters.detail = 0.72;
			parameters.erosion = 0.48;
			parameters.puffiness = 0.90;
			parameters.towering = 1.0;
			parameters.dominance = 0.90;
			parameters.overhang = 0.92;
			parameters.fineDetail = 0.82;
			parameters.detailOctaves = 4;
			break;
		case CloudType::Cumulus:
		default:
			break;
	}
	return (parameters);
}

CloudType CloudVolume::parseType(const std::string& name)
{
	std::string lower = name;
	std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char character) {
		return (static_cast<char>(std::tolower(character)));
	});
	if (lower == "cumulus")
		return (CloudType::Cumulus);
	if (lower == "stratocumulus" || lower == "strato_cumulus" || lower == "strato-cumulus")
		return (CloudType::Stratocumulus);
	if (lower == "stratus")
		return (CloudType::Stratus);
	if (lower == "cirrus")
		return (CloudType::Cirrus);
	if (lower == "cumulonimbus" || lower == "storm")
		return (CloudType::Cumulonimbus);
	throw std::invalid_argument("Unknown cloud type: " + name);
}

const char* CloudVolume::typeName(CloudType type)
{
	switch (type)
	{
		case CloudType::Stratocumulus: return ("stratocumulus");
		case CloudType::Stratus: return ("stratus");
		case CloudType::Cirrus: return ("cirrus");
		case CloudType::Cumulonimbus: return ("cumulonimbus");
		case CloudType::Cumulus:
		default: return ("cumulus");
	}
}

CloudVolume::CloudVolume(const CloudParameters& parameters)
	: _parameters(parameters), _samplingStream(nextCloudSamplingStream(parameters.seed))
{
	if (
		!finiteVector(parameters.position)
		|| !finiteVector(parameters.size)
		|| !finiteVector(parameters.shearDirection)
		|| !finiteVector(parameters.offset)
	)
		throw std::invalid_argument("Cloud vectors must be finite.");
	requireFinitePositive(parameters.size.getX(), "width");
	requireFinitePositive(parameters.size.getY(), "height");
	requireFinitePositive(parameters.size.getZ(), "depth");
	requireFinitePositive(parameters.extinction, "extinction");
	requireFinitePositive(parameters.featureScale, "feature scale");
	if (!std::isfinite(parameters.macroScale) || parameters.macroScale < 0.25 || parameters.macroScale > 3.0)
		throw std::invalid_argument("Cloud macro scale must be between 0.25 and 3.0.");
	requireFinitePositive(parameters.metersPerUnit, "meters per unit");
	requireUnitInterval(parameters.coverage, "coverage");
	requireUnitInterval(parameters.detail, "detail");
	requireUnitInterval(parameters.erosion, "erosion");
	requireUnitInterval(parameters.puffiness, "puffiness");
	requireUnitInterval(parameters.towering, "towering");
	requireUnitInterval(parameters.dominance, "dominance");
	requireUnitInterval(parameters.overhang, "overhang");
	requireUnitInterval(parameters.fineDetail, "fine detail");
	requireUnitInterval(parameters.weatherVariation, "weather variation");
	requireUnitInterval(parameters.baseVariation, "base variation");
	if (!std::isfinite(parameters.primaryDetail) || parameters.primaryDetail < 0.25 || parameters.primaryDetail > 16.0)
		throw std::invalid_argument("Cloud primary detail must be between 0.25 and 16.");
	if (!std::isfinite(parameters.directionalCacheResolution) || parameters.directionalCacheResolution < 0.0 || parameters.directionalCacheResolution > 16.0)
		throw std::invalid_argument("Cloud directional cache resolution must be between zero and 16.");
	if (
		!std::isfinite(parameters.multipleScatteringFalloff)
		|| parameters.multipleScatteringFalloff <= 0.0
		|| parameters.multipleScatteringFalloff > 1.0
	)
		throw std::invalid_argument("Cloud multiple scattering falloff must be greater than zero and at most one.");
	if (
		!std::isfinite(parameters.multipleScatteringCompensation)
		|| parameters.multipleScatteringCompensation < 0.0
		|| parameters.multipleScatteringCompensation > 2.0
	)
		throw std::invalid_argument("Cloud multiple scattering compensation must be between zero and two.");
	requireFiniteColor(parameters.albedo);
	if (!std::isfinite(parameters.anisotropy) || parameters.anisotropy < -0.99 || parameters.anisotropy > 0.99)
		throw std::invalid_argument("Cloud anisotropy must be between -0.99 and 0.99.");
	if (!std::isfinite(parameters.backscatter) || parameters.backscatter < -0.99 || parameters.backscatter > 0.99)
		throw std::invalid_argument("Cloud backscatter must be between -0.99 and 0.99.");
	requireUnitInterval(parameters.forwardWeight, "forward weight");
	if (
		!std::isfinite(parameters.dropletSizeMicrons)
		|| (parameters.dropletSizeMicrons != 0.0
			&& (parameters.dropletSizeMicrons < 5.0 || parameters.dropletSizeMicrons > 50.0))
	)
		throw std::invalid_argument("Cloud droplet size must be zero or between 5 and 50 microns.");
	if (parameters.detailOctaves < 1 || parameters.detailOctaves > 8)
		throw std::invalid_argument("Cloud detail octaves must be between 1 and 8.");
	if (parameters.maxTrackingSteps < 1 || parameters.maxTrackingSteps > 4096)
		throw std::invalid_argument("Cloud tracking steps must be between 1 and 4096.");

	const Vector3 halfSize = parameters.size * 0.5;
	this->_minimum = parameters.position - halfSize;
	this->_maximum = parameters.position + halfSize;
	this->_majorantSceneUnits = parameters.extinction * parameters.metersPerUnit;
	if (
		!finiteVector(this->_minimum)
		|| !finiteVector(this->_maximum)
		|| !std::isfinite(this->_majorantSceneUnits)
		|| this->_majorantSceneUnits <= 0.0
	)
		throw std::invalid_argument("Cloud derived bounds and extinction must be finite.");
	const double diagonal = std::hypot(
		parameters.size.getX(),
		parameters.size.getY(),
		parameters.size.getZ()
	);
	if (!std::isfinite(diagonal) || !std::isfinite(diagonal * this->_majorantSceneUnits))
		throw std::invalid_argument("Cloud optical extent is too large.");
	// gradientNoise converts lattice coordinates to int at every fBm octave.
	// Validate the largest detail-frequency/octave amplification rather than
	// only the base coordinate, so extreme but finite offsets cannot overflow.
	const double maximumNoiseFrequency = 6.8 * std::pow(
		2.03,
		static_cast<double>(parameters.detailOctaves - 1)
	);
	const double maximumBaseNoiseCoordinate = (
		static_cast<double>(std::numeric_limits<int>::max()) - 256.0
	) / maximumNoiseFrequency;
	for (int axis = 0; axis < 3; axis++)
	{
		const double axisScale = parameters.featureScale * (axis == 1 ? 0.78 : 1.0);
		const double minimumNoiseCoordinate = (this->_minimum[axis] + parameters.offset[axis]) / axisScale;
		const double maximumNoiseCoordinate = (this->_maximum[axis] + parameters.offset[axis]) / axisScale;
		if (
			!std::isfinite(minimumNoiseCoordinate)
			|| !std::isfinite(maximumNoiseCoordinate)
			|| std::fabs(minimumNoiseCoordinate) > maximumBaseNoiseCoordinate
			|| std::fabs(maximumNoiseCoordinate) > maximumBaseNoiseCoordinate
		)
			throw std::invalid_argument("Cloud noise coordinates are outside the supported range.");
	}
	this->_phaseFunction = std::make_shared<HenyeyGreenstein>(
		parameters.albedo,
		parameters.anisotropy,
		parameters.backscatter,
		parameters.forwardWeight
	);
	if (parameters.dropletSizeMicrons > 0.0)
	{
		std::dynamic_pointer_cast<HenyeyGreenstein>(this->_phaseFunction)->setDropletPhase(
			parameters.dropletSizeMicrons
		);
	}
	std::dynamic_pointer_cast<HenyeyGreenstein>(this->_phaseFunction)->setDepthAnisotropyReduction(
		parameters.multipleScatteringFalloff < 1.0
	);
	this->buildConvectiveLobes();
	if (this->_lobeGrid.empty())
		this->buildLobeGrid();
	this->buildDensityMajorants();
}

Material* CloudVolume::getMaterial(void) const
{
	return (this->_phaseFunction.get());
}

bool CloudVolume::boundsInterval(const Ray& ray, double t_min, double t_max, double& entryT, double& exitT) const
{
	entryT = t_min;
	exitT = t_max;
	const Vector3& origin = ray.getOrigin();
	const Vector3& direction = ray.getDirection();

	for (int axis = 0; axis < 3; axis++)
	{
		if (std::fabs(direction[axis]) <= CLOUD_EPSILON)
		{
			if (origin[axis] < this->_minimum[axis] || origin[axis] > this->_maximum[axis])
				return (false);
			continue;
		}
		double axisEntry = (this->_minimum[axis] - origin[axis]) / direction[axis];
		double axisExit = (this->_maximum[axis] - origin[axis]) / direction[axis];
		if (axisEntry > axisExit)
			std::swap(axisEntry, axisExit);
		entryT = std::max(entryT, axisEntry);
		exitT = std::min(exitT, axisExit);
		if (entryT >= exitT)
			return (false);
	}
	return (std::isfinite(entryT) && std::isfinite(exitT));
}

double CloudVolume::gradientNoise(const Vector3& position, std::uint32_t salt) const
{
	const int ix = static_cast<int>(std::floor(position.getX()));
	const int iy = static_cast<int>(std::floor(position.getY()));
	const int iz = static_cast<int>(std::floor(position.getZ()));
	const double fx = position.getX() - std::floor(position.getX());
	const double fy = position.getY() - std::floor(position.getY());
	const double fz = position.getZ() - std::floor(position.getZ());
	const double u = fade(fx);
	const double v = fade(fy);
	const double w = fade(fz);
	double values[2][2][2];

	for (int z = 0; z < 2; z++)
		for (int y = 0; y < 2; y++)
			for (int x = 0; x < 2; x++)
				values[z][y][x] = hashUnit(latticeHash(ix + x, iy + y, iz + z, this->_parameters.seed, salt));
	const double x00 = lerp(values[0][0][0], values[0][0][1], u);
	const double x10 = lerp(values[0][1][0], values[0][1][1], u);
	const double x01 = lerp(values[1][0][0], values[1][0][1], u);
	const double x11 = lerp(values[1][1][0], values[1][1][1], u);
	return (lerp(lerp(x00, x10, v), lerp(x01, x11, v), w));
}

double CloudVolume::fbm(const Vector3& position, int octaves, std::uint32_t salt) const
{
	double amplitude = 0.5;
	double frequency = 1.0;
	double sum = 0.0;
	double normalization = 0.0;

	for (int octave = 0; octave < octaves; octave++)
	{
		const Vector3 samplePosition(
			position.getX() * frequency + octave * 17.17,
			position.getY() * frequency - octave * 11.31,
			position.getZ() * frequency + octave * 7.73
		);
		sum += this->gradientNoise(samplePosition, salt + static_cast<std::uint32_t>(octave) * 0x9e3779b9u) * amplitude;
		normalization += amplitude;
		frequency *= 2.03;
		amplitude *= 0.5;
	}
	return (normalization > 0.0 ? sum / normalization : 0.0);
}

double CloudVolume::verticalProfile(double height, double growthNoise, double coverageMask) const
{
	const double h = clamp01(height);
	switch (this->_parameters.type)
	{
		case CloudType::Stratocumulus:
		{
			const double top = 0.48 + 0.28 * this->_parameters.towering * (0.55 * coverageMask + 0.45 * growthNoise);
			return (smoothstep(0.0, 0.045, h) * (1.0 - smoothstep(top - 0.16, top, h)));
		}
		case CloudType::Stratus:
		{
			// Let the same mesoscale field that selects the layer also displace its
			// boundaries. A fixed envelope makes a shallow deck reveal a ruler-flat
			// top at grazing angles even though its density varies horizontally.
			const double layerGrowth = clamp01(0.78 * growthNoise + 0.22 * coverageMask);
			const double base = 0.015 + 0.055 * (1.0 - layerGrowth);
			const double top = 0.76 + 0.21 * layerGrowth;
			return (
				smoothstep(base, base + 0.04, h)
				* (1.0 - smoothstep(top - 0.18, top, h))
			);
		}
		case CloudType::Cirrus:
			return (smoothstep(0.04, 0.20, h) * (1.0 - smoothstep(0.68, 0.96, h)));
		case CloudType::Cumulonimbus:
		{
			const double top = 0.58 + 0.40 * this->_parameters.towering * std::pow(coverageMask, 0.35);
			const double tower = smoothstep(0.0, 0.025, h) * (1.0 - smoothstep(top - 0.14, top, h));
			const double anvil = smoothstep(0.68, 0.83, h) * (1.0 - smoothstep(0.94, 1.0, h));
			return (clamp01(tower + 0.48 * anvil * smoothstep(0.55, 0.9, coverageMask)));
		}
		case CloudType::Cumulus:
		default:
		{
			const double top = 0.22 + 0.73 * this->_parameters.towering
				* std::pow(coverageMask, 0.55) * (0.38 + 0.62 * growthNoise);
			return (smoothstep(0.0, 0.025, h) * (1.0 - smoothstep(top - 0.13, top, h)));
		}
	}
}

double CloudVolume::cellularPuffs(const Vector3& position, double scale, std::uint32_t salt) const
{
	const Vector3 gridPosition = position / scale;
	const int cellX = static_cast<int>(std::floor(gridPosition.getX()));
	const int cellY = static_cast<int>(std::floor(gridPosition.getY()));
	const int cellZ = static_cast<int>(std::floor(gridPosition.getZ()));
	double field = 0.0;
	double verticalStretch = 0.88;
	if (this->_parameters.type == CloudType::Stratocumulus)
		verticalStretch = 0.62;
	else if (this->_parameters.type == CloudType::Stratus)
		verticalStretch = 0.35;
	else if (this->_parameters.type == CloudType::Cirrus)
		verticalStretch = 0.24;
	else if (this->_parameters.type == CloudType::Cumulonimbus)
		verticalStretch = 1.18;

	for (int z = -1; z <= 1; z++)
	{
		for (int y = -1; y <= 1; y++)
		{
			for (int x = -1; x <= 1; x++)
			{
				const int px = cellX + x;
				const int py = cellY + y;
				const int pz = cellZ + z;
				const std::uint32_t hash = latticeHash(px, py, pz, this->_parameters.seed, salt);
				const double jitterX = 0.18 + 0.64 * hashUnit(mixBits(hash ^ 0xa511e9b3u));
				const double jitterY = 0.18 + 0.64 * hashUnit(mixBits(hash ^ 0x63d83595u));
				const double jitterZ = 0.18 + 0.64 * hashUnit(mixBits(hash ^ 0x9e3779b9u));
				const double radius = 0.62 + 0.30 * hashUnit(mixBits(hash ^ 0xc2b2ae35u));
				const double dx = (gridPosition.getX() - (static_cast<double>(px) + jitterX)) / radius;
				const double dy = (gridPosition.getY() - (static_cast<double>(py) + jitterY)) / (radius * verticalStretch);
				const double dz = (gridPosition.getZ() - (static_cast<double>(pz) + jitterZ)) / radius;
				const double distance = std::sqrt(dx * dx + dy * dy + dz * dz);
				field = std::max(field, 1.0 - distance);
			}
		}
	}
	return (smoothstep(0.0, 0.48, field));
}

void CloudVolume::buildConvectiveLobes(void)
{
	this->_lobes.clear();
	this->_lobeGrid.clear();
	this->_lobeGridX = 0;
	this->_lobeGridY = 0;
	this->_lobeGridZ = 0;
	if (
		this->_parameters.type == CloudType::Stratus
		|| this->_parameters.type == CloudType::Cirrus
		|| this->_parameters.coverage <= 0.0
	)
		return;

	constexpr double TWO_PI = 6.28318530717958647692;
	const double feature = this->_parameters.featureScale;
	// Smaller, overlapping macro cells keep the analytic primitives below the
	// visible feature scale. Shallow decks need the densest tiling because their
	// vertical extent cannot hide a sparse set of broad ellipsoids.
	const double spacing = feature * (
		this->_parameters.type == CloudType::Stratocumulus ? 1.15 : 1.65
	);
	// Broad stratocumulus decks need enough independent thermals to overlap into
	// a field. The convective 8x8 cap turns large decks into a sparse set of
	// isolated, feature-scale ellipsoids when their natural grid is much denser.
	const int maximumGridAxis = this->_parameters.type == CloudType::Stratocumulus
		? 32
		: 12;
	const int gridX = clampedCeilToInt(this->_parameters.size.getX() / spacing, 2, maximumGridAxis);
	const int gridZ = clampedCeilToInt(this->_parameters.size.getZ() / spacing, 2, maximumGridAxis);
	const double cellX = this->_parameters.size.getX() * 0.76 / static_cast<double>(gridX);
	const double cellZ = this->_parameters.size.getZ() * 0.76 / static_cast<double>(gridZ);
	const double startX = this->_parameters.position.getX() - 0.38 * this->_parameters.size.getX() + 0.5 * cellX;
	const double startZ = this->_parameters.position.getZ() - 0.38 * this->_parameters.size.getZ() + 0.5 * cellZ;

	auto randomValue = [this](std::uint32_t index, std::uint32_t salt) {
		return (hashUnit(mixBits(this->_parameters.seed ^ mixBits(index + salt))));
	};
	auto appendLobe = [this](Vector3 center, Vector3 radius, double strength) {
		if (this->_parameters.weatherVariation > 0.0)
		{
			const Vector3 p = (center - this->_parameters.position) / (this->_parameters.featureScale * 3.0);
			const double weather = this->fbm(Vector3(p.getX(), 0.73, p.getZ()), 2, 0x71931abdu);
			const double variation = this->_parameters.weatherVariation * (2.0 * weather - 1.0);
			radius = radius * (1.0 + 0.65 * variation);
			center.setY(this->_minimum.getY()
				+ (center.getY() - this->_minimum.getY()) * (1.0 + 0.30 * variation));
		}
		radius.setX(std::min(radius.getX(), this->_parameters.size.getX() * 0.499));
		radius.setY(std::min(radius.getY(), this->_parameters.size.getY() * 0.499));
		radius.setZ(std::min(radius.getZ(), this->_parameters.size.getZ() * 0.499));
		center.setX(std::clamp(
			center.getX(),
			this->_minimum.getX() + radius.getX(),
			this->_maximum.getX() - radius.getX()
		));
		center.setY(std::clamp(
			center.getY(),
			this->_minimum.getY(),
			this->_maximum.getY() - radius.getY()
		));
		center.setZ(std::clamp(
			center.getZ(),
			this->_minimum.getZ() + radius.getZ(),
			this->_maximum.getZ() - radius.getZ()
		));
		this->_lobes.push_back({center, radius, strength});
	};
	auto appendCauliflowerShell = [this, &appendLobe, &randomValue](
		const Vector3& center,
		const Vector3& parentRadius,
		std::uint32_t parentIndex,
		double amount,
		double topBias
	) {
		const double detail = this->_parameters.fineDetail * clamp01(amount);
		if (detail <= 0.0)
			return;
		const int childCount = std::clamp(
			static_cast<int>(std::round(3.0 + detail * 9.0)),
			3,
			12
		);
		const double rotation = 6.28318530717958647692
			* randomValue(parentIndex, 0x5f91du);
		const double minimumParentRadius = std::min({
			parentRadius.getX(),
			parentRadius.getY(),
			parentRadius.getZ()
		});
		for (int child = 0; child < childCount; child++)
		{
			const std::uint32_t childIndex = parentIndex * 17u
				+ static_cast<std::uint32_t>(child) + 0x2a31u;
			const double sequence = (
				static_cast<double>(child) + 0.35
				+ 0.30 * randomValue(childIndex, 0x91b7du)
			) / static_cast<double>(childCount);
			// A Fibonacci shell avoids horizontal rings. Suppress the underside and
			// favor newly condensing upper cells while still populating both camera-
			// facing and rear surfaces for arbitrary views.
			const double directionY = std::clamp(
				-0.42 + 1.34 * sequence + 0.18 * topBias,
				-0.55,
				0.96
			);
			const double radial = std::sqrt(std::max(0.0, 1.0 - directionY * directionY));
			const double azimuth = rotation
				+ 2.39996322972865332 * static_cast<double>(child)
				+ 0.32 * (randomValue(childIndex, 0x38e51u) - 0.5);
			const Vector3 outward(
				std::cos(azimuth) * radial,
				directionY,
				std::sin(azimuth) * radial
			);
			// Keep secondary cells embedded in their parent. Placing their centers on
			// the exact ellipsoid boundary made the construction read as a necklace of
			// pasted-on spheres instead of one turbulent condensation surface.
			const double childRadius = minimumParentRadius * (
				0.22 + detail * (0.10 + 0.10 * randomValue(childIndex, 0xa73c1u))
			);
			const double shellSurface = 1.06
				+ 0.06 * randomValue(childIndex, 0x61df3u);
			auto shellOffset = [childRadius, shellSurface](double parentExtent) {
				// Target a small, consistent protrusion on every axis. A single scale
				// embeds children too deeply along the broad axis of an anvil, but puts
				// them completely outside a near-spherical parent.
				return (std::max(
					parentExtent * 0.62,
					parentExtent * shellSurface - childRadius
				));
			};
			const Vector3 childCenter = center + Vector3(
				outward.getX() * shellOffset(parentRadius.getX()),
				outward.getY() * shellOffset(parentRadius.getY()),
				outward.getZ() * shellOffset(parentRadius.getZ())
			);
			const Vector3 childRadii(
				childRadius * (0.88 + 0.18 * randomValue(childIndex, 0x1d4abu)),
				childRadius * (0.86 + 0.18 * randomValue(childIndex, 0xc81e7u)),
				childRadius * (0.88 + 0.18 * randomValue(childIndex, 0x4a29fu))
			);
			appendLobe(childCenter, childRadii, 1.0);

			if (detail < 0.40)
				continue;
			const int microCount = detail >= 0.68 ? 2 : 1;
			for (int micro = 0; micro < microCount; micro++)
			{
				const std::uint32_t microIndex = childIndex * 29u
					+ static_cast<std::uint32_t>(micro) + 0x713u;
				const double microAzimuth = azimuth
					+ (static_cast<double>(micro) - 0.5) * 1.85
					+ 0.55 * (randomValue(microIndex, 0x17cb3u) - 0.5);
				Vector3 tangent(
					std::cos(microAzimuth),
					0.28 * (randomValue(microIndex, 0x8a3d1u) - 0.25),
					std::sin(microAzimuth)
				);
				if (Utilities::vectorLengthSquared(tangent) > CLOUD_EPSILON)
					tangent = Utilities::normalize(tangent);
				Vector3 microDirection = outward * 0.78 + tangent * 0.48;
				if (Utilities::vectorLengthSquared(microDirection) <= CLOUD_EPSILON)
					microDirection = outward;
				else
					microDirection = Utilities::normalize(microDirection);
				const double microRadius = childRadius * (
					0.38 + 0.19 * randomValue(microIndex, 0x71a93u)
				);
				const Vector3 microCenter = childCenter
					+ microDirection * (childRadius * 0.98);
				appendLobe(
					microCenter,
					Vector3(microRadius, microRadius * 0.82, microRadius),
					0.98
				);
				if (
					detail >= 0.72
					&& randomValue(microIndex, 0x61de3u) < 0.30 + 0.34 * detail
				)
				{
					const std::uint32_t nanoIndex = microIndex * 7u + 0x19u;
					const double nanoRadius = microRadius * (
						0.34 + 0.14 * randomValue(nanoIndex, 0x2c17bu)
					);
					Vector3 nanoDirection = microDirection + tangent * (
						0.42 * (randomValue(nanoIndex, 0x8f21du) - 0.5)
					);
					if (Utilities::vectorLengthSquared(nanoDirection) > CLOUD_EPSILON)
						nanoDirection = Utilities::normalize(nanoDirection);
					appendLobe(
						microCenter + nanoDirection * (microRadius * 0.92),
						Vector3(nanoRadius, nanoRadius * 0.84, nanoRadius),
						0.92
					);
				}
			}
		}
	};
	struct Thermal
	{
		double x;
		double z;
		double radius;
		double strength;
	};
	std::vector<Thermal> thermals;
	const int thermalCount = std::clamp(
		1 + static_cast<int>(std::round(this->_parameters.coverage * 5.0)),
		2,
		6
	);
	thermals.reserve(static_cast<std::size_t>(thermalCount));
	for (int thermal = 0; thermal < thermalCount; thermal++)
	{
		const std::uint32_t index = static_cast<std::uint32_t>(thermal);
		if (thermal == 0)
		{
			thermals.push_back({
				(0.46 + 0.10 * (randomValue(index, 0x41d2bu) - 0.5)) * static_cast<double>(gridX - 1),
				(0.48 + 0.14 * (randomValue(index, 0x98af3u) - 0.5)) * static_cast<double>(gridZ - 1),
				0.90 + this->_parameters.coverage * 0.75 + this->_parameters.dominance * 0.42,
				1.0 + this->_parameters.dominance * 0.05
			});
		}
		else
		{
			thermals.push_back({
				randomValue(index, 0x41d2bu) * static_cast<double>(gridX - 1),
				randomValue(index, 0x98af3u) * static_cast<double>(gridZ - 1),
				1.0 + this->_parameters.coverage * 1.8 + randomValue(index, 0x21ce7u) * 0.9,
				(0.70 + randomValue(index, 0xd1749u) * 0.30) * (1.0 - 0.38 * this->_parameters.dominance)
			});
		}
	}
	const double formationThreshold = this->_parameters.type == CloudType::Stratocumulus
		? std::clamp(1.0 - this->_parameters.coverage, 0.16, 0.68)
		: 0.54 - 0.34 * this->_parameters.coverage;
	const double shearAngle = TWO_PI * randomValue(0u, 0x6f2a1u);
	Vector3 shearDirection(
		this->_parameters.shearDirection.getX(),
		0.0,
		this->_parameters.shearDirection.getZ()
	);
	if (Utilities::vectorLengthSquared(shearDirection) <= CLOUD_EPSILON)
		shearDirection = Vector3(std::cos(shearAngle), 0.0, std::sin(shearAngle));
	else
		shearDirection = Utilities::normalize(shearDirection);
	const double shearDistance = feature * this->_parameters.towering
		* (0.18 + this->_parameters.overhang * (1.1 + 1.8 * randomValue(0u, 0xac731u)));
	// A dense stratocumulus tiling gets its breakup from the displaced metaball
	// field. Shelling every shallow cell is both redundant and disproportionately
	// expensive; sparse convective towers retain the multiscale shell.
	const bool useCauliflowerShell = this->_parameters.fineDetail > 0.45
		&& this->_parameters.type != CloudType::Stratocumulus;
	const bool useLegacyChildren = this->_parameters.fineDetail <= 0.45;

	// A connected primary plume gives the formation a readable macro silhouette.
	// The weather columns below remain as shorter supporting cells and a flat base.
	const double heroBaseX = startX + thermals[0].x * cellX
		+ (randomValue(0u, 0x93a17u) - 0.5) * cellX * 0.20;
	const double heroBaseZ = startZ + thermals[0].z * cellZ
		+ (randomValue(0u, 0x7c5d1u) - 0.5) * cellZ * 0.20;
	double heroHeightFraction = 0.25 + 0.68 * this->_parameters.towering
		* (0.35 + 0.65 * this->_parameters.dominance);
	if (this->_parameters.type == CloudType::Stratocumulus)
		heroHeightFraction = 0.18 + 0.25 * this->_parameters.towering;
	else if (this->_parameters.type == CloudType::Cumulonimbus)
		heroHeightFraction += 0.07;
	const double heroHeight = this->_parameters.size.getY() * std::min(0.96, heroHeightFraction);
	const int heroLevels = clampedCeilToInt(heroHeight / (feature * 0.64), 3, 16);
	const double macroScale = this->_parameters.macroScale;
	const double shoulderAngle = TWO_PI * randomValue(0u, 0x7d31bu);
	const Vector3 shoulderDirection(std::cos(shoulderAngle), 0.0, std::sin(shoulderAngle));
	Vector3 heroWander(0.0, 0.0, 0.0);
	for (int level = 0; level < heroLevels; level++)
	{
		const double t = heroLevels > 1
			? static_cast<double>(level) / static_cast<double>(heroLevels - 1)
			: 0.0;
		const std::uint32_t index = 0x1000u + static_cast<std::uint32_t>(level);
		if (level > 0)
		{
			heroWander += Vector3(
				(randomValue(index, 0x18e4du) - 0.5) * feature * 0.18,
				0.0,
				(randomValue(index, 0xa24b3u) - 0.5) * feature * 0.18
			);
		}
		const double middle = smoothstep(0.24, 0.52, t) * (1.0 - smoothstep(0.66, 0.84, t));
		const double neck = 1.0 - 0.12 * this->_parameters.dominance * middle;
		const double trunkBulge = 1.0 + 0.18 * this->_parameters.dominance
			* smoothstep(0.08, 0.32, t) * (1.0 - smoothstep(0.62, 0.80, t));
		// Localize the broad crown below the final cell. A monotonic envelope made
		// every upper level an oblate disk; tapering it back to zero leaves a broad
		// anvil/congestus shelf with a rounded overshooting top.
		const double crown = this->_parameters.overhang
			* smoothstep(0.76, 0.90, t)
			* (1.0 - smoothstep(0.94, 1.0, t));
		const double radiusBase = feature
			* (1.02 + 0.24 * randomValue(index, 0xd872fu))
			* (0.82 + 0.34 * this->_parameters.dominance)
			* neck
			* trunkBulge;
		const double crownMacroScale = 1.0 + 0.75 * (macroScale - 1.0);
		const double levelMacroScale = lerp(
			macroScale,
			crownMacroScale,
			smoothstep(0.62, 0.96, t)
		);
		// macro_scale controls the coherent formation, not the radius of every
		// implicit primitive. Let the overlapping shoulders carry most of that
		// width; otherwise high macro scales expose a stack of giant ellipsoids.
		const double horizontalMacroScale = 1.0 + 0.34 * (levelMacroScale - 1.0);
		const double verticalMacroScale = 1.0 + 0.48 * (levelMacroScale - 1.0);
		// A true anvil may spread strongly; ordinary cumulus overhang should remain
		// a rounded cauliflower cap rather than acquiring the same disk aspect.
		const double crownExpansion = this->_parameters.type == CloudType::Cumulonimbus
			? 0.42
			: 0.20;
		double horizontalRadius = radiusBase * horizontalMacroScale
			* (1.0 + crownExpansion * crown);
		if (this->_parameters.type == CloudType::Stratocumulus)
			horizontalRadius = std::min(horizontalRadius, 1.08 * std::min(cellX, cellZ));
		double verticalRadius = radiusBase * verticalMacroScale * (
			0.82 + 0.12 * randomValue(index, 0x621f3u)
		);
		double y;
		if (this->_parameters.type == CloudType::Stratocumulus)
		{
			// A shallow layer's nominal height can be smaller than its feature-scale
			// radii. Keep all three hero levels inside that vertical interval instead
			// of collapsing them onto the same center and clipping one giant disk.
			verticalRadius = std::min(verticalRadius, heroHeight * 0.44);
			const double edgeInset = verticalRadius;
			y = this->_minimum.getY() + lerp(
				edgeInset,
				std::max(edgeInset, heroHeight - edgeInset),
				t
			);
		}
		else
		{
			y = this->_minimum.getY() + feature * 0.18 + t * std::max(
				0.0,
				heroHeight - verticalRadius - feature * 0.18
			);
		}
		const Vector3 shear = shearDirection * (shearDistance * t * t);
		const Vector3 center(
			heroBaseX + heroWander.getX() + shear.getX(),
			y,
			heroBaseZ + heroWander.getZ() + shear.getZ()
		);
		appendLobe(
			center,
			Vector3(horizontalRadius, verticalRadius, horizontalRadius),
			1.0 - 0.18 * smoothstep(0.70, 1.0, t)
		);
		if (level > 0 && useCauliflowerShell)
		{
			appendCauliflowerShell(
				center,
				Vector3(horizontalRadius, verticalRadius, horizontalRadius),
				index,
				0.52 + 0.48 * smoothstep(0.16, 0.72, t),
				smoothstep(0.45, 0.92, t)
			);
		}
		if (
			this->_parameters.type == CloudType::Cumulonimbus
			&& crown > 0.06
		)
		{
			// Build the anvil from overlapping rounded cells rather than stretching
			// the primary plume into one analytic disk. The asymmetric reach follows
			// the shear while the crown envelope leaves the overshooting top round.
			const double baseAnvilRadius = radiusBase * (0.68 + 0.16 * crown);
			const double spreadSteps[6] = {-3.55, -2.70, -1.85, -1.00, 1.00, 2.00};
			for (int cell = 0; cell < 6; cell++)
			{
				const std::uint32_t cellIndex = index * 5u
					+ static_cast<std::uint32_t>(cell) + 0x317u;
				const double anvilRadius = baseAnvilRadius * (
					0.90 + 0.18 * randomValue(cellIndex, 0x93d71u)
				);
				const Vector3 lateral = shearDirection * (
					spreadSteps[cell] * horizontalRadius * (0.55 + 0.45 * crown)
				);
				const double anvilY = this->_minimum.getY() + heroHeight * (
					0.78 + 0.08 * smoothstep(0.76, 0.94, t)
				) + anvilRadius * 0.10 * (randomValue(cellIndex, 0x71c2bu) - 0.5);
				const Vector3 anvilCenter(
					center.getX() + lateral.getX(),
					anvilY,
					center.getZ() + lateral.getZ()
				);
				const Vector3 anvilRadii(
					anvilRadius * (1.26 + 0.16 * randomValue(cellIndex, 0x9ad31u)),
					anvilRadius * (0.72 + 0.08 * randomValue(cellIndex, 0x7ce2bu)),
					anvilRadius * (0.96 + 0.12 * randomValue(cellIndex, 0x4fae3u))
				);
				appendLobe(anvilCenter, anvilRadii, 0.94);
				if (useCauliflowerShell)
				{
					appendCauliflowerShell(
						anvilCenter,
						anvilRadii,
						cellIndex ^ 0xa173u,
						0.24 + 0.18 * crown,
						0.72
					);
				}
			}
		}
		// Coalesced neighboring updrafts broaden the congestus body without
		// inflating feature_scale (which would erase cauliflower detail).
		const double shoulderEnvelope = smoothstep(0.0, 0.18, t)
			* (1.0 - smoothstep(0.70, 0.94, t));
		if (shoulderEnvelope > 0.0 && macroScale > 0.85)
		{
			const double shoulderOffset = feature * macroScale
				* (0.68 + 0.34 * middle + 0.12 * randomValue(index, 0xa4e91u));
			const double shoulderRadius = radiusBase
				* (0.58 + 0.16 * clamp01(macroScale - 0.65));
			for (int side = -1; side <= 1; side += 2)
			{
				const double sideVariation = 0.88 + 0.20 * randomValue(
					index + static_cast<std::uint32_t>(side + 1),
					0x2f6cdu
				);
				const Vector3 shoulderCenter = center
					+ shoulderDirection * (static_cast<double>(side) * shoulderOffset)
					+ Vector3(
						0.0,
						verticalRadius * shoulderEnvelope * (-0.12 + 0.30 * sideVariation),
						0.0
					);
				appendLobe(
					shoulderCenter,
					Vector3(
						shoulderRadius * sideVariation,
						shoulderRadius * (0.72 + 0.12 * sideVariation),
						shoulderRadius * sideVariation
					),
					0.92 * shoulderEnvelope
				);
			}
		}

		// The Fibonacci shell above is the irregular multiscale boundary model.
		// The older azimuthal child loop below is retained only as the low-detail
		// fallback; running both produces visible rings of bead-like spheres.
		if (level == 0 || !useLegacyChildren)
			continue;
		const int childCount = this->_parameters.puffiness >= 0.84
			? (t >= 0.58 ? 9 : 6)
			: (this->_parameters.puffiness >= 0.58 ? 4 : 3);
		const double ringRotation = TWO_PI * randomValue(index, 0x4ba91u);
		for (int child = 0; child < childCount; child++)
		{
			const std::uint32_t childIndex = index * 8u + static_cast<std::uint32_t>(child);
			const double angle = ringRotation + TWO_PI * (
				static_cast<double>(child) / static_cast<double>(childCount)
				+ 0.18 * (randomValue(childIndex, 0xf13c7u) - 0.5)
			);
			const double childRadius = radiusBase * (
				0.28 + 0.20 * this->_parameters.puffiness
					* randomValue(childIndex, 0x65ab1u)
			) * (1.0 + 0.24 * smoothstep(0.64, 1.0, t));
			const double radialOffset = horizontalRadius * (
				0.90 + 0.09 * randomValue(childIndex, 0x912efu)
			);
			const double verticalOffset = verticalRadius * (
				-0.12 + 0.48 * randomValue(childIndex, 0x2d4a9u)
				+ 0.30 * smoothstep(0.52, 0.94, t)
			);
			const Vector3 childCenter = center + Vector3(
				std::cos(angle) * radialOffset,
				verticalOffset,
				std::sin(angle) * radialOffset
			);
			appendLobe(
				childCenter,
				Vector3(
					childRadius,
					childRadius * (0.76 + 0.24 * randomValue(childIndex, 0xe7d31u)),
					childRadius
				),
				0.90
			);

			const double expectedMicroCount = this->_parameters.fineDetail
				* (0.48 + 1.30 * smoothstep(0.18, 0.68, t));
			int microCount = static_cast<int>(std::floor(expectedMicroCount));
			if (
				randomValue(childIndex, 0x51e2bu)
				< expectedMicroCount - static_cast<double>(microCount)
			)
				microCount++;
			microCount = std::clamp(microCount, 0, 2);
			for (int micro = 0; micro < microCount; micro++)
			{
				const std::uint32_t microIndex = childIndex * 7u + static_cast<std::uint32_t>(micro);
				const double microAngle = angle + (randomValue(microIndex, 0x8cd19u) - 0.5) * 2.35;
				const double microRadius = childRadius * (
					0.32 + 0.20 * this->_parameters.fineDetail
					* randomValue(microIndex, 0x31af7u)
				);
				const double microOffset = childRadius * (
					0.80 + 0.14 * randomValue(microIndex, 0xc4813u)
				);
				const Vector3 microCenter = childCenter + Vector3(
					std::cos(microAngle) * microOffset,
					microRadius * (-0.15 + 0.90 * randomValue(microIndex, 0x51badu)),
					std::sin(microAngle) * microOffset
				);
				appendLobe(
					microCenter,
					Vector3(
						microRadius,
						microRadius * (0.74 + 0.24 * randomValue(microIndex, 0xe27c1u)),
						microRadius
					),
					0.78
				);
				if (
					t >= 0.42
					&& randomValue(microIndex, 0xc72a5u)
					< this->_parameters.fineDetail * 0.38
				)
				{
					const std::uint32_t nanoIndex = microIndex * 11u + 3u;
					const double nanoAngle = microAngle
						+ (randomValue(nanoIndex, 0x91f63u) - 0.5) * 2.10;
					const double nanoRadius = microRadius * (
						0.22 + 0.16 * randomValue(nanoIndex, 0x3d815u)
					);
					const double nanoOffset = microRadius * (
						0.66 + 0.18 * randomValue(nanoIndex, 0xa1c47u)
					);
					appendLobe(
						microCenter + Vector3(
							std::cos(nanoAngle) * nanoOffset,
							nanoRadius * (-0.25 + randomValue(nanoIndex, 0x71abdu)),
							std::sin(nanoAngle) * nanoOffset
						),
						Vector3(nanoRadius, nanoRadius * 0.82, nanoRadius),
						0.70
					);
				}
			}
		}
	}

	for (int z = 0; z < gridZ; z++)
	{
		for (int x = 0; x < gridX; x++)
		{
			const std::uint32_t column = static_cast<std::uint32_t>(z * gridX + x);
			double formation = 0.0;
			for (const Thermal& thermal : thermals)
			{
				const double dx = static_cast<double>(x) - thermal.x;
				const double dz = static_cast<double>(z) - thermal.z;
				formation = std::max(
					formation,
					thermal.strength * std::exp(-(dx * dx + dz * dz) / (2.0 * thermal.radius * thermal.radius))
				);
			}
			if (this->_parameters.type == CloudType::Stratocumulus)
			{
				// A shallow deck is organized by a continuous weather field, not a few
				// isolated convective updrafts. Use it only to select neighboring lobe
				// cells; density evaluation remains on the accelerated lobe path.
				const double weather = this->fbm(
					Vector3(
						static_cast<double>(x) * 0.22,
						static_cast<double>(this->_parameters.seed) * 0.0017 + 9.3,
						static_cast<double>(z) * 0.22
					),
					3,
					0x62c93d1u
				);
				formation = 0.74 * weather + 0.26 * formation;
			}
			if (this->_parameters.weatherVariation > 0.0)
			{
				const double weather = this->fbm(Vector3(x * 0.29, 1.73, z * 0.29), 2, 0x49178a3du);
				formation += this->_parameters.weatherVariation * 0.50 * (2.0 * weather - 1.0);
			}
			formation = clamp01(formation + 0.10 * (randomValue(column, 0x11a53u) - 0.5));
			if (formation < formationThreshold)
				continue;
			const double heroDX = static_cast<double>(x) - thermals[0].x;
			const double heroDZ = static_cast<double>(z) - thermals[0].z;
			const double heroWeight = std::exp(
				-(heroDX * heroDX + heroDZ * heroDZ) / (2.0 * 0.95 * 0.95)
			);
			const double supportFormation = std::pow(
				smoothstep(formationThreshold, 1.0, formation),
				1.35
			);
			const double heightFormation = supportFormation
				* (1.0 - 0.52 * this->_parameters.dominance)
				* (1.0 - 0.18 * heroWeight * this->_parameters.dominance);

			const double deckStagger = this->_parameters.type == CloudType::Stratocumulus
				? (z % 2 == 0 ? -0.20 : 0.20) * cellX
				: 0.0;
			const double cellJitter = (this->_parameters.type == CloudType::Stratocumulus
				? 0.82 : 0.58) + 0.30 * this->_parameters.weatherVariation;
			const double baseX = startX + static_cast<double>(x) * cellX + deckStagger
				+ (randomValue(column, 0x72e31u) - 0.5) * cellX * cellJitter;
			const double baseZ = startZ + static_cast<double>(z) * cellZ
				+ (randomValue(column, 0x93c47u) - 0.5) * cellZ * cellJitter;
			const double centerDistanceX = (baseX - this->_parameters.position.getX()) / (0.5 * this->_parameters.size.getX());
			const double centerDistanceZ = (baseZ - this->_parameters.position.getZ()) / (0.5 * this->_parameters.size.getZ());
			const double centerBias = std::max(0.55, 1.0 - 0.32 * std::sqrt(
				centerDistanceX * centerDistanceX + centerDistanceZ * centerDistanceZ
			));
			double heightFraction = 0.10 + this->_parameters.towering
				* (0.16 + 0.76 * heightFormation + 0.08 * randomValue(column, 0xb31d9u)) * centerBias;
			if (this->_parameters.type == CloudType::Stratocumulus)
				heightFraction = 0.16 + 0.32 * this->_parameters.towering * randomValue(column, 0x3ad17u);
			if (this->_parameters.type == CloudType::Cumulonimbus)
				heightFraction = std::min(0.98, heightFraction + 0.18 * centerBias);
			const double columnHeight = this->_parameters.size.getY() * std::min(0.96, heightFraction);
			const int levels = clampedCeilToInt(columnHeight / (feature * 0.72), 2, 10);
			Vector3 wander(0.0, 0.0, 0.0);

			for (int level = 0; level < levels; level++)
			{
				const double t = levels > 1 ? static_cast<double>(level) / static_cast<double>(levels - 1) : 0.0;
				const std::uint32_t index = column * 16u + static_cast<std::uint32_t>(level);
				double radiusBase = feature * (
					1.12 - 0.38 * t + 0.42 * randomValue(index, 0xd872fu)
				);
				// Neighboring columns should overlap into an irregular field. Bound each
				// primitive by the actual grid pitch so a large feature scale cannot turn
				// one support cell into the dominant visible disk.
				const double gridRadius = (
					this->_parameters.type == CloudType::Stratocumulus ? 1.08 : 0.76
				) * std::min(cellX, cellZ);
				const double gridVariation = this->_parameters.type == CloudType::Stratocumulus
					? 0.72 + 0.28 * randomValue(index, 0x5f73du)
					: 1.0;
				radiusBase = std::min(radiusBase, gridRadius * gridVariation);
				const double crown = this->_parameters.overhang
					* smoothstep(0.68, 0.84, t)
					* (1.0 - smoothstep(0.88, 0.96, t))
					* smoothstep(0.42, 0.88, heightFormation);
				const double crownExpansion = this->_parameters.type == CloudType::Cumulonimbus
					? 0.72
					: 0.20;
				const double horizontalRadius = radiusBase * (1.0 + crownExpansion * crown);
				if (level > 0)
				{
					wander += Vector3(
						(randomValue(index, 0x18e4du) - 0.5) * feature * 0.34,
						0.0,
						(randomValue(index, 0xa24b3u) - 0.5) * feature * 0.34
					);
				}
				const double verticalScale = level == 0
					? 0.78
					: (this->_parameters.type == CloudType::Stratocumulus ? 0.78 : 0.94);
				double verticalRadius = radiusBase * verticalScale;
				double y;
				if (this->_parameters.type == CloudType::Stratocumulus)
				{
					// Two shallow-layer levels must occupy different heights and overlap;
					// feature-scale radii larger than the column made both centers collapse.
					verticalRadius = std::min(verticalRadius, columnHeight * 0.44);
					const double edgeInset = verticalRadius;
					y = this->_minimum.getY() + lerp(
						edgeInset,
						std::max(edgeInset, columnHeight - edgeInset),
						t
					);
				}
				else
				{
					y = this->_minimum.getY() + feature * 0.20 + t * std::max(
						0.0,
						columnHeight - verticalRadius - feature * 0.20
					);
				}
				const Vector3 shear = shearDirection * (shearDistance * t * t);
				const Vector3 center(baseX + wander.getX() + shear.getX(), y, baseZ + wander.getZ() + shear.getZ());
				const Vector3 parentRadii(horizontalRadius, verticalRadius, horizontalRadius);
				appendLobe(center, parentRadii, 1.0);
				if (level > 0 && useCauliflowerShell)
				{
					appendCauliflowerShell(
						center,
						parentRadii,
						index ^ 0x8d31u,
						0.38 + 0.34 * t,
						0.35 * t
					);
				}

				if (level == 0 || !useLegacyChildren)
					continue;
				const int childCount = this->_parameters.puffiness > 0.72 ? 2 : 1;
				for (int child = 0; child < childCount; child++)
				{
					const std::uint32_t childIndex = index * 3u + static_cast<std::uint32_t>(child);
					const double angle = TWO_PI * randomValue(childIndex, 0xf13c7u);
					const double childRadius = radiusBase * (
						0.32 + 0.28 * this->_parameters.puffiness * randomValue(childIndex, 0x65ab1u)
					);
					const double radialOffset = horizontalRadius * (0.48 + 0.24 * randomValue(childIndex, 0x912efu));
					const double verticalOffset = radiusBase * (0.08 + 0.46 * randomValue(childIndex, 0x2d4a9u));
					appendLobe(
						center + Vector3(
							std::cos(angle) * radialOffset,
							verticalOffset,
							std::sin(angle) * radialOffset
						),
						Vector3(childRadius, childRadius * (0.78 + 0.24 * randomValue(childIndex, 0xe7d31u)), childRadius),
						0.88
					);
					const double expectedMicroCount = this->_parameters.fineDetail
						* (0.30 + 0.85 * smoothstep(0.28, 0.82, t));
					int microCount = static_cast<int>(std::floor(expectedMicroCount));
					if (
						randomValue(childIndex, 0x71e4bu)
						< expectedMicroCount - static_cast<double>(microCount)
					)
						microCount++;
					microCount = std::clamp(microCount, 0, 2);
					for (int micro = 0; micro < microCount; micro++)
					{
						const std::uint32_t microIndex = childIndex * 5u + static_cast<std::uint32_t>(micro);
						const double microAngle = angle + (randomValue(microIndex, 0x8cd19u) - 0.5) * 3.14159265358979323846;
						const double microRadius = childRadius * (
							0.28 + 0.22 * this->_parameters.fineDetail * randomValue(microIndex, 0x31af7u)
						);
						const double microOffset = childRadius * (0.68 + 0.20 * randomValue(microIndex, 0xc4813u));
						const Vector3 childCenter = center + Vector3(
							std::cos(angle) * radialOffset,
							verticalOffset,
							std::sin(angle) * radialOffset
						);
						appendLobe(
							childCenter + Vector3(
								std::cos(microAngle) * microOffset,
								microRadius * (0.15 + 0.65 * randomValue(microIndex, 0x51badu)),
								std::sin(microAngle) * microOffset
							),
							Vector3(microRadius, microRadius * (0.76 + 0.25 * randomValue(microIndex, 0xe27c1u)), microRadius),
							0.74
						);
					}
				}
			}
		}
	}

	if (this->_lobes.empty())
	{
		const double radius = std::min(feature, 0.24 * this->_parameters.size.getX());
		appendLobe(
			Vector3(this->_parameters.position.getX(), this->_minimum.getY() + radius * 0.45, this->_parameters.position.getZ()),
			Vector3(radius, radius * 0.45, radius),
			1.0
		);
	}
	this->buildLobeGrid();
}

void CloudVolume::buildLobeGrid(void)
{
	const double cellSize = std::max(1e-6, this->_parameters.featureScale);
	this->_lobeGridX = clampedCeilToInt(this->_parameters.size.getX() / cellSize, 1, 64);
	this->_lobeGridY = clampedCeilToInt(this->_parameters.size.getY() / cellSize, 1, 64);
	this->_lobeGridZ = clampedCeilToInt(this->_parameters.size.getZ() / cellSize, 1, 64);
	this->_lobeGrid.assign(
		static_cast<std::size_t>(this->_lobeGridX * this->_lobeGridY * this->_lobeGridZ),
		{}
	);

	auto coordinate = [](double value, double minimum, double extent, int count) {
		return (std::clamp(
			static_cast<int>(std::floor((value - minimum) / extent * static_cast<double>(count))),
			0,
			count - 1
		));
	};
	for (std::uint32_t lobeIndex = 0; lobeIndex < this->_lobes.size(); lobeIndex++)
	{
		const Lobe& lobe = this->_lobes[lobeIndex];
		// densityAt warps lookup positions near the boundary. Inflate occupancy by
		// the maximum warp so an empty cell remains a conservative zero-majorant
		// region for delta tracking.
		const double padding = this->_parameters.featureScale * (
			0.16 + 0.12 * this->_parameters.fineDetail
				+ 0.18 * this->_parameters.erosion
		);
		const int minX = coordinate(lobe.center.getX() - lobe.radius.getX() - padding, this->_minimum.getX(), this->_parameters.size.getX(), this->_lobeGridX);
		const int maxX = coordinate(lobe.center.getX() + lobe.radius.getX() + padding, this->_minimum.getX(), this->_parameters.size.getX(), this->_lobeGridX);
		const int minY = coordinate(lobe.center.getY() - lobe.radius.getY() - padding, this->_minimum.getY(), this->_parameters.size.getY(), this->_lobeGridY);
		const int maxY = coordinate(lobe.center.getY() + lobe.radius.getY() + padding, this->_minimum.getY(), this->_parameters.size.getY(), this->_lobeGridY);
		const int minZ = coordinate(lobe.center.getZ() - lobe.radius.getZ() - padding, this->_minimum.getZ(), this->_parameters.size.getZ(), this->_lobeGridZ);
		const int maxZ = coordinate(lobe.center.getZ() + lobe.radius.getZ() + padding, this->_minimum.getZ(), this->_parameters.size.getZ(), this->_lobeGridZ);
		for (int z = minZ; z <= maxZ; z++)
			for (int y = minY; y <= maxY; y++)
				for (int x = minX; x <= maxX; x++)
				{
					const std::size_t gridIndex = static_cast<std::size_t>(
						(z * this->_lobeGridY + y) * this->_lobeGridX + x
					);
					this->_lobeGrid[gridIndex].push_back(lobeIndex);
				}
	}
}

void CloudVolume::buildDensityMajorants(void)
{
	_densityMajorants.assign(_lobeGrid.size(), 0.0);
	const Vector3 cell(_parameters.size.getX() / _lobeGridX,
		_parameters.size.getY() / _lobeGridY, _parameters.size.getZ() / _lobeGridZ);
	const double warp = _parameters.featureScale *
		(0.16 + 0.12 * _parameters.fineDetail + 0.18 * _parameters.erosion);
	const Vector3 padding(warp * 0.5, warp * 0.36, warp * 0.5);
	for (int z = 0; z < _lobeGridZ; z++)
	for (int y = 0; y < _lobeGridY; y++)
	for (int x = 0; x < _lobeGridX; x++)
	{
		const std::size_t index = (z * _lobeGridY + y) * _lobeGridX + x;
		if (_parameters.coverage <= 0.0)
			continue;
		double bound = 0.0;
		if (!_lobes.empty())
		{
			// Interval bound over the entire cell enlarged by the maximum warp.
			// Every later density operation only removes energy from this union.
			const Vector3 low = _minimum + Vector3(x * cell.getX(), y * cell.getY(), z * cell.getZ()) - padding;
			const Vector3 high = low + cell + padding * 2.0;
			double energy = 0.0;
			for (const auto lobeIndex : _lobeGrid[index])
			{
				const auto& lobe = _lobes[lobeIndex];
				double distanceSquared = 0.0;
				for (int axis = 0; axis < 3; axis++)
				{
					const double distance = std::max({low[axis] - lobe.center[axis],
						lobe.center[axis] - high[axis], 0.0}) / lobe.radius[axis];
					distanceSquared += distance * distance;
				}
				const double contribution = lobe.strength * (1.0 - smoothstep(
					lerp(0.52, 0.72, _parameters.puffiness), 1.0, std::sqrt(distanceSquared)));
				energy += contribution * contribution * contribution;
				if (energy >= 1.0)
					break;
			}
			bound = smoothstep(0.025, 0.34, std::cbrt(energy));
		}
		else
		{
			// Weather/structure are in [0,1]. Bound the increasing and decreasing
			// factors at opposite ends of the height interval, never at midpoints.
			const double low = static_cast<double>(y) / _lobeGridY;
			const double high = static_cast<double>(y + 1) / _lobeGridY;
			if (_parameters.type == CloudType::Cirrus)
				bound = smoothstep(0.04, 0.20, high) * (1.0 - smoothstep(0.68, 0.96, low));
			else if (_parameters.type == CloudType::Stratus)
				bound = smoothstep(0.015, 0.055, high) * (1.0 - smoothstep(0.79, 0.97, low));
			else
				bound = 1.0;
			bound = smoothstep(0.025, 0.38, bound);
		}
		// Retain a small rounding margin without turning proven empty cells on.
		_densityMajorants[index] = bound > 0.0 ? std::min(1.0, bound + 1e-9) : 0.0;
	}
}

double CloudVolume::densityMajorantAt(const Vector3& position) const
{
	for (int axis = 0; axis < 3; axis++)
		if (!std::isfinite(position[axis]) || position[axis] < _minimum[axis] || position[axis] > _maximum[axis])
			return 0.0;
	const Vector3 p = position + (_lobes.empty() ? Vector3(0.0, 0.0, 0.0) : _parameters.offset);
	const int counts[3] = {_lobeGridX, _lobeGridY, _lobeGridZ};
	int coordinates[3];
	for (int axis = 0; axis < 3; axis++)
		coordinates[axis] = static_cast<int>(std::clamp(std::floor(
			(p[axis] - _minimum[axis]) / _parameters.size[axis] * counts[axis]), 0.0,
			static_cast<double>(counts[axis] - 1)));
	return _densityMajorants[(coordinates[2] * _lobeGridY + coordinates[1]) * _lobeGridX + coordinates[0]];
}

double CloudVolume::convectiveLobeField(const Vector3& position) const
{
	if (
		this->_lobeGrid.empty()
		|| position.getX() < this->_minimum.getX() || position.getX() > this->_maximum.getX()
		|| position.getY() < this->_minimum.getY() || position.getY() > this->_maximum.getY()
		|| position.getZ() < this->_minimum.getZ() || position.getZ() > this->_maximum.getZ()
	)
		return (0.0);
	auto coordinate = [](double value, double minimum, double extent, int count) {
		return (std::clamp(
			static_cast<int>(std::floor((value - minimum) / extent * static_cast<double>(count))),
			0,
			count - 1
		));
	};
	const int x = coordinate(position.getX(), this->_minimum.getX(), this->_parameters.size.getX(), this->_lobeGridX);
	const int y = coordinate(position.getY(), this->_minimum.getY(), this->_parameters.size.getY(), this->_lobeGridY);
	const int z = coordinate(position.getZ(), this->_minimum.getZ(), this->_parameters.size.getZ(), this->_lobeGridZ);
	const std::size_t gridIndex = static_cast<std::size_t>((z * this->_lobeGridY + y) * this->_lobeGridX + x);
	double cubicEnergy = 0.0;
	const double denseCore = lerp(0.52, 0.72, this->_parameters.puffiness);
	for (const std::uint32_t lobeIndex : this->_lobeGrid[gridIndex])
	{
		const Lobe& lobe = this->_lobes[lobeIndex];
		const double dx = (position.getX() - lobe.center.getX()) / lobe.radius.getX();
		const double dy = (position.getY() - lobe.center.getY()) / lobe.radius.getY();
		const double dz = (position.getZ() - lobe.center.getZ()) / lobe.radius.getZ();
		const double distanceSquared = dx * dx + dy * dy + dz * dz;
		if (distanceSquared >= 1.0)
			continue;
		const double contribution = lobe.strength * (
			1.0 - smoothstep(denseCore, 1.0, std::sqrt(distanceSquared))
		);
		// A cubic metaball union gives overlapping cells a continuous neck while
		// avoiding the broad inflation of a quadratic sum. The small iso threshold
		// below also keeps the compact ellipsoid support from becoming the rendered
		// silhouette verbatim.
		cubicEnergy += contribution * contribution * contribution;
		if (cubicEnergy >= 1.0)
			return (1.0);
	}
	return (clamp01(std::cbrt(cubicEnergy)));
}

double CloudVolume::densityAt(const Vector3& position) const
{
	if (this->_parameters.coverage <= 0.0)
		return (0.0);
	if (
		position.getX() < this->_minimum.getX() || position.getX() > this->_maximum.getX()
		|| position.getY() < this->_minimum.getY() || position.getY() > this->_maximum.getY()
		|| position.getZ() < this->_minimum.getZ() || position.getZ() > this->_maximum.getZ()
	)
		return (0.0);

	// The same conservative bounds used by tracking also reject empty primary
	// and shadow quadrature points before evaluating any noise.
	if (_parameters.localMajorants && !_densityMajorants.empty() && densityMajorantAt(position) <= 0.0)
		return 0.0;

	const double height = (position.getY() - this->_minimum.getY()) / this->_parameters.size.getY();
	const Vector3 advected = position + this->_parameters.offset;
	const double featureScale = this->_parameters.featureScale;
	const Vector3 basePosition(
		advected.getX() / featureScale,
		advected.getY() / (featureScale * 0.78),
		advected.getZ() / featureScale
	);
	if (!this->_lobes.empty())
	{
		const Vector3 warpPosition(
			advected.getX() / (featureScale * 1.05),
			advected.getY() / (featureScale * 1.05),
			advected.getZ() / (featureScale * 1.05)
		);
		// The lobe union is a macro envelope, not a collection of visible analytic
		// ellipsoids. A feature-scale displacement gives its boundary the broad,
		// coherent breakup seen in real convection before the finer erosion pass.
		const double warpAmplitude = featureScale * (
			0.16 + 0.12 * this->_parameters.fineDetail
				+ 0.18 * this->_parameters.erosion
		);
		const double warpX = this->gradientNoise(warpPosition, 0x8193abu);
		const double warpY = this->gradientNoise(
			warpPosition + Vector3(17.1, -3.7, 9.2),
			0x4d12efu
		);
		const double warpZ = this->gradientNoise(
			warpPosition + Vector3(-8.3, 14.6, 5.1),
			0xb7315u
		);
		const Vector3 warped = advected + Vector3(
			(warpX - 0.5) * warpAmplitude,
			(warpY - 0.5) * warpAmplitude * 0.72,
			(warpZ - 0.5) * warpAmplitude
		);
		double density = this->convectiveLobeField(warped);
		density *= smoothstep(0.0, 0.022, height);
		if (this->_parameters.baseVariation > 0.0 && density > 0.0)
		{
			const double baseWeather = this->fbm(Vector3(
				basePosition.getX() * 0.22, 0.73, basePosition.getZ() * 0.22), 2, 0x731ca91du);
			const double localBase = this->_parameters.baseVariation * (0.035 + 0.14 * baseWeather);
			density *= smoothstep(localBase, localBase + 0.028, height);
		}
		density *= 1.0 - smoothstep(0.97, 1.0, height);
		if (density <= 0.0)
			return (0.0);
		const bool shallowDeck = this->_parameters.type == CloudType::Stratocumulus;
		if (!shallowDeck && density >= 0.80)
			return 1.0;
		const Vector3 shapePosition = shallowDeck
			? Vector3(
				basePosition.getX() * 0.38 + 8.7,
				static_cast<double>(this->_parameters.seed) * 0.0019 - 15.3,
				basePosition.getZ() * 0.38 + 4.1
			)
			: Vector3(
				basePosition.getX() * 0.82 + 8.7,
				basePosition.getY() * 0.74 - 15.3,
				basePosition.getZ() * 0.82 + 4.1
			);
		const double shapeNoise = this->fbm(
			shapePosition,
			shallowDeck ? 3 : 2,
			0x93a4d71bu
		);
		const double shapeBillow = 1.0 - std::fabs(2.0 * shapeNoise - 1.0);
		const double shapeSignal = 0.72 * shapeNoise + 0.28 * shapeBillow;
		double boundarySignal = shapeSignal;

		if (shallowDeck)
		{
			// Intersect the overlapping shallow cells with a coherent, spatially
			// varying condensation top. Without this mask their clipped ellipsoid
			// caps remain visible as a regular row when viewed near the horizon.
			const double deckDetail = this->fbm(
				Vector3(
					basePosition.getX() * 1.34 - 3.1,
					static_cast<double>(this->_parameters.seed) * 0.0023 + 6.7,
					basePosition.getZ() * 1.34 + 11.9
				),
				3,
				0x51a7ce3u
			);
			const double deckBillow = 1.0 - std::fabs(2.0 * deckDetail - 1.0);
			const double deckSignal = 0.48 * shapeSignal
				+ 0.36 * deckDetail + 0.16 * deckBillow;
			boundarySignal = deckSignal;
			const double localTop = 0.10 + 0.06 * this->_parameters.coverage
				+ 0.40 * this->_parameters.towering * (0.12 + 0.88 * deckSignal)
				+ 0.035 * this->_parameters.fineDetail * (2.0 * deckDetail - 1.0);
			density *= 1.0 - smoothstep(localTop - 0.075, localTop, height);
		}
		if (density <= 0.0)
			return (0.0);

		if (density >= 0.80)
			return 1.0;
		const double detailFrequency = lerp(3.2, 6.0, this->_parameters.fineDetail);
		const Vector3 detailPosition(
			basePosition.getX() * detailFrequency + 19.7,
			basePosition.getY() * detailFrequency - 7.3,
			basePosition.getZ() * detailFrequency + 31.1
		);
		const double detailNoise = this->fbm(detailPosition, this->_parameters.detailOctaves, 0x51ed270bu);
		const double billow = 1.0 - std::fabs(2.0 * detailNoise - 1.0);
		const double erosionSignal = 0.62 * detailNoise + 0.38 * billow;
		const double edgeWeight = 1.0 - smoothstep(0.34, 0.80, density);
		// Shift the broad boundary inward with mesostructure at roughly the lobe
		// scale. This is deliberately independent of the fine erosion control:
		// even a clean cloud must not reveal its construction ellipsoids.
		density -= edgeWeight * (0.08 + 0.28 * this->_parameters.fineDetail)
			* (0.18 + 0.82 * (1.0 - boundarySignal));
		density -= this->_parameters.erosion * this->_parameters.detail
			* edgeWeight * (1.0 - erosionSignal) * 0.46;
		// Real cumulus interiors are optically smooth and dense. Restrict the
		// high-frequency field to the condensation boundary, then remap that
		// boundary into a compact transition instead of imprinting noise through
		// the whole volume.
		density = smoothstep(0.025, 0.34, density);
		return (clamp01(density));
	}
	const Vector3 weatherPosition(
		advected.getX() / (featureScale * 4.6),
		this->_parameters.seed * 0.001,
		advected.getZ() / (featureScale * 4.6)
	);
	const double weather = this->fbm(weatherPosition, 3, 0x2468aceu);
	const double growthNoise = this->fbm(Vector3(
		advected.getX() / (featureScale * 1.75),
		this->_parameters.seed * 0.002 + 13.7,
		advected.getZ() / (featureScale * 1.75)
	), 3, 0x7412bc3u);
	double carrier = 0.68 * weather + 0.32 * growthNoise;

	if (this->_parameters.type == CloudType::Cirrus)
	{
		const double angle = 6.28318530717958647692
			* hashUnit(mixBits(this->_parameters.seed ^ 0x7af31u));
		Vector3 wind(
			this->_parameters.shearDirection.getX(),
			0.0,
			this->_parameters.shearDirection.getZ()
		);
		if (Utilities::vectorLengthSquared(wind) <= CLOUD_EPSILON)
			wind = Vector3(std::cos(angle), 0.0, std::sin(angle));
		else
			wind = Utilities::normalize(wind);
		const double along = basePosition.getX() * wind.getX()
			+ basePosition.getZ() * wind.getZ();
		const double across = -basePosition.getX() * wind.getZ()
			+ basePosition.getZ() * wind.getX();
		const double streak = this->fbm(Vector3(
			along * lerp(0.46, 0.14, this->_parameters.overhang),
			basePosition.getY() * 2.4,
			across * lerp(1.8, 3.0, this->_parameters.fineDetail)
		), 3, 0xc1aa551u);
		carrier = 0.52 * carrier + 0.48 * (1.0 - std::fabs(2.0 * streak - 1.0));
	}

	const double threshold = 1.0 - this->_parameters.coverage;
	const double coverageWidth = std::max(0.045, this->_parameters.coverage * 0.24);
	const double coverageMask = smoothstep(threshold - 0.075, threshold + coverageWidth, carrier);
	if (coverageMask <= 0.0)
		return 0.0;
	double structure = carrier;
	if (this->_parameters.type != CloudType::Cirrus)
	{
		const double baseNoise = this->fbm(basePosition, 3, 0x13579bdu);
		const double puffs = this->cellularPuffs(
			advected,
			featureScale * 0.78,
			0x4f1bbcddu
		);
		structure = lerp(
			baseNoise,
			std::max(puffs, baseNoise * 0.72),
			this->_parameters.puffiness
		);
	}
	double density = coverageMask * structure;
	density *= this->verticalProfile(height, growthNoise, coverageMask);
	if (density <= 0.0)
		return (0.0);

	const double detailFrequency = lerp(3.4, 6.8, this->_parameters.fineDetail);
	const Vector3 detailPosition(
		basePosition.getX() * detailFrequency + 19.7,
		basePosition.getY() * detailFrequency - 7.3,
		basePosition.getZ() * detailFrequency + 31.1
	);
	const double detailNoise = this->fbm(
		detailPosition,
		this->_parameters.detailOctaves,
		0x51ed270bu
	);
	const double billow = 1.0 - std::fabs(2.0 * detailNoise - 1.0);
	const double erosionSignal = 0.58 * detailNoise + 0.42 * billow;
	const double edgeWeight = 1.0 - smoothstep(0.20, 0.64, density);
	density -= this->_parameters.erosion * this->_parameters.detail * edgeWeight * (1.0 - erosionSignal) * 0.58;
	density = smoothstep(0.025, 0.38, density);
	return (clamp01(density));
}

bool CloudVolume::integrationInterval(
	const Ray& ray,
	double t_min,
	double t_max,
	double& entryT,
	double& exitT
) const
{
	return (this->boundsInterval(ray, t_min, t_max, entryT, exitT));
}

double CloudVolume::extinctionAt(const Vector3& position) const
{
	return (this->_majorantSceneUnits * this->densityAt(position));
}

Color CloudVolume::singleScatteringCoefficientAt(
	const Vector3& position,
	const Vector3& incidentDirection,
	const Vector3& scatteredDirection
) const
{
	return singleScatteringWithExtinction(position, incidentDirection, scatteredDirection, extinctionAt(position));
}

Color CloudVolume::singleScatteringWithExtinction(
	const Vector3& position, const Vector3& incidentDirection,
	const Vector3& scatteredDirection, double extinction
) const
{
	if (extinction <= 0.0)
		return (Color(0.0, 0.0, 0.0));
	HitRecord hitRecord;
	hitRecord.position = position;
	hitRecord.material = this->_phaseFunction.get();
	const Ray incidentRay = Ray::fromNormalizedDirection(position, incidentDirection);
	const double phase = this->_phaseFunction->scatteringPDF(
		incidentRay,
		hitRecord,
		scatteredDirection
	);
	if (phase <= 0.0 || !std::isfinite(phase))
		return (Color(0.0, 0.0, 0.0));
	return (this->_parameters.albedo * (extinction * phase));
}

void CloudVolume::lobeGridCell(
	const Ray& ray,
	double t,
	double exitT,
	double boundaryEpsilonT,
	double& densityMajorant,
	double& cellExitT
) const
{
	if (this->_lobeGrid.empty())
	{
		densityMajorant = 1.0;
		cellExitT = exitT;
		return;
	}
	const double probeT = std::min(exitT, t + boundaryEpsilonT);
	// densityAt evaluates the lobe field in position + offset space before
	// applying its bounded noise warp. The grid already pads every lobe by the
	// maximum warp, so traversing that same translated field space preserves
	// empty-cell skipping without discarding shifted density.
	const Vector3 fieldOrigin = ray.getOrigin()
		+ (this->_lobes.empty() ? Vector3(0.0, 0.0, 0.0) : this->_parameters.offset);
	const Vector3 point = fieldOrigin + ray.getDirection() * probeT;
	auto coordinate = [](double value, double minimum, double extent, int count) {
		return (std::clamp(
			static_cast<int>(std::floor((value - minimum) / extent * static_cast<double>(count))),
			0,
			count - 1
		));
	};
	const int x = coordinate(point.getX(), this->_minimum.getX(), this->_parameters.size.getX(), this->_lobeGridX);
	const int y = coordinate(point.getY(), this->_minimum.getY(), this->_parameters.size.getY(), this->_lobeGridY);
	const int z = coordinate(point.getZ(), this->_minimum.getZ(), this->_parameters.size.getZ(), this->_lobeGridZ);
	const std::size_t gridIndex = static_cast<std::size_t>(
		(z * this->_lobeGridY + y) * this->_lobeGridX + x
	);
	densityMajorant = this->_densityMajorants[gridIndex];
	if (!this->_parameters.localMajorants && densityMajorant > 0.0)
		densityMajorant = 1.0;
	const int coordinates[3] = {x, y, z};
	const int counts[3] = {this->_lobeGridX, this->_lobeGridY, this->_lobeGridZ};
	cellExitT = exitT;
	for (int axis = 0; axis < 3; axis++)
	{
		const double direction = ray.getDirection()[axis];
		if (std::fabs(direction) <= CLOUD_EPSILON)
			continue;
		const double cellExtent = this->_parameters.size[axis]
			/ static_cast<double>(counts[axis]);
		double boundary;
		if (point[axis] < this->_minimum[axis])
		{
			if (direction < 0.0)
				continue;
			boundary = this->_minimum[axis];
		}
		else if (point[axis] > this->_maximum[axis])
		{
			if (direction > 0.0)
				continue;
			boundary = this->_maximum[axis];
		}
		else
		{
			boundary = this->_minimum[axis] + cellExtent * (
				direction > 0.0
					? static_cast<double>(coordinates[axis] + 1)
					: static_cast<double>(coordinates[axis])
			);
		}
		const double boundaryT = (boundary - fieldOrigin[axis]) / direction;
		if (boundaryT > t + boundaryEpsilonT * 0.25)
			cellExitT = std::min(cellExitT, boundaryT);
	}
}

bool CloudVolume::sampleCollision(Ray& ray, double t_min, double t_max, double& hitT) const
{
	double entryT;
	double exitT;
	if (!this->boundsInterval(ray, t_min, t_max, entryT, exitT))
		return (false);
	entryT = std::max(entryT, 0.0);
	if (entryT >= exitT)
		return (false);

	const double rayLength = Utilities::vectorLength(ray.getDirection());
	if (!std::isfinite(rayLength) || rayLength <= 0.0)
		return (false);
	const double depthScale = Sampler::isReferenceVolumeTransport() ? 1.0 : std::pow(
		this->_parameters.multipleScatteringFalloff,
		static_cast<double>(Sampler::currentBounce())
	);
	const double rate = this->_majorantSceneUnits * depthScale * rayLength;
	if (!std::isfinite(rate) || rate <= 0.0)
		return (false);
	double currentT = entryT;
	const double boundaryEpsilonT = std::max(
		1e-10,
		this->_parameters.featureScale * 1e-7 / rayLength
	);
	for (std::uint32_t step = 0; ; step++)
	{
		double densityMajorant;
		double cellExitT;
		this->lobeGridCell(ray, currentT, exitT, boundaryEpsilonT, densityMajorant, cellExitT);
		if (densityMajorant <= 0.0)
		{
			currentT = cellExitT + boundaryEpsilonT;
			if (currentT >= exitT || !std::isfinite(currentT))
				return (false);
			continue;
		}
		// Hash the full cloud/bounce/null-event tuple into a 32-dimension block.
		// The sampler adds its own bounce stride, so an additive step stride of 32
		// would alias step N with bounce N. Preserving the low five bits also keeps
		// the free-flight and acceptance dimensions on their progressive sequences.
		const std::uint32_t tupleHash = mixBits(
			this->_samplingStream
			^ mixBits(step + 0x9e3779b9u)
			^ mixBits(Sampler::currentBounce() + 0x85ebca6bu)
		);
		const std::uint32_t dimension = CLOUD_TRACKING_DIMENSION
			+ ((tupleHash & 0x01ffffffu) << 5u);
		const double freeFlight = -std::log(std::max(1e-12, 1.0 - Sampler::sample1D(dimension))) / (rate * densityMajorant);
		const double collisionT = currentT + freeFlight;
		if (collisionT >= cellExitT)
		{
			currentT = cellExitT + boundaryEpsilonT;
			if (currentT >= exitT || !std::isfinite(currentT))
				return (false);
			continue;
		}
		currentT = collisionT;
		if (currentT >= exitT || !std::isfinite(currentT))
			return (false);
		const double density = this->densityAt(ray.pointAtRay(currentT));
		if (density > 0.0 && Sampler::sample1D(dimension + CLOUD_ACCEPTANCE_OFFSET) < density / densityMajorant)
		{
			hitT = currentT;
			return (true);
		}
		if (step == std::numeric_limits<std::uint32_t>::max())
			return (false);
	}
}

bool CloudVolume::stableFeatureCollision(
	const Ray& ray,
	double t_min,
	double t_max,
	double& hitT,
	double& density
) const
{
	double entryT;
	double exitT;
	if (!this->boundsInterval(ray, t_min, t_max, entryT, exitT))
		return (false);
	entryT = std::max(0.0, entryT);
	if (entryT >= exitT)
		return (false);
	const double rayLength = Utilities::vectorLength(ray.getDirection());
	if (!std::isfinite(rayLength) || rayLength <= 0.0)
		return (false);
	const double interval = exitT - entryT;
	const int maximumSteps = std::min(192, this->_parameters.maxTrackingSteps);
	const int minimumSteps = std::min(24, maximumSteps);
	const int stepCount = clampedCeilToInt(
		interval * rayLength / (this->_parameters.featureScale * 0.08),
		minimumSteps,
		maximumSteps
	);
	const double stepT = interval / static_cast<double>(stepCount);
	const double stepDistance = stepT * rayLength;
	double opticalDepth = 0.0;
	for (int step = 0; step < stepCount; step++)
	{
		const double sampleT = entryT + (static_cast<double>(step) + 0.5) * stepT;
		const double segmentOpticalDepth = this->_majorantSceneUnits
			* this->densityAt(ray.pointAtRay(sampleT)) * stepDistance;
		opticalDepth += segmentOpticalDepth;
		if (opticalDepth >= CLOUD_FEATURE_OPACITY_THRESHOLD)
		{
			hitT = sampleT;
			const Vector3 featurePosition = ray.pointAtRay(sampleT);
			density = this->densityAt(featurePosition);
			return (true);
		}
	}
	return (false);
}

Vector3 CloudVolume::featureNormalAt(const Vector3& position) const
{
	const Vector3 fieldPosition = position + this->_parameters.offset;
	// The denoiser must see the same boundary hierarchy as transport. A coarse
	// nearest-lobe normal turns genuine cloudlets into circular regression
	// artifacts, so prefer a deterministic fine-density gradient and retain the
	// analytic lobe gradient only as a flat-field fallback.
	const double detailEpsilon = std::max(1e-3, this->_parameters.featureScale * 0.028);
	const Vector3 detailGradient(
		this->densityAt(position + Vector3(detailEpsilon, 0.0, 0.0))
			- this->densityAt(position - Vector3(detailEpsilon, 0.0, 0.0)),
		this->densityAt(position + Vector3(0.0, detailEpsilon, 0.0))
			- this->densityAt(position - Vector3(0.0, detailEpsilon, 0.0)),
		this->densityAt(position + Vector3(0.0, 0.0, detailEpsilon))
			- this->densityAt(position - Vector3(0.0, 0.0, detailEpsilon))
	);
	if (Utilities::vectorLengthSquared(detailGradient) > 1e-12)
		return (Utilities::normalize(detailGradient * -1.0));
	if (!this->_lobeGrid.empty())
	{
		auto coordinate = [](double value, double minimum, double extent, int count) {
			return (std::clamp(
				static_cast<int>(std::floor((value - minimum) / extent * static_cast<double>(count))),
				0,
				count - 1
			));
		};
		if (
			fieldPosition.getX() >= this->_minimum.getX() && fieldPosition.getX() <= this->_maximum.getX()
			&& fieldPosition.getY() >= this->_minimum.getY() && fieldPosition.getY() <= this->_maximum.getY()
			&& fieldPosition.getZ() >= this->_minimum.getZ() && fieldPosition.getZ() <= this->_maximum.getZ()
		)
		{
			const int x = coordinate(fieldPosition.getX(), this->_minimum.getX(), this->_parameters.size.getX(), this->_lobeGridX);
			const int y = coordinate(fieldPosition.getY(), this->_minimum.getY(), this->_parameters.size.getY(), this->_lobeGridY);
			const int z = coordinate(fieldPosition.getZ(), this->_minimum.getZ(), this->_parameters.size.getZ(), this->_lobeGridZ);
			const std::size_t gridIndex = static_cast<std::size_t>((z * this->_lobeGridY + y) * this->_lobeGridX + x);
			double bestDistance = std::numeric_limits<double>::infinity();
			Vector3 bestGradient(0.0, 1.0, 0.0);
			for (const std::uint32_t lobeIndex : this->_lobeGrid[gridIndex])
			{
				const Lobe& lobe = this->_lobes[lobeIndex];
				const Vector3 difference = fieldPosition - lobe.center;
				const double dx = difference.getX() / lobe.radius.getX();
				const double dy = difference.getY() / lobe.radius.getY();
				const double dz = difference.getZ() / lobe.radius.getZ();
				const double distance = dx * dx + dy * dy + dz * dz;
				if (distance < bestDistance)
				{
					bestDistance = distance;
					bestGradient = Vector3(
						difference.getX() / (lobe.radius.getX() * lobe.radius.getX()),
						difference.getY() / (lobe.radius.getY() * lobe.radius.getY()),
						difference.getZ() / (lobe.radius.getZ() * lobe.radius.getZ())
					);
				}
			}
			if (Utilities::vectorLengthSquared(bestGradient) > 1e-12)
				return (Utilities::normalize(bestGradient));
		}
	}
	// Layer clouds have no analytic lobes. Their low-frequency density gradient
	// is still a stable orientation guide, while convective clouds intentionally
	// use the smoother lobe gradient above so NFOR does not preserve path noise
	// as false micro-geometry.
	const double epsilon = std::max(1e-3, this->_parameters.featureScale * 0.055);
	const Vector3 densityGradient(
		this->densityAt(position + Vector3(epsilon, 0.0, 0.0)) - this->densityAt(position - Vector3(epsilon, 0.0, 0.0)),
		this->densityAt(position + Vector3(0.0, epsilon, 0.0)) - this->densityAt(position - Vector3(0.0, epsilon, 0.0)),
		this->densityAt(position + Vector3(0.0, 0.0, epsilon)) - this->densityAt(position - Vector3(0.0, 0.0, epsilon))
	);
	if (Utilities::vectorLengthSquared(densityGradient) > 1e-12)
		return (Utilities::normalize(densityGradient * -1.0));
	return (Vector3(0.0, 1.0, 0.0));
}

bool CloudVolume::hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const
{
	const bool samplePrimaryFeatures = Sampler::isFeatureSampling() && Sampler::currentBounce() == 0;
	double featureDensity = 0.0;
	if (samplePrimaryFeatures)
	{
		if (!this->stableFeatureCollision(ray, t_min, t_max, hitRecord.t0, featureDensity))
			return (false);
	}
	else if (!this->sampleCollision(ray, t_min, t_max, hitRecord.t0))
		return (false);
	hitRecord.position = ray.pointAtRay(hitRecord.t0);
	hitRecord.t1 = hitRecord.t0;
	// A cloud is a participating medium, not a surface. Feeding a density
	// gradient to a surface denoiser makes perfectly deterministic iso-density
	// contours look like geometric edges and embosses them into the result.
	// Stable entry depth already protects the silhouette; keep the normal guide
	// constant so interior radiance is filtered as a volume signal. A scalar
	// density guide below can still distinguish thin and thick structures
	// without inventing a surface orientation.
	hitRecord.normal = Vector3(0.0, 1.0, 0.0);
	hitRecord.geometricNormal = hitRecord.normal;
	hitRecord.frontFace = true;
	// Preserve the deterministic density encountered at the optical-depth
	// feature crossing. NFOR uses it to avoid blending a thin wisp into an
	// adjacent optically thick billow; cloud albedo is already carried by the
	// material and is almost constant across the medium.
	hitRecord.u = samplePrimaryFeatures ? std::clamp(featureDensity, 0.0, 1.0) : 0.0;
	hitRecord.material = this->_phaseFunction.get();
	return (true);
}

bool CloudVolume::hitAny(Ray& ray, double t_min, double t_max) const
{
	double hitT;
	return (this->sampleCollision(ray, t_min, t_max, hitT));
}

Color CloudVolume::shadowTransmittance(Ray& ray, double t_min, double t_max) const
{
	double entryT;
	double exitT;
	if (!this->boundsInterval(ray, t_min, t_max, entryT, exitT))
		return (Color(1.0, 1.0, 1.0));
	entryT = std::max(entryT, 0.0);
	if (entryT >= exitT)
		return (Color(1.0, 1.0, 1.0));
	const double rayLength = Utilities::vectorLength(ray.getDirection());
	if (!std::isfinite(rayLength) || rayLength <= 0.0 || this->_majorantSceneUnits <= 0.0)
		return (Color(1.0, 1.0, 1.0));

	if (Sampler::isReferenceVolumeTransport())
	{
		// Ratio tracking uses the same conservative piecewise majorant as free
		// flight. No quadrature or opaque cutoff can hide thin features here.
		double t = entryT;
		double transmittance = 1.0;
		const double epsilon = std::max(1e-10, _parameters.featureScale * 1e-7 / rayLength);
		for (std::uint32_t event = 0; t < exitT; event++)
		{
			double majorant, cellExit;
			lobeGridCell(ray, t, exitT, epsilon, majorant, cellExit);
			if (majorant <= 0.0)
			{
				t = cellExit + epsilon;
				continue;
			}
			const std::uint32_t hash = mixBits(_samplingStream ^ 0x718bc391u
				^ mixBits(event) ^ mixBits(Sampler::currentBounce()));
			const std::uint32_t dimension = CLOUD_TRACKING_DIMENSION + ((hash & 0x01ffffffu) << 5u);
			t += -std::log(std::max(1e-12, 1.0 - Sampler::sample1D(dimension)))
				/ (_majorantSceneUnits * rayLength * majorant);
			if (t >= cellExit)
			{
				t = cellExit + epsilon;
				continue;
			}
			transmittance *= std::max(0.0, 1.0 - densityAt(ray.pointAtRay(t)) / majorant);
			if (transmittance <= 0.0)
				return Color(0.0, 0.0, 0.0);
			// Unbiased roulette keeps optically thick reference shadows bounded in
			// expected cost without deterministically discarding their contribution.
			if (transmittance < 0.05)
			{
				if (Sampler::sample1D(dimension + CLOUD_ACCEPTANCE_OFFSET) >= transmittance / 0.05)
					return Color(0.0, 0.0, 0.0);
				transmittance = 0.05;
			}
		}
		return Color(transmittance, transmittance, transmittance);
	}

	// Only cache complete segments to fixed directional sources. Truncated
	// shadows and stochastic directions retain the direct integrator.
	if (!Sampler::isReferenceVolumeTransport()
		&& (Sampler::isVolumeControlSampling() || Sampler::isDirectionalShadowSampling()))
	{
		double fullEntry, fullExit;
		if (boundsInterval(ray, t_min, std::numeric_limits<double>::max(), fullEntry, fullExit)
			&& t_max + std::max(1e-8, std::fabs(fullExit) * 1e-10) >= fullExit)
		{
			double opticalDepth;
			if (directionalOpticalDepth(ray.pointAtRay(entryT), ray.getDirection(), opticalDepth))
			{
				opticalDepth *= std::pow(this->_parameters.multipleScatteringFalloff,
					static_cast<double>(Sampler::currentBounce()));
				const double transmittance = std::exp(-opticalDepth);
				return Color(transmittance, transmittance, transmittance);
			}
		}
	}

	const double interval = exitT - entryT;
	const double detailStepScale = std::clamp(
		0.18 / std::pow(2.0, static_cast<double>(this->_parameters.detailOctaves - 1)),
		0.025,
		0.18
	);
	const double targetStep = std::max(
		this->_parameters.featureScale * detailStepScale / rayLength,
		interval / static_cast<double>(this->_parameters.maxTrackingSteps)
	);
	const int minimumSteps = std::min(8, this->_parameters.maxTrackingSteps);
	const int stepCount = std::clamp(
		static_cast<int>(std::ceil(interval / targetStep)),
		minimumSteps,
		this->_parameters.maxTrackingSteps
	);
	const double stepT = interval / static_cast<double>(stepCount);
	const double stepDistance = stepT * rayLength;
	const double depthScale = Sampler::isReferenceVolumeTransport() ? 1.0 : std::pow(
		this->_parameters.multipleScatteringFalloff,
		static_cast<double>(Sampler::currentBounce())
	);
	const double boundaryEpsilonT = std::max(
		1e-10,
		this->_parameters.featureScale * 1e-7 / rayLength
	);
	double opticalDepth = 0.0;

	for (int step = 0; step < stepCount; )
	{
		const double sampleT = entryT + (static_cast<double>(step) + 0.5) * stepT;
		if (!this->_lobeGrid.empty())
		{
			double densityMajorant;
			double cellExitT;
			this->lobeGridCell(ray, sampleT, exitT, boundaryEpsilonT, densityMajorant, cellExitT);
			if (densityMajorant <= 0.0)
			{
				// Preserve the original global midpoint rule: jump directly to the
				// first midpoint at or beyond this conservative empty-cell boundary.
				// This avoids procedural density evaluation without changing samples
				// in occupied cells or the resulting integral.
				const int firstStepAfterCell = std::clamp(
					static_cast<int>(std::ceil((cellExitT - entryT) / stepT - 0.5)),
					step + 1,
					stepCount
				);
				step = firstStepAfterCell;
				continue;
			}
		}
		opticalDepth += this->_majorantSceneUnits * depthScale
			* this->densityAt(ray.pointAtRay(sampleT)) * stepDistance;
		if (opticalDepth >= 20.0)
			return (Color(0.0, 0.0, 0.0));
		step++;
	}
	const double transmittance = std::exp(-opticalDepth);
	return (Color(transmittance, transmittance, transmittance));
}

bool CloudVolume::createBoundingBox(AABB& outputBoundingBox) const
{
	outputBoundingBox = AABB(this->_minimum, this->_maximum);
	return (true);
}

const CloudParameters& CloudVolume::getParameters(void) const
{
	return (this->_parameters);
}

Color CloudVolume::volumeAlbedo(void) const
{
	return (this->_parameters.albedo);
}

double CloudVolume::volumeFeatureScale(void) const
{
	return (this->_parameters.featureScale / this->_parameters.primaryDetail);
}

double CloudVolume::multipleScatteringFalloff(void) const
{
	return (this->_parameters.multipleScatteringFalloff);
}

double CloudVolume::multipleScatteringCompensation(void) const
{
	return (this->_parameters.multipleScatteringCompensation);
}
