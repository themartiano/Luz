#include "VolumeGuidingField.hpp"
#include "Defaults.hpp"
#include "ONB.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
	constexpr double GOLDEN_ANGLE = 2.39996322972865332223;
	constexpr double FIXED_POINT_SCALE = 1048576.0;
	constexpr double MAX_RECORDED_WEIGHT = 1048576.0;

	bool finiteVector(const Vector3& value)
	{
		return (
			std::isfinite(value.getX())
			&& std::isfinite(value.getY())
			&& std::isfinite(value.getZ())
		);
	}

	void saturatingAdd(std::atomic<std::uint64_t>& destination, std::uint64_t value)
	{
		std::uint64_t current = destination.load(std::memory_order_relaxed);
		while (true)
		{
			const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
			const std::uint64_t updated = value > maximum - current
				? maximum
				: current + value;
			if (destination.compare_exchange_weak(
				current,
				updated,
				std::memory_order_relaxed,
				std::memory_order_relaxed
			))
				return;
		}
	}
}

VolumeGuidingField::VolumeGuidingField(
	const Vector3& minimum,
	const Vector3& maximum,
	std::uint32_t spatialResolution,
	std::uint32_t directionalLobes,
	double lobeAnisotropy
)
	: _minimum(minimum), _maximum(maximum), _spatialResolution(spatialResolution),
	  _directionalLobes(directionalLobes), _lobeAnisotropy(lobeAnisotropy)
{
	if (!finiteVector(minimum) || !finiteVector(maximum))
		throw std::invalid_argument("Volume guiding bounds must be finite.");
	const Vector3 extent = maximum - minimum;
	if (extent.getX() <= 0.0 || extent.getY() <= 0.0 || extent.getZ() <= 0.0)
		throw std::invalid_argument("Volume guiding bounds must have positive extent.");
	if (spatialResolution < 1 || spatialResolution > 128)
		throw std::invalid_argument("Volume guiding spatial resolution must be between 1 and 128.");
	if (directionalLobes < 4 || directionalLobes > 64)
		throw std::invalid_argument("Volume guiding directional lobes must be between 4 and 64.");
	if (!std::isfinite(lobeAnisotropy) || lobeAnisotropy < 0.0 || lobeAnisotropy > 0.95)
		throw std::invalid_argument("Volume guiding lobe anisotropy must be between 0 and 0.95.");

	_inverseExtent = Vector3(1.0 / extent.getX(), 1.0 / extent.getY(), 1.0 / extent.getZ());
	_cellCount = static_cast<std::size_t>(spatialResolution)
		* static_cast<std::size_t>(spatialResolution)
		* static_cast<std::size_t>(spatialResolution);
	const std::size_t weightCount = _cellCount * static_cast<std::size_t>(directionalLobes);
	_trainingWeights = std::make_unique<std::atomic<std::uint64_t>[]>(weightCount);
	for (std::size_t index = 0; index < weightCount; index++)
		_trainingWeights[index].store(0, std::memory_order_relaxed);
	_lobeCenters.reserve(directionalLobes);
	for (std::uint32_t lobe = 0; lobe < directionalLobes; lobe++)
	{
		const double z = 1.0 - 2.0 * (static_cast<double>(lobe) + 0.5)
			/ static_cast<double>(directionalLobes);
		const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
		const double phi = GOLDEN_ANGLE * static_cast<double>(lobe);
		_lobeCenters.emplace_back(radius * std::cos(phi), radius * std::sin(phi), z);
	}
}

std::size_t VolumeGuidingField::spatialIndex(const Vector3& position) const
{
	if (!finiteVector(position))
		return (_cellCount);
	const Vector3 normalized(
		(position.getX() - _minimum.getX()) * _inverseExtent.getX(),
		(position.getY() - _minimum.getY()) * _inverseExtent.getY(),
		(position.getZ() - _minimum.getZ()) * _inverseExtent.getZ()
	);
	if (
		normalized.getX() < 0.0 || normalized.getX() > 1.0
		|| normalized.getY() < 0.0 || normalized.getY() > 1.0
		|| normalized.getZ() < 0.0 || normalized.getZ() > 1.0
	)
		return (_cellCount);
	auto coordinate = [this](double value) {
		return (std::min(
			_spatialResolution - 1u,
			static_cast<std::uint32_t>(value * static_cast<double>(_spatialResolution))
		));
	};
	const std::uint32_t x = coordinate(normalized.getX());
	const std::uint32_t y = coordinate(normalized.getY());
	const std::uint32_t z = coordinate(normalized.getZ());
	return ((static_cast<std::size_t>(z) * _spatialResolution + y)
		* _spatialResolution + x);
}

std::size_t VolumeGuidingField::nearestLobe(const Vector3& direction) const
{
	const double lengthSquared = Utilities::vectorLengthSquared(direction);
	if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
		return (_directionalLobes);
	const Vector3 normalized = direction / std::sqrt(lengthSquared);
	std::size_t selected = 0;
	double selectedDot = -std::numeric_limits<double>::infinity();
	for (std::size_t lobe = 0; lobe < _lobeCenters.size(); lobe++)
	{
		const double dot = Utilities::dot(normalized, _lobeCenters[lobe]);
		if (dot > selectedDot)
		{
			selectedDot = dot;
			selected = lobe;
		}
	}
	return (selected);
}

