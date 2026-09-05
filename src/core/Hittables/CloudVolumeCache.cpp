#include "Hittables/CloudVolume.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace
{
	constexpr double CLOUD_CACHE_EPSILON = 1e-12;
	constexpr double GAUSS_POINT_LOW = 0.21132486540518713;
	constexpr double GAUSS_POINT_HIGH = 0.78867513459481287;
	bool sameDirection(const Vector3& a, const Vector3& b)
	{
		return Utilities::vectorLengthSquared(a - b) < 1e-18;
	}
}

// Sweep from the light-facing boundary, integrating each segment with two
// Gauss points and interpolating optical depth on the preceding slice.
std::shared_ptr<const CloudVolume::DirectionalTransmittanceCache>
CloudVolume::buildDirectionalCache(const Vector3& direction) const
{
	auto cache = std::make_shared<DirectionalTransmittanceCache>();
	cache->direction = direction;
	const Vector3 localDirection = direction;
	const double spacing = this->_parameters.featureScale / this->_parameters.directionalCacheResolution;
	for (int axis = 0; axis < 3; axis++)
		cache->cells[axis] = static_cast<std::uint32_t>(std::clamp(
			std::ceil(this->_parameters.size[axis] / spacing), 1.0, 256.0));
	auto pointCountEstimate = [&cache]() {
		return (static_cast<std::uint64_t>(cache->cells[0] + 1u)
			* (cache->cells[1] + 1u) * (cache->cells[2] + 1u));
	};
	while (pointCountEstimate() > 1048576u)
	{
		const auto largest = std::max_element(cache->cells.begin(), cache->cells.end());
		(*largest)--;
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
	if (dominantRate <= CLOUD_CACHE_EPSILON)
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
					if (localDirection[axis] > CLOUD_CACHE_EPSILON)
						distanceToBoundary = std::min(
							distanceToBoundary,
							(this->_maximum[axis] - position[axis]) / localDirection[axis]
						);
					else if (localDirection[axis] < -CLOUD_CACHE_EPSILON)
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
				if (segmentDistance <= CLOUD_CACHE_EPSILON)
					continue;
				const Vector3 lowSamplePosition = position
					+ localDirection * (segmentDistance * GAUSS_POINT_LOW);
				const Vector3 highSamplePosition = position
					+ localDirection * (segmentDistance * GAUSS_POINT_HIGH);
				double opticalDepth = this->_majorantSceneUnits * segmentDistance * 0.5
					* (this->densityAt(lowSamplePosition)
						+ this->densityAt(highSamplePosition));
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

const CloudVolume::DirectionalTransmittanceCache*
CloudVolume::directionalCache(const Vector3& direction) const
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
	if (snapshot && snapshot->size() >= 4u)
		return nullptr;
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

double CloudVolume::cachedDirectionalOpticalDepth(
	const DirectionalTransmittanceCache& cache,
	const Vector3& position
) const
{
	const Vector3 normalized(
		(position.getX() - this->_minimum.getX()) / this->_parameters.size.getX(),
		(position.getY() - this->_minimum.getY()) / this->_parameters.size.getY(),
		(position.getZ() - this->_minimum.getZ()) / this->_parameters.size.getZ()
	);
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


bool CloudVolume::directionalOpticalDepth(
	const Vector3& position, const Vector3& direction, double& opticalDepth
) const
{
	if (this->_parameters.directionalCacheResolution <= 0.0)
		return false;
	for (int axis = 0; axis < 3; axis++)
		if (!std::isfinite(position[axis]) || !std::isfinite(direction[axis]))
			return false;
	const double length = Utilities::vectorLength(direction);
	if (!std::isfinite(length) || length <= CLOUD_CACHE_EPSILON)
		return false;
	const Vector3 normalizedDirection = direction / length;
	Ray ray = Ray::fromNormalizedDirection(position, normalizedDirection);
	double entry, exit;
	if (!boundsInterval(ray, 0.0, std::numeric_limits<double>::max(), entry, exit))
	{
		opticalDepth = 0.0;
		return true;
	}
	const auto* cache = directionalCache(normalizedDirection);
	if (!cache)
		return false;
	opticalDepth = cachedDirectionalOpticalDepth(*cache, ray.pointAtRay(entry));
	return true;
}
