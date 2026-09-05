#include "Hittables/SparseGridVolume.hpp"
#include "AABB.hpp"
#include "Defaults.hpp"
#include "Materials/HenyeyGreenstein.hpp"
#include "Sampler.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace
{
	constexpr double VOLUME_EPSILON = 1e-12;
	constexpr double UNIT_BELOW_ONE = 0x1.fffffffffffffp-1;
	constexpr double DEGREES_TO_RADIANS = 3.14159265358979323846 / 180.0;
	constexpr double FEATURE_OPTICAL_DEPTH = 0.4307829160924542; // -log(0.65)
	constexpr std::uint32_t TRACKING_DIMENSION = 0x5a000000u;
	constexpr std::uint32_t TRANSMITTANCE_DIMENSION = 0x6b000000u;
	constexpr std::uint32_t TRACKING_ACCEPTANCE_OFFSET =
		Sampler::DIM_VOLUME_ACCEPTANCE - Sampler::DIM_VOLUME_DISTANCE;
	constexpr double GAUSS_POINT_LOW = 0.21132486540518711775;
	constexpr double GAUSS_POINT_HIGH = 0.78867513459481288225;
	constexpr std::size_t MAX_DIRECTIONAL_CACHE_POINTS = 16u * 1024u * 1024u;
	constexpr std::size_t DENSITY_REMAP_INTERVALS = 65536u;
	static_assert(std::numeric_limits<float>::is_iec559);

	float conservativeDensityDown(float value)
	{
		if (value <= 0.0f)
			return (0.0f);
		return (std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) - 1u));
	}

	float conservativeDensityUp(float value)
	{
		if (value <= 0.0f)
			return (0.0f);
		return (std::bit_cast<float>(std::bit_cast<std::uint32_t>(value) + 1u));
	}

	std::uint32_t mixBits(std::uint32_t value)
	{
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		value ^= value >> 16u;
		return (value);
	}

	bool finiteVector(const Vector3& value)
	{
		return (std::isfinite(value.getX())
			&& std::isfinite(value.getY())
			&& std::isfinite(value.getZ()));
	}

	void requirePositive(double value, const std::string& label)
	{
		if (!std::isfinite(value) || value <= 0.0)
			throw std::invalid_argument("Grid volume " + label + " must be finite and positive.");
	}

	void requireUnitInterval(double value, const std::string& label)
	{
		if (!std::isfinite(value) || value < 0.0 || value > 1.0)
			throw std::invalid_argument("Grid volume " + label + " must be between zero and one.");
	}

	void requireAnisotropy(double value, const std::string& label)
	{
		if (!std::isfinite(value) || value < -0.99 || value > 0.99)
			throw std::invalid_argument("Grid volume " + label + " must be between -0.99 and 0.99.");
	}

	bool sameDirection(const Vector3& left, const Vector3& right)
	{
		return (Utilities::vectorLengthSquared(left - right) <= 1e-20);
	}

	std::uint32_t nextSamplingStream(void)
	{
		static std::atomic<std::uint32_t> nextStream(1u);
		return (nextStream.fetch_add(1u, std::memory_order_relaxed));
	}
}