void VolumeGuidingField::record(
	const Vector3& position,
	const Vector3& incidentDirection,
	double radianceWeight
)
{
	if (_frozen.load(std::memory_order_acquire) || !std::isfinite(radianceWeight) || radianceWeight <= 0.0)
		return;
	const std::size_t cell = this->spatialIndex(position);
	const std::size_t lobe = this->nearestLobe(incidentDirection);
	if (cell >= _cellCount || lobe >= _directionalLobes)
		return;
	const double clamped = std::min(radianceWeight, MAX_RECORDED_WEIGHT);
	const std::uint64_t fixedWeight = std::max<std::uint64_t>(
		1,
		static_cast<std::uint64_t>(std::llround(clamped * FIXED_POINT_SCALE))
	);
	saturatingAdd(
		_trainingWeights[cell * static_cast<std::size_t>(_directionalLobes) + lobe],
		fixedWeight
	);
}

void VolumeGuidingField::freeze(double priorWeight)
{
	if (!std::isfinite(priorWeight) || priorWeight <= 0.0)
		throw std::invalid_argument("Volume guiding prior weight must be finite and positive.");
	if (_frozen.load(std::memory_order_acquire))
		return;
	const std::size_t weightCount = _cellCount * static_cast<std::size_t>(_directionalLobes);
	_probabilities.assign(weightCount, 0.0);
	for (std::size_t cell = 0; cell < _cellCount; cell++)
	{
		double total = 0.0;
		for (std::size_t lobe = 0; lobe < _directionalLobes; lobe++)
		{
			const std::size_t index = cell * static_cast<std::size_t>(_directionalLobes) + lobe;
			const double weight = static_cast<double>(
				_trainingWeights[index].load(std::memory_order_relaxed)
			) / FIXED_POINT_SCALE + priorWeight;
			_probabilities[index] = weight;
			total += weight;
		}
		for (std::size_t lobe = 0; lobe < _directionalLobes; lobe++)
			_probabilities[cell * static_cast<std::size_t>(_directionalLobes) + lobe] /= total;
	}
	_frozen.store(true, std::memory_order_release);
}

double VolumeGuidingField::lobePDF(const Vector3& center, const Vector3& direction) const
{
	const double lengthSquared = Utilities::vectorLengthSquared(direction);
	if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0)
		return (0.0);
	const double cosine = std::clamp(
		Utilities::dot(center, direction / std::sqrt(lengthSquared)),
		-1.0,
		1.0
	);
	const double g = _lobeAnisotropy;
	const double denominator = std::max(1e-12, 1.0 + g * g - 2.0 * g * cosine);
	return ((1.0 - g * g) / (4.0 * D_PI * denominator * std::sqrt(denominator)));
}

Vector3 VolumeGuidingField::sampleLobe(
	const Vector3& center,
	const Sampler::Sample2D& sample
) const
{
	const double u = std::clamp(static_cast<double>(sample.x), 0.0, 1.0);
	const double v = std::clamp(static_cast<double>(sample.y), 0.0, 1.0);
	const double phi = 2.0 * D_PI * u;
	const double g = _lobeAnisotropy;
	double cosine = 1.0 - 2.0 * v;
	if (g > 1e-6)
	{
		const double remapped = (1.0 - g * g) / (1.0 - g + 2.0 * g * v);
		cosine = std::clamp((1.0 + g * g - remapped * remapped) / (2.0 * g), -1.0, 1.0);
	}
	const double sine = std::sqrt(std::max(0.0, 1.0 - cosine * cosine));
	return (ONB(center).local(sine * std::cos(phi), sine * std::sin(phi), cosine));
}

double VolumeGuidingField::pdf(const Vector3& position, const Vector3& direction) const
{
	if (!_frozen.load(std::memory_order_acquire))
		return (0.0);
	const std::size_t cell = this->spatialIndex(position);
	if (cell >= _cellCount)
		return (0.0);
	double result = 0.0;
	for (std::size_t lobe = 0; lobe < _directionalLobes; lobe++)
	{
		result += _probabilities[cell * static_cast<std::size_t>(_directionalLobes) + lobe]
			* this->lobePDF(_lobeCenters[lobe], direction);
	}
	return (result);
}

VolumeGuidingField::Sample VolumeGuidingField::sample(
	const Vector3& position,
	double lobeSample,
	const Sampler::Sample2D& directionSample
) const
{
	Sample result;
	if (!_frozen.load(std::memory_order_acquire) || !std::isfinite(lobeSample))
		return (result);
	const std::size_t cell = this->spatialIndex(position);
	if (cell >= _cellCount)
		return (result);
	const double target = std::clamp(lobeSample, 0.0, std::nextafter(1.0, 0.0));
	double cumulative = 0.0;
	std::size_t selected = _directionalLobes - 1u;
	for (std::size_t lobe = 0; lobe < _directionalLobes; lobe++)
	{
		cumulative += _probabilities[cell * static_cast<std::size_t>(_directionalLobes) + lobe];
		if (target < cumulative)
		{
			selected = lobe;
			break;
		}
	}
	result.direction = this->sampleLobe(_lobeCenters[selected], directionSample);
	result.pdf = this->pdf(position, result.direction);
	result.valid = result.pdf > 0.0 && std::isfinite(result.pdf);
	return (result);
}

bool VolumeGuidingField::isFrozen(void) const
{
	return (_frozen.load(std::memory_order_acquire));
}

bool VolumeGuidingField::contains(const Vector3& position) const
{
	return (this->spatialIndex(position) < _cellCount);
}

std::size_t VolumeGuidingField::cellCount(void) const
{
	return (_cellCount);
}

std::size_t VolumeGuidingField::memoryBytes(void) const
{
	return (
		_cellCount * static_cast<std::size_t>(_directionalLobes)
			* (sizeof(std::atomic<std::uint64_t>) + sizeof(double))
		+ _lobeCenters.size() * sizeof(Vector3)
	);
}