SparseGridVolume::SparseGridVolume(const GridVolumeParameters& parameters)
	: _parameters(parameters), _grid(SparseVolumeGrid::load(parameters.fileName))
{
	this->_samplingStream = nextSamplingStream();
	if (parameters.fileName.empty())
		throw std::invalid_argument("Grid volume requires a file.");
	if (!finiteVector(parameters.position) || !finiteVector(parameters.rotationDegrees)
		|| !finiteVector(parameters.size))
		throw std::invalid_argument("Grid volume position, rotation, and size must be finite.");
	requirePositive(parameters.extinction, "extinction");
	if (!std::isfinite(parameters.densityThreshold)
		|| parameters.densityThreshold < 0.0 || parameters.densityThreshold >= 1.0)
		throw std::invalid_argument("Grid volume density threshold must be finite and in [0, 1).");
	if (!std::isfinite(parameters.densityGamma)
		|| parameters.densityGamma < 0.05 || parameters.densityGamma > 20.0)
		throw std::invalid_argument("Grid volume density gamma must be between 0.05 and 20.");
	requirePositive(parameters.metersPerUnit, "meters per unit");
	requireAnisotropy(parameters.anisotropy, "anisotropy");
	requireAnisotropy(parameters.backscatter, "backscatter");
	requireUnitInterval(parameters.forwardWeight, "forward weight");
	if (!std::isfinite(parameters.dropletSizeMicrons)
		|| (parameters.dropletSizeMicrons != 0.0
			&& (parameters.dropletSizeMicrons < 5.0 || parameters.dropletSizeMicrons > 50.0)))
		throw std::invalid_argument("Grid volume droplet size must be zero or between 5 and 50 microns.");
	if (!std::isfinite(parameters.multipleScatteringFalloff)
		|| parameters.multipleScatteringFalloff <= 0.0
		|| parameters.multipleScatteringFalloff > 1.0)
		throw std::invalid_argument("Grid volume multiple scattering falloff must be greater than zero and at most one.");
	if (!std::isfinite(parameters.multipleScatteringCompensation)
		|| parameters.multipleScatteringCompensation < 0.0
		|| parameters.multipleScatteringCompensation > 2.0)
		throw std::invalid_argument("Grid volume multiple scattering compensation must be between zero and two.");
	if (parameters.shadowSamplesPerBrick < 1 || parameters.shadowSamplesPerBrick > 32)
		throw std::invalid_argument("Grid volume shadow samples per brick must be between 1 and 32.");
	if (!std::isfinite(parameters.primaryDetail)
		|| parameters.primaryDetail < 0.25 || parameters.primaryDetail > 16.0)
		throw std::invalid_argument("Grid volume primary detail must be between 0.25 and 16.");
	if (!std::isfinite(parameters.albedo.getRed()) || !std::isfinite(parameters.albedo.getGreen())
		|| !std::isfinite(parameters.albedo.getBlue())
		|| parameters.albedo.getRed() < 0.0 || parameters.albedo.getRed() > 1.0
		|| parameters.albedo.getGreen() < 0.0 || parameters.albedo.getGreen() > 1.0
		|| parameters.albedo.getBlue() < 0.0 || parameters.albedo.getBlue() > 1.0)
		throw std::invalid_argument("Grid volume albedo channels must be between zero and one.");
	if (this->_grid.maximumDensity() <= 0.0f)
		throw std::invalid_argument("Grid volume contains no positive density.");
	this->buildDensityRemap();

	const bool nativeSize = parameters.size == Vector3(0.0, 0.0, 0.0);
	if (nativeSize)
	{
		const auto& dimensions = this->_grid.dimensions();
		const auto& voxelSize = this->_grid.voxelSize();
		this->_parameters.size = Vector3(
			static_cast<double>(dimensions[0]) * voxelSize[0] / parameters.metersPerUnit,
			static_cast<double>(dimensions[1]) * voxelSize[1] / parameters.metersPerUnit,
			static_cast<double>(dimensions[2]) * voxelSize[2] / parameters.metersPerUnit
		);
	}
	requirePositive(this->_parameters.size.getX(), "width");
	requirePositive(this->_parameters.size.getY(), "height");
	requirePositive(this->_parameters.size.getZ(), "depth");
	this->_minimum = parameters.position - this->_parameters.size * 0.5;
	this->_maximum = parameters.position + this->_parameters.size * 0.5;
	const double xRadians = parameters.rotationDegrees.getX() * DEGREES_TO_RADIANS;
	const double yRadians = parameters.rotationDegrees.getY() * DEGREES_TO_RADIANS;
	const double zRadians = parameters.rotationDegrees.getZ() * DEGREES_TO_RADIANS;
	this->_cosX = std::cos(xRadians);
	this->_sinX = std::sin(xRadians);
	this->_cosY = std::cos(yRadians);
	this->_sinY = std::sin(yRadians);
	this->_cosZ = std::cos(zRadians);
	this->_sinZ = std::sin(zRadians);
	const std::array<Vector3, 8> corners = {
		Vector3(this->_minimum.getX(), this->_minimum.getY(), this->_minimum.getZ()),
		Vector3(this->_maximum.getX(), this->_minimum.getY(), this->_minimum.getZ()),
		Vector3(this->_minimum.getX(), this->_maximum.getY(), this->_minimum.getZ()),
		Vector3(this->_maximum.getX(), this->_maximum.getY(), this->_minimum.getZ()),
		Vector3(this->_minimum.getX(), this->_minimum.getY(), this->_maximum.getZ()),
		Vector3(this->_maximum.getX(), this->_minimum.getY(), this->_maximum.getZ()),
		Vector3(this->_minimum.getX(), this->_maximum.getY(), this->_maximum.getZ()),
		Vector3(this->_maximum.getX(), this->_maximum.getY(), this->_maximum.getZ())
	};
	double worldMinimum[3] = {
		std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::infinity(),
		std::numeric_limits<double>::infinity()
	};
	double worldMaximum[3] = {
		-std::numeric_limits<double>::infinity(),
		-std::numeric_limits<double>::infinity(),
		-std::numeric_limits<double>::infinity()
	};
	for (const Vector3& corner : corners)
	{
		const Vector3 worldCorner = this->localToWorldPoint(corner);
		for (int axis = 0; axis < 3; axis++)
		{
			worldMinimum[axis] = std::min(worldMinimum[axis], worldCorner[axis]);
			worldMaximum[axis] = std::max(worldMaximum[axis], worldCorner[axis]);
		}
	}
	this->_boundingBox = AABB(
		Vector3(worldMinimum[0], worldMinimum[1], worldMinimum[2]),
		Vector3(worldMaximum[0], worldMaximum[1], worldMaximum[2])
	);
	this->_extinctionSceneUnits = parameters.extinction * parameters.metersPerUnit;
	const auto& dimensions = this->_grid.dimensions();
	this->_featureScale = std::min({
		this->_parameters.size.getX() / static_cast<double>(dimensions[0]),
		this->_parameters.size.getY() / static_cast<double>(dimensions[1]),
		this->_parameters.size.getZ() / static_cast<double>(dimensions[2])
	}) * SparseVolumeGrid::BRICK_EDGE;
	requirePositive(this->_featureScale, "derived feature scale");

	auto phase = std::make_shared<HenyeyGreenstein>(
		parameters.albedo,
		parameters.anisotropy,
		parameters.backscatter,
		parameters.forwardWeight
	);
	if (parameters.dropletSizeMicrons > 0.0)
		phase->setDropletPhase(parameters.dropletSizeMicrons);
	phase->setDepthAnisotropyReduction(parameters.multipleScatteringFalloff < 1.0);
	this->_phaseFunction = phase;
}

Material* SparseGridVolume::getMaterial(void) const
{
	return (this->_phaseFunction.get());
}

Vector3 SparseGridVolume::rotate(const Vector3& vector) const
{
	double x = vector.getX();
	double y = vector.getY();
	double z = vector.getZ();
	double nextY = y * this->_cosX - z * this->_sinX;
	double nextZ = y * this->_sinX + z * this->_cosX;
	y = nextY;
	z = nextZ;
	double nextX = x * this->_cosY + z * this->_sinY;
	nextZ = -x * this->_sinY + z * this->_cosY;
	x = nextX;
	z = nextZ;
	nextX = x * this->_cosZ - y * this->_sinZ;
	nextY = x * this->_sinZ + y * this->_cosZ;
	return (Vector3(nextX, nextY, z));
}

Vector3 SparseGridVolume::inverseRotate(const Vector3& vector) const
{
	double x = vector.getX();
	double y = vector.getY();
	double z = vector.getZ();
	double nextX = x * this->_cosZ + y * this->_sinZ;
	double nextY = -x * this->_sinZ + y * this->_cosZ;
	x = nextX;
	y = nextY;
	nextX = x * this->_cosY - z * this->_sinY;
	double nextZ = x * this->_sinY + z * this->_cosY;
	x = nextX;
	z = nextZ;
	nextY = y * this->_cosX + z * this->_sinX;
	nextZ = -y * this->_sinX + z * this->_cosX;
	return (Vector3(x, nextY, nextZ));
}

Vector3 SparseGridVolume::worldToLocalPoint(const Vector3& position) const
{
	return (this->_parameters.position
		+ this->inverseRotate(position - this->_parameters.position));
}

Vector3 SparseGridVolume::localToWorldPoint(const Vector3& position) const
{
	return (this->_parameters.position
		+ this->rotate(position - this->_parameters.position));
}

Ray SparseGridVolume::worldToLocalRay(const Ray& ray) const
{
	return (Ray(
		this->worldToLocalPoint(ray.getOrigin()),
		this->inverseRotate(ray.getDirection())
	));
}

Vector3 SparseGridVolume::normalizedLocalPosition(const Vector3& position) const
{
	return (Vector3(
		(position.getX() - this->_minimum.getX()) / this->_parameters.size.getX(),
		(position.getY() - this->_minimum.getY()) / this->_parameters.size.getY(),
		(position.getZ() - this->_minimum.getZ()) / this->_parameters.size.getZ()
	));
}

Vector3 SparseGridVolume::normalizedPosition(const Vector3& position) const
{
	return (this->normalizedLocalPosition(this->worldToLocalPoint(position)));
}

double SparseGridVolume::densityAtLocal(const Vector3& position) const
{
	return (this->remapDensity(static_cast<double>(this->_grid.sampleUnchecked(
		this->normalizedLocalPosition(position)
	))));
}

double SparseGridVolume::densityAt(const Vector3& position) const
{
	return (this->remapDensity(static_cast<double>(
		this->_grid.sample(this->normalizedPosition(position))
	)));
}

void SparseGridVolume::buildDensityRemap(void)
{
	if (this->_parameters.densityThreshold == 0.0
		&& this->_parameters.densityGamma == 1.0)
		return;
	this->_densityRemap.resize(DENSITY_REMAP_INTERVALS + 1u);
	const double maximumDensity = static_cast<double>(this->_grid.maximumDensity());
	this->_densityRemapScale = static_cast<double>(DENSITY_REMAP_INTERVALS)
		/ maximumDensity;
	this->_densityRemapThreshold = maximumDensity
		* this->_parameters.densityThreshold;
	const double inverseRange = 1.0 / (1.0 - this->_parameters.densityThreshold);
	for (std::size_t index = 0; index <= DENSITY_REMAP_INTERVALS; index++)
	{
		const double normalized = static_cast<double>(index)
			/ static_cast<double>(DENSITY_REMAP_INTERVALS);
		const double thresholded = std::clamp(
			(normalized - this->_parameters.densityThreshold) * inverseRange,
			0.0,
			1.0
		);
		this->_densityRemap[index] = static_cast<float>(
			maximumDensity * std::pow(thresholded, this->_parameters.densityGamma)
		);
	}
}

double SparseGridVolume::remapDensity(double density) const
{
	if (this->_densityRemap.empty())
		return (density);
	if (density <= this->_densityRemapThreshold)
		return (0.0);
	const double coordinate = std::clamp(
		density * this->_densityRemapScale,
		0.0,
		static_cast<double>(DENSITY_REMAP_INTERVALS)
	);
	const std::size_t low = static_cast<std::size_t>(coordinate);
	const std::size_t high = std::min(low + 1u, DENSITY_REMAP_INTERVALS);
	const double fraction = coordinate - static_cast<double>(low);
	return (static_cast<double>(this->_densityRemap[low]) * (1.0 - fraction)
		+ static_cast<double>(this->_densityRemap[high]) * fraction);
}

bool SparseGridVolume::boundsInterval(
	const Ray& ray,
	double t_min,
	double t_max,
	double& entryT,
	double& exitT
) const
{
	const Ray localRay = this->worldToLocalRay(ray);
	entryT = t_min;
	exitT = t_max;
	for (int axis = 0; axis < 3; axis++)
	{
		const double direction = localRay.getDirection()[axis];
		const double origin = localRay.getOrigin()[axis];
		if (std::fabs(direction) <= VOLUME_EPSILON)
		{
			if (origin < this->_minimum[axis] || origin > this->_maximum[axis])
				return (false);
			continue;
		}
		double axisEntry = (this->_minimum[axis] - origin) / direction;
		double axisExit = (this->_maximum[axis] - origin) / direction;
		if (axisEntry > axisExit)
			std::swap(axisEntry, axisExit);
		entryT = std::max(entryT, axisEntry);
		exitT = std::min(exitT, axisExit);
		if (entryT >= exitT)
			return (false);
	}
	return (true);
}

bool SparseGridVolume::integrationInterval(
	const Ray& ray,
	double t_min,
	double t_max,
	double& entryT,
	double& exitT
) const
{
	return (this->boundsInterval(ray, t_min, t_max, entryT, exitT));
}

std::shared_ptr<const SparseGridVolume::DirectionalTransmittanceCache>
SparseGridVolume::buildDirectionalCache(const Vector3& direction) const
{
	auto cache = std::make_shared<DirectionalTransmittanceCache>();
	cache->direction = direction;
	const Vector3 localDirection = this->inverseRotate(direction);
	const auto& gridDimensions = this->_grid.dimensions();
	std::uint32_t subdivisions = static_cast<std::uint32_t>(std::clamp(
		static_cast<int>(std::ceil(std::sqrt(this->_parameters.primaryDetail))),
		1,
		3
	));
	std::array<std::uint32_t, 3> brickCounts = {0u, 0u, 0u};
	for (int axis = 0; axis < 3; axis++)
	{
		brickCounts[axis] = (gridDimensions[axis] - 1u)
			/ SparseVolumeGrid::BRICK_EDGE + 1u;
	}
	auto setSubdivisionCells = [&cache, &brickCounts](std::uint32_t value)
	{
		for (int axis = 0; axis < 3; axis++)
			cache->cells[axis] = std::max(1u, brickCounts[axis] * value);
	};
	auto pointCountEstimate = [&cache]()
	{
		return ((static_cast<long double>(cache->cells[0]) + 1.0L)
			* (static_cast<long double>(cache->cells[1]) + 1.0L)
			* (static_cast<long double>(cache->cells[2]) + 1.0L));
	};
	setSubdivisionCells(subdivisions);
	while (subdivisions > 1u
		&& pointCountEstimate() > static_cast<long double>(MAX_DIRECTIONAL_CACHE_POINTS))
	{
		subdivisions--;
		setSubdivisionCells(subdivisions);
	}
	if (pointCountEstimate() > static_cast<long double>(MAX_DIRECTIONAL_CACHE_POINTS))
	{
		const long double scale = std::cbrt(
			static_cast<long double>(MAX_DIRECTIONAL_CACHE_POINTS)
			/ pointCountEstimate()
		) * 0.995L;
		for (int axis = 0; axis < 3; axis++)
		{
			cache->cells[axis] = std::max<std::uint32_t>(
				1u,
				static_cast<std::uint32_t>(std::floor(
					static_cast<long double>(cache->cells[axis]) * scale
				))
			);
		}
		while (pointCountEstimate() > static_cast<long double>(MAX_DIRECTIONAL_CACHE_POINTS))
		{
			const int largestAxis = cache->cells[0] >= cache->cells[1]
				? (cache->cells[0] >= cache->cells[2] ? 0 : 2)
				: (cache->cells[1] >= cache->cells[2] ? 1 : 2);
			if (cache->cells[largestAxis] <= 1u)
				break;
			cache->cells[largestAxis]--;
		}
	}
	for (int axis = 0; axis < 3; axis++)
		cache->points[axis] = cache->cells[axis] + 1u;
	const std::size_t pointCount = static_cast<std::size_t>(cache->points[0])
		* static_cast<std::size_t>(cache->points[1])
		* static_cast<std::size_t>(cache->points[2]);
	cache->opticalDepth.assign(pointCount, 0.0f);

	auto index = [&cache](std::uint32_t x, std::uint32_t y, std::uint32_t z)
	{
		return ((static_cast<std::size_t>(z) * cache->points[1] + y)
			* cache->points[0] + x);
	};
	const double cellSize[3] = {
		this->_parameters.size.getX() / static_cast<double>(cache->cells[0]),
		this->_parameters.size.getY() / static_cast<double>(cache->cells[1]),
		this->_parameters.size.getZ() / static_cast<double>(cache->cells[2])
	};
	int dominantAxis = 0;
	double dominantRate = 0.0;
	for (int axis = 0; axis < 3; axis++)
	{
		const double rate = std::fabs(localDirection[axis]) / cellSize[axis];
		if (rate > dominantRate)
		{
			dominantRate = rate;
			dominantAxis = axis;
		}
	}
	if (dominantRate <= VOLUME_EPSILON)
		return (cache);
	const int firstSecondaryAxis = (dominantAxis + 1) % 3;
	const int secondSecondaryAxis = (dominantAxis + 2) % 3;
	const int directionSign = localDirection[dominantAxis] >= 0.0 ? 1 : -1;
	const double dominantStepDistance = cellSize[dominantAxis]
		/ std::fabs(localDirection[dominantAxis]);

	auto positionFromCoordinates = [this, &cache](const std::array<std::uint32_t, 3>& coordinate)
	{
		return (Vector3(
			this->_minimum.getX() + this->_parameters.size.getX()
				* static_cast<double>(coordinate[0]) / cache->cells[0],
			this->_minimum.getY() + this->_parameters.size.getY()
				* static_cast<double>(coordinate[1]) / cache->cells[1],
			this->_minimum.getZ() + this->_parameters.size.getZ()
				* static_cast<double>(coordinate[2]) / cache->cells[2]
		));
	};
	auto previousSliceDepth = [this, &cache, &index, dominantAxis,
		firstSecondaryAxis, secondSecondaryAxis](
			const Vector3& position,
			std::uint32_t previousSlice
		)
	{
		double coordinates[3] = {0.0, 0.0, 0.0};
		for (int axis = 0; axis < 3; axis++)
		{
			coordinates[axis] = std::clamp(
				(position[axis] - this->_minimum[axis]) / this->_parameters.size[axis]
					* static_cast<double>(cache->cells[axis]),
				0.0,
				static_cast<double>(cache->cells[axis])
			);
		}
		const int axis0 = firstSecondaryAxis;
		const int axis1 = secondSecondaryAxis;
		const std::uint32_t low0 = static_cast<std::uint32_t>(std::floor(coordinates[axis0]));
		const std::uint32_t low1 = static_cast<std::uint32_t>(std::floor(coordinates[axis1]));
		const std::uint32_t high0 = std::min(low0 + 1u, cache->cells[axis0]);
		const std::uint32_t high1 = std::min(low1 + 1u, cache->cells[axis1]);
		const double fraction0 = coordinates[axis0] - static_cast<double>(low0);
		const double fraction1 = coordinates[axis1] - static_cast<double>(low1);
		auto sample = [&](std::uint32_t coordinate0, std::uint32_t coordinate1)
		{
			std::array<std::uint32_t, 3> point = {0u, 0u, 0u};
			point[dominantAxis] = previousSlice;
			point[axis0] = coordinate0;
			point[axis1] = coordinate1;
			return (static_cast<double>(cache->opticalDepth[index(
				point[0], point[1], point[2]
			)]));
		};
		const double low = sample(low0, low1) * (1.0 - fraction0)
			+ sample(high0, low1) * fraction0;
		const double high = sample(low0, high1) * (1.0 - fraction0)
			+ sample(high0, high1) * fraction0;
		return (low * (1.0 - fraction1) + high * fraction1);
	};

	for (std::uint32_t sliceOffset = 0; sliceOffset <= cache->cells[dominantAxis]; sliceOffset++)
	{
		const std::uint32_t slice = directionSign > 0
			? cache->cells[dominantAxis] - sliceOffset
			: sliceOffset;
		for (std::uint32_t secondary0 = 0;
			secondary0 <= cache->cells[firstSecondaryAxis]; secondary0++)
		{
			for (std::uint32_t secondary1 = 0;
				secondary1 <= cache->cells[secondSecondaryAxis]; secondary1++)
			{
				std::array<std::uint32_t, 3> coordinate = {0u, 0u, 0u};
				coordinate[dominantAxis] = slice;
				coordinate[firstSecondaryAxis] = secondary0;
				coordinate[secondSecondaryAxis] = secondary1;
				if (sliceOffset == 0)
					continue;
				const Vector3 position = positionFromCoordinates(coordinate);
				double distanceToBoundary = std::numeric_limits<double>::max();
				for (int axis = 0; axis < 3; axis++)
				{
					if (localDirection[axis] > VOLUME_EPSILON)
						distanceToBoundary = std::min(
							distanceToBoundary,
							(this->_maximum[axis] - position[axis]) / localDirection[axis]
						);
					else if (localDirection[axis] < -VOLUME_EPSILON)
						distanceToBoundary = std::min(
							distanceToBoundary,
							(this->_minimum[axis] - position[axis]) / localDirection[axis]
						);
				}
				const double segmentDistance = std::clamp(
					distanceToBoundary,
					0.0,
					dominantStepDistance
				);
				if (segmentDistance <= VOLUME_EPSILON)
					continue;
				const Vector3 lowSamplePosition = position
					+ localDirection * (segmentDistance * GAUSS_POINT_LOW);
				const Vector3 highSamplePosition = position
					+ localDirection * (segmentDistance * GAUSS_POINT_HIGH);
				double opticalDepth = this->_extinctionSceneUnits * segmentDistance * 0.5
					* (this->densityAtLocal(lowSamplePosition)
						+ this->densityAtLocal(highSamplePosition));
				if (distanceToBoundary > dominantStepDistance + 1e-9)
				{
					const std::uint32_t previousSlice = static_cast<std::uint32_t>(
						static_cast<int>(slice) + directionSign
					);
					opticalDepth += previousSliceDepth(
						position + localDirection * dominantStepDistance,
						previousSlice
					);
				}
				cache->opticalDepth[index(
					coordinate[0], coordinate[1], coordinate[2]
				)] = static_cast<float>(std::max(0.0, opticalDepth));
			}
		}
	}
	return (cache);
}

const SparseGridVolume::DirectionalTransmittanceCache*
SparseGridVolume::directionalCache(const Vector3& direction) const
{
	auto snapshot = std::atomic_load_explicit(
		&this->_directionalCacheSnapshot,
		std::memory_order_acquire
	);
	if (snapshot)
	{
		for (const auto& cache : *snapshot)
		{
			if (sameDirection(cache->direction, direction))
				return (cache.get());
		}
	}
	std::lock_guard<std::mutex> lock(this->_directionalCacheMutex);
	snapshot = std::atomic_load_explicit(
		&this->_directionalCacheSnapshot,
		std::memory_order_acquire
	);
	if (snapshot)
	{
		for (const auto& cache : *snapshot)
		{
			if (sameDirection(cache->direction, direction))
				return (cache.get());
		}
	}
	const auto cache = this->buildDirectionalCache(direction);
	auto updated = std::make_shared<DirectionalCacheList>();
	if (snapshot)
		*updated = *snapshot;
	updated->push_back(cache);
	std::shared_ptr<const DirectionalCacheList> immutableUpdated = updated;
	std::atomic_store_explicit(
		&this->_directionalCacheSnapshot,
		immutableUpdated,
		std::memory_order_release
	);
	return (cache.get());
}

double SparseGridVolume::cachedDirectionalOpticalDepth(
	const DirectionalTransmittanceCache& cache,
	const Vector3& position
) const
{
	const Vector3 normalized = this->normalizedPosition(position);
	double coordinate[3] = {0.0, 0.0, 0.0};
	std::uint32_t low[3] = {0u, 0u, 0u};
	std::uint32_t high[3] = {0u, 0u, 0u};
	double fraction[3] = {0.0, 0.0, 0.0};
	for (int axis = 0; axis < 3; axis++)
	{
		coordinate[axis] = std::clamp(
			normalized[axis] * static_cast<double>(cache.cells[axis]),
			0.0,
			static_cast<double>(cache.cells[axis])
		);
		low[axis] = static_cast<std::uint32_t>(std::floor(coordinate[axis]));
		high[axis] = std::min(low[axis] + 1u, cache.cells[axis]);
		fraction[axis] = coordinate[axis] - static_cast<double>(low[axis]);
	}
	auto sample = [&cache](std::uint32_t x, std::uint32_t y, std::uint32_t z)
	{
		const std::size_t index = (static_cast<std::size_t>(z) * cache.points[1] + y)
			* cache.points[0] + x;
		return (static_cast<double>(cache.opticalDepth[index]));
	};
	const double x00 = sample(low[0], low[1], low[2]) * (1.0 - fraction[0])
		+ sample(high[0], low[1], low[2]) * fraction[0];
	const double x10 = sample(low[0], high[1], low[2]) * (1.0 - fraction[0])
		+ sample(high[0], high[1], low[2]) * fraction[0];
	const double x01 = sample(low[0], low[1], high[2]) * (1.0 - fraction[0])
		+ sample(high[0], low[1], high[2]) * fraction[0];
	const double x11 = sample(low[0], high[1], high[2]) * (1.0 - fraction[0])
		+ sample(high[0], high[1], high[2]) * fraction[0];
	const double y0 = x00 * (1.0 - fraction[1]) + x10 * fraction[1];
	const double y1 = x01 * (1.0 - fraction[1]) + x11 * fraction[1];
	return (std::max(0.0, y0 * (1.0 - fraction[2]) + y1 * fraction[2]));
}

bool SparseGridVolume::segmentAtLocal(
	BrickTraversal& traversal,
	double t,
	double volumeExitT,
	BrickSegment& segment
) const
{
	const auto& dimensions = this->_grid.dimensions();
	auto coordinate = [&traversal](int axis) -> std::int32_t&
	{
		if (axis == 0)
			return (traversal.segment.x);
		if (axis == 1)
			return (traversal.segment.y);
		return (traversal.segment.z);
	};
	auto updateBoundary = [this, &traversal, &dimensions, &coordinate](int axis)
	{
		const double direction = traversal.localRay.getDirection()[axis];
		if (std::fabs(direction) <= VOLUME_EPSILON)
		{
			traversal.step[axis] = 0;
			traversal.boundaryT[axis] = std::numeric_limits<double>::infinity();
			return;
		}
		traversal.step[axis] = direction > 0.0 ? 1 : -1;
		const double scaledBoundary = direction > 0.0
			? std::min<double>((coordinate(axis) + 1) * SparseVolumeGrid::BRICK_EDGE, dimensions[axis])
			: coordinate(axis) * SparseVolumeGrid::BRICK_EDGE;
		const double worldBoundary = this->_minimum[axis]
			+ this->_parameters.size[axis] * scaledBoundary / static_cast<double>(dimensions[axis]);
		traversal.boundaryT[axis] = (worldBoundary
			- traversal.localRay.getOrigin()[axis]) / direction;
	};

	if (!traversal.initialized)
	{
		const Vector3 normalized = this->normalizedLocalPosition(
			traversal.localRay.pointAtRay(t)
		);
		for (int axis = 0; axis < 3; axis++)
		{
			if (!std::isfinite(normalized[axis])
				|| normalized[axis] < -1e-8 || normalized[axis] > 1.0 + 1e-8)
				return (false);
		}
		const double nx = std::clamp(normalized.getX(), 0.0, UNIT_BELOW_ONE);
		const double ny = std::clamp(normalized.getY(), 0.0, UNIT_BELOW_ONE);
		const double nz = std::clamp(normalized.getZ(), 0.0, UNIT_BELOW_ONE);
		traversal.segment.x = static_cast<std::int32_t>(std::floor(
			nx * dimensions[0] / SparseVolumeGrid::BRICK_EDGE
		));
		traversal.segment.y = static_cast<std::int32_t>(std::floor(
			ny * dimensions[1] / SparseVolumeGrid::BRICK_EDGE
		));
		traversal.segment.z = static_cast<std::int32_t>(std::floor(
			nz * dimensions[2] / SparseVolumeGrid::BRICK_EDGE
		));
		for (int axis = 0; axis < 3; axis++)
			updateBoundary(axis);
		traversal.initialized = true;
	}
	else if (t < traversal.segment.exitT)
	{
		segment = traversal.segment;
		return (true);
	}
	else
	{
		if (traversal.segment.exitT >= volumeExitT)
			return (false);
		const double passedBoundaryT = t + 1e-12;
		bool advanced = false;
		for (int axis = 0; axis < 3; axis++)
		{
			while (traversal.step[axis] != 0
				&& traversal.boundaryT[axis] <= passedBoundaryT)
			{
				coordinate(axis) += traversal.step[axis];
				const std::int32_t brickCount = static_cast<std::int32_t>(
					(dimensions[axis] + SparseVolumeGrid::BRICK_EDGE - 1u)
						/ SparseVolumeGrid::BRICK_EDGE
				);
				if (coordinate(axis) < 0 || coordinate(axis) >= brickCount)
					return (false);
				updateBoundary(axis);
				advanced = true;
			}
		}
		if (!advanced)
			return (false);
	}

	this->_grid.interpolationBrickBounds(
		traversal.segment.x,
		traversal.segment.y,
		traversal.segment.z,
		traversal.segment.minimum,
		traversal.segment.maximum
	);
	if (!this->_densityRemap.empty())
	{
		traversal.segment.minimum = conservativeDensityDown(
			static_cast<float>(this->remapDensity(
				static_cast<double>(traversal.segment.minimum)
			))
		);
		traversal.segment.maximum = conservativeDensityUp(
			static_cast<float>(this->remapDensity(
				static_cast<double>(traversal.segment.maximum)
			))
		);
	}
	traversal.segment.exitT = volumeExitT;
	for (int axis = 0; axis < 3; axis++)
	{
		if (traversal.boundaryT[axis] > t + 1e-12)
			traversal.segment.exitT = std::min(
				traversal.segment.exitT,
				traversal.boundaryT[axis]
			);
	}
	segment = traversal.segment;
	return (segment.exitT > t);
}

bool SparseGridVolume::sampleCollision(Ray& ray, double t_min, double t_max, double& hitT) const
{
	double entryT;
	double exitT;
	if (!this->boundsInterval(ray, t_min, t_max, entryT, exitT))
		return (false);
	entryT = std::max(0.0, entryT);
	const double rayLength = Utilities::vectorLength(ray.getDirection());
	if (entryT >= exitT || !std::isfinite(rayLength) || rayLength <= 0.0)
		return (false);
	const double depthScale = Sampler::isReferenceVolumeTransport() ? 1.0 : std::pow(
		this->_parameters.multipleScatteringFalloff,
		static_cast<double>(Sampler::currentBounce())
	);
	const double epsilonT = std::max(1e-10, this->_featureScale * 1e-8 / rayLength);
	BrickTraversal traversal(this->worldToLocalRay(ray));
	double currentT = entryT;
	for (std::uint32_t step = 0; currentT < exitT; step++)
	{
		BrickSegment segment;
		if (!this->segmentAtLocal(traversal, std::min(exitT, currentT + epsilonT), exitT, segment))
			return (false);
		if (segment.maximum <= 0.0f)
		{
			currentT = segment.exitT + epsilonT;
			continue;
		}
		const double rate = this->_extinctionSceneUnits * depthScale
			* static_cast<double>(segment.maximum) * rayLength;
		if (!std::isfinite(rate) || rate <= 0.0)
			return (false);
		const std::uint32_t tupleHash = mixBits(step + 0x9e3779b9u)
			^ mixBits(Sampler::currentBounce() + 0x85ebca6bu)
			^ mixBits(this->_samplingStream + 0xc2b2ae35u);
		// Preserve the sampler's low-five-bit semantic dimension so distance and
		// acceptance retain their progressive sequences while the high bits give
		// every medium and null event an independent stream.
		const std::uint32_t dimension = TRACKING_DIMENSION
			+ Sampler::DIM_VOLUME_DISTANCE
			+ ((tupleHash & 0x01ffffffu) << 5u);
		const double freeFlight = -std::log(std::max(1e-12, 1.0 - Sampler::sample1D(dimension))) / rate;
		const double collisionT = currentT + freeFlight;
		if (collisionT >= segment.exitT)
		{
			currentT = segment.exitT + epsilonT;
			continue;
		}
		currentT = collisionT;
		const double density = this->densityAtLocal(
			traversal.localRay.pointAtRay(currentT)
		);
		if (density > 0.0
			&& Sampler::sample1D(dimension + TRACKING_ACCEPTANCE_OFFSET)
				< density / static_cast<double>(segment.maximum))
		{
			hitT = currentT;
			return (true);
		}
		if (step == std::numeric_limits<std::uint32_t>::max())
			return (false);
	}
	return (false);
}

bool SparseGridVolume::stableFeatureCollision(
	const Ray& ray,
	double t_min,
	double t_max,
	double& hitT,
	double& featureDensity
) const
{
	double entryT;
	double exitT;
	if (!this->boundsInterval(ray, t_min, t_max, entryT, exitT))
		return (false);
	entryT = std::max(0.0, entryT);
	const double rayLength = Utilities::vectorLength(ray.getDirection());
	if (entryT >= exitT || !std::isfinite(rayLength) || rayLength <= 0.0)
		return (false);
	const double epsilonT = std::max(1e-10, this->_featureScale * 1e-8 / rayLength);
	BrickTraversal traversal(this->worldToLocalRay(ray));
	double opticalDepth = 0.0;
	double currentT = entryT;
	while (currentT < exitT)
	{
		BrickSegment segment;
		if (!this->segmentAtLocal(traversal, std::min(exitT, currentT + epsilonT), exitT, segment))
			return (false);
		if (segment.maximum > 0.0f)
		{
			const int samples = this->_parameters.shadowSamplesPerBrick;
			const double stepT = (segment.exitT - currentT) / static_cast<double>(samples);
			for (int sample = 0; sample < samples; sample++)
			{
				const double sampleT = currentT + (sample + 0.5) * stepT;
				const double density = this->densityAtLocal(
					traversal.localRay.pointAtRay(sampleT)
				);
				opticalDepth += this->_extinctionSceneUnits
					* density * stepT * rayLength;
				if (opticalDepth >= FEATURE_OPTICAL_DEPTH)
				{
					hitT = sampleT;
					featureDensity = density;
					return (true);
				}
			}
		}
		currentT = segment.exitT + epsilonT;
	}
	return (false);
}

bool SparseGridVolume::hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const
{
	const bool featureSampling = Sampler::isFeatureSampling() && Sampler::currentBounce() == 0;
	double featureDensity = 0.0;
	if (featureSampling)
	{
		if (!this->stableFeatureCollision(
			ray,
			t_min,
			t_max,
			hitRecord.t0,
			featureDensity
		))
			return (false);
	}
	else if (!this->sampleCollision(ray, t_min, t_max, hitRecord.t0))
		return (false);
	hitRecord.t1 = hitRecord.t0;
	hitRecord.position = ray.pointAtRay(hitRecord.t0);
	hitRecord.normal = Vector3(0.0, 1.0, 0.0);
	hitRecord.geometricNormal = hitRecord.normal;
	hitRecord.frontFace = true;
	// The deterministic camera-ray density is a stable volumetric edge guide.
	// Albedo is already available through the phase material and is nearly
	// constant for clouds, so storing it here discarded the useful signal.
	hitRecord.u = featureSampling ? std::clamp(featureDensity, 0.0, 1.0) : 0.0;
	hitRecord.material = this->_phaseFunction.get();
	return (true);
}

bool SparseGridVolume::hitAny(Ray& ray, double t_min, double t_max) const
{
	double hitT;
	return (this->sampleCollision(ray, t_min, t_max, hitT));
}

Color SparseGridVolume::shadowTransmittance(Ray& ray, double t_min, double t_max) const
{
	double entryT;
	double exitT;
	if (!this->boundsInterval(ray, t_min, t_max, entryT, exitT))
		return (Color(1.0, 1.0, 1.0));
	entryT = std::max(0.0, entryT);
	const double rayLength = Utilities::vectorLength(ray.getDirection());
	if (entryT >= exitT || !std::isfinite(rayLength) || rayLength <= 0.0)
		return (Color(1.0, 1.0, 1.0));
	const double depthScale = Sampler::isReferenceVolumeTransport() ? 1.0 : std::pow(
		this->_parameters.multipleScatteringFalloff,
		static_cast<double>(Sampler::currentBounce())
	);
	const double epsilonT = std::max(1e-10, this->_featureScale * 1e-8 / rayLength);
	if (this->_parameters.directionalCache && !Sampler::isReferenceVolumeTransport() && (
		Sampler::isVolumeControlSampling()
		|| Sampler::isDirectionalShadowSampling()
	))
	{
		double fullEntryT;
		double fullExitT;
		if (this->boundsInterval(ray, t_min, T_MAX, fullEntryT, fullExitT))
		{
			const double exitTolerance = std::max(1e-8, std::fabs(fullExitT) * 1e-10);
			if (t_max + exitTolerance >= fullExitT)
			{
				const Vector3 direction = ray.getDirection() / rayLength;
				const auto cache = this->directionalCache(direction);
				const Vector3 cachePosition = ray.pointAtRay(std::max(0.0, fullEntryT));
				const double opticalDepth = this->cachedDirectionalOpticalDepth(
					*cache,
					cachePosition
				) * depthScale;
				const double transmittance = opticalDepth >= 20.0
					? 0.0
					: std::exp(-opticalDepth);
				return (Color(transmittance, transmittance, transmittance));
			}
		}
	}
	if (Sampler::isActive() && !Sampler::isVolumeControlSampling())
	{
		// Piecewise residual ratio tracking. A ray-adapted constant control is
		// integrated analytically in each brick segment; ratio tracking evaluates
		// only the signed residual. Conservative interpolation bounds keep the
		// estimator unbiased even where the residual extinction is negative.
		double logTransmittance = 0.0;
		BrickTraversal traversal(this->worldToLocalRay(ray));
		double currentT = entryT;
		std::uint32_t event = 0;
		while (currentT < exitT)
		{
			BrickSegment segment;
			if (!this->segmentAtLocal(traversal, std::min(exitT, currentT + epsilonT), exitT, segment))
				break;
			if (segment.maximum <= 0.0f)
			{
				currentT = segment.exitT + epsilonT;
				continue;
			}
			const double segmentStartT = currentT;
			const double segmentLengthT = segment.exitT - segmentStartT;
			const double midpointT = segmentStartT + segmentLengthT * 0.5;
			const double minimumDensity = static_cast<double>(segment.minimum);
			const double maximumDensity = static_cast<double>(segment.maximum);
			const double controlDensity = std::clamp(
				this->densityAtLocal(traversal.localRay.pointAtRay(midpointT)),
				minimumDensity,
				maximumDensity
			);
			const double residualDensityMajorant = std::max(
				controlDensity - minimumDensity,
				maximumDensity - controlDensity
			);
			const double extinctionScale = this->_extinctionSceneUnits * depthScale;
			logTransmittance -= extinctionScale * controlDensity
				* segmentLengthT * rayLength;
			if (residualDensityMajorant <= 1e-12)
			{
				currentT = segment.exitT + epsilonT;
				continue;
			}
			const double residualMajorant = extinctionScale * residualDensityMajorant;
			const double rate = residualMajorant * rayLength;
			if (!std::isfinite(rate) || rate <= 0.0)
				return (Color(1.0, 1.0, 1.0));
			double eventT = segmentStartT;
			while (eventT < segment.exitT)
			{
				const std::uint32_t coordinateHash = mixBits(
					static_cast<std::uint32_t>(segment.x) * 0x9e3779b9u
					^ static_cast<std::uint32_t>(segment.y) * 0x85ebca6bu
					^ static_cast<std::uint32_t>(segment.z) * 0xc2b2ae35u
					^ event
					^ mixBits(this->_samplingStream + 0x27d4eb2du)
				);
				const std::uint32_t dimension = TRANSMITTANCE_DIMENSION
					+ Sampler::DIM_VOLUME_DISTANCE
					+ ((coordinateHash & 0x01ffffffu) << 5u);
				const double distance = -std::log(std::max(
					1e-12,
					1.0 - Sampler::sample1D(dimension)
				)) / rate;
				eventT += distance;
				if (eventT >= segment.exitT)
					break;
				const double residualExtinction = extinctionScale
					* (this->densityAtLocal(traversal.localRay.pointAtRay(eventT))
						- controlDensity);
				const double factor = std::clamp(
					1.0 - residualExtinction / residualMajorant,
					0.0,
					2.0
				);
				if (factor <= 0.0)
					return (Color(0.0, 0.0, 0.0));
				logTransmittance += std::log(factor);
				event++;
				if (event == std::numeric_limits<std::uint32_t>::max())
					break;
			}
			currentT = segment.exitT + epsilonT;
		}
		const double maximumLog = std::log(std::numeric_limits<double>::max());
		const double transmittance = logTransmittance >= maximumLog
			? std::numeric_limits<double>::max()
			: std::exp(logTransmittance);
		return (Color(transmittance, transmittance, transmittance));
	}
	double opticalDepth = 0.0;
	BrickTraversal traversal(this->worldToLocalRay(ray));
	double currentT = entryT;
	while (currentT < exitT)
	{
		BrickSegment segment;
		if (!this->segmentAtLocal(traversal, std::min(exitT, currentT + epsilonT), exitT, segment))
			break;
		if (segment.maximum > 0.0f)
		{
			const int samples = Sampler::isVolumeControlSampling()
				? std::max(1, this->_parameters.shadowSamplesPerBrick / 8)
				: this->_parameters.shadowSamplesPerBrick;
			const double stepT = (segment.exitT - currentT) / static_cast<double>(samples);
			for (int sample = 0; sample < samples; sample++)
			{
				const double sampleT = currentT + (sample + 0.5) * stepT;
				opticalDepth += this->_extinctionSceneUnits * depthScale
					* this->densityAtLocal(traversal.localRay.pointAtRay(sampleT))
					* stepT * rayLength;
				if (opticalDepth >= 20.0)
					return (Color(0.0, 0.0, 0.0));
			}
		}
		currentT = segment.exitT + epsilonT;
	}
	const double transmittance = std::exp(-opticalDepth);
	return (Color(transmittance, transmittance, transmittance));
}

double SparseGridVolume::extinctionAt(const Vector3& position) const
{
	return (this->_extinctionSceneUnits * this->densityAt(position));
}

Color SparseGridVolume::singleScatteringCoefficientAt(
	const Vector3& position,
	const Vector3& incidentDirection,
	const Vector3& scatteredDirection
) const
{
	return singleScatteringWithExtinction(position, incidentDirection, scatteredDirection, extinctionAt(position));
}

Color SparseGridVolume::singleScatteringWithExtinction(
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
	const double phase = this->_phaseFunction->scatteringPDF(incidentRay, hitRecord, scatteredDirection);
	if (!std::isfinite(phase) || phase <= 0.0)
		return (Color(0.0, 0.0, 0.0));
	return (this->_parameters.albedo * (extinction * phase));
}

bool SparseGridVolume::createBoundingBox(AABB& outputBoundingBox) const
{
	outputBoundingBox = this->_boundingBox;
	return (true);
}

Color SparseGridVolume::volumeAlbedo(void) const
{
	return (this->_parameters.albedo);
}

double SparseGridVolume::volumeFeatureScale(void) const
{
	// The deterministic single-scattering control is deliberately band-limited.
	// Full-resolution stochastic transport still calls densityAt directly. The
	// quality-controlled scale lets final renders resolve substantially more of
	// the source field without changing the represented density or path measure.
	return (this->_featureScale * 32.0 / this->_parameters.primaryDetail);
}

double SparseGridVolume::multipleScatteringFalloff(void) const
{
	return (this->_parameters.multipleScatteringFalloff);
}

double SparseGridVolume::multipleScatteringCompensation(void) const
{
	return (this->_parameters.multipleScatteringCompensation);
}

double SparseGridVolume::integratedDirectionalOpticalDepth(const Ray& ray) const
{
	double entryT, exitT;
	if (!this->boundsInterval(ray, 0.0, T_MAX, entryT, exitT))
		return 0.0;
	BrickTraversal traversal(this->worldToLocalRay(ray));
	const double epsilonT = std::max(1e-10, this->_featureScale * 1e-8);
	const int samples = Sampler::isVolumeControlSampling()
		? std::max(1, this->_parameters.shadowSamplesPerBrick / 8)
		: this->_parameters.shadowSamplesPerBrick;
	double opticalDepth = 0.0;
	for (double currentT = std::max(0.0, entryT); currentT < exitT; )
	{
		BrickSegment segment;
		if (!this->segmentAtLocal(traversal, std::min(exitT, currentT + epsilonT), exitT, segment))
			break;
		if (segment.maximum > 0.0f)
		{
			const double stepT = (segment.exitT - currentT) / samples;
			for (int sample = 0; sample < samples; sample++)
				opticalDepth += this->_extinctionSceneUnits * stepT
					* this->densityAtLocal(traversal.localRay.pointAtRay(currentT + (sample + 0.5) * stepT));
		}
		currentT = segment.exitT + epsilonT;
	}
	// Do not clamp at the transmittance cutoff: reconstruction needs deep tau.
	return opticalDepth;
}

bool SparseGridVolume::directionalOpticalDepth(
	const Vector3& position,
	const Vector3& direction,
	double& opticalDepth
) const
{
	const double lengthSquared = Utilities::vectorLengthSquared(direction);
	if (!std::isfinite(lengthSquared) || lengthSquared <= VOLUME_EPSILON)
		return (false);
	const Vector3 normalizedDirection = direction / std::sqrt(lengthSquared);
	if (!this->_parameters.directionalCache)
	{
		// Keep the reconstruction model supplied with optical depth when the
		// cache is disabled. Returning false here would change sky/cavity fill.
		opticalDepth = this->integratedDirectionalOpticalDepth(
			Ray::fromNormalizedDirection(position, normalizedDirection));
		return std::isfinite(opticalDepth);
	}
	const DirectionalTransmittanceCache* cache = this->directionalCache(
		normalizedDirection
	);
	opticalDepth = this->cachedDirectionalOpticalDepth(*cache, position);
	return (std::isfinite(opticalDepth));
}

const GridVolumeParameters& SparseGridVolume::getParameters(void) const
{
	return (this->_parameters);
}

const SparseVolumeGrid& SparseGridVolume::getGrid(void) const
{
	return (this->_grid);
}
