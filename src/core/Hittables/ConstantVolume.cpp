#include "Hittables/ConstantVolume.hpp"
#include "Hittables/Sphere.hpp"
#include "Materials/Isotropic.hpp"
#include "Defaults.hpp"
#include "Utilities.hpp"
#include "Sampler.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <stdexcept>

namespace
{
	constexpr std::uint32_t VOLUME_STREAM_DIMENSION = 0x31000000u;

	std::uint32_t mixBits(std::uint32_t value)
	{
		value ^= value >> 16u;
		value *= 0x7feb352du;
		value ^= value >> 15u;
		value *= 0x846ca68bu;
		value ^= value >> 16u;
		return (value);
	}

	std::uint32_t nextSamplingStream(void)
	{
		static std::atomic<std::uint32_t> nextStream(1u);
		return (nextStream.fetch_add(1u, std::memory_order_relaxed));
	}

	double	validatedDensity(double density)
	{
		if (!std::isfinite(density) || density <= 0.0)
		{
			throw std::invalid_argument("Volume density must be finite and positive.");
		}
		return (density);
	}
}

/*
	Constructors
*/

// Constructs the ConstantVolume with default values
ConstantVolume::ConstantVolume(void)
{
	this->_samplingStream = nextSamplingStream();
	this->_boundary = std::make_shared<Sphere>(
		Vector3(0.0, 0.0, 0.0),
		6.0,
		nullptr
	);
	this->_phaseFunction = std::make_shared<Isotropic>(Color(0.6, 0.6, 0.6));
	this->_density = D_VOLUME_DENSITY;
	this->_negativeInverseDensity = -1.0 / D_VOLUME_DENSITY;
}

// Constructs the ConstantVolume with custom values
ConstantVolume::ConstantVolume(std::shared_ptr<Hittable> boundary, std::shared_ptr<Material> phaseFunction, double density)
{
	this->_samplingStream = nextSamplingStream();
	if (!boundary)
	{
		throw std::invalid_argument("Volume boundary must not be null.");
	}
	if (!phaseFunction)
	{
		throw std::invalid_argument("Volume phase function must not be null.");
	}
	this->_boundary = boundary;
	this->_phaseFunction = phaseFunction;
	this->_density = validatedDensity(density);
	this->_negativeInverseDensity = -1.0 / this->_density;
}

// Returns the ConstantVolume's material
Material*	ConstantVolume::getMaterial(void) const
{
	return (this->_phaseFunction.get());
}

bool	ConstantVolume::sampleScatteringDistance(Ray& ray, double t_min, double t_max, double& hitT) const
{
	double entryT;
	double exitT;

	if (!this->_boundary || !this->_phaseFunction)
	{
		return (false);
	}
	if (!this->_boundary->hitInterval(ray, -T_MAX, T_MAX, entryT, exitT))
	{
		return (false);
	}

	if (entryT < t_min)
	{
		entryT = t_min;
	}
	if (exitT > t_max)
	{
		exitT = t_max;
	}
	if (entryT >= exitT)
	{
		return (false);
	}

	if (entryT < 0)
	{
		entryT = 0;
	}

	double rayLength = Utilities::vectorLength(ray.getDirection());
	if (rayLength <= 0.0 || !std::isfinite(rayLength))
	{
		return (false);
	}
	double distanceInsideBoundary = (exitT - entryT) * rayLength;
	const std::uint32_t dimension = VOLUME_STREAM_DIMENSION
		+ Sampler::DIM_VOLUME_DISTANCE
		+ ((mixBits(this->_samplingStream) & 0x01ffffffu) << 5u);
	double hitDistance = this->_negativeInverseDensity
		* log(std::max(Sampler::sample1D(dimension), 1e-12));

	if (hitDistance > distanceInsideBoundary)
	{
		return (false);
	}

	hitT = entryT + hitDistance / rayLength;
	return (true);
}

// Calculates if the ConstantVolume is hit by 'ray', is closer than 't_max' and farther than T_MIN
bool	ConstantVolume::hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const
{
	if (!this->sampleScatteringDistance(ray, t_min, t_max, hitRecord.t0))
	{
		return (false);
	}
	hitRecord.position = ray.pointAtRay(hitRecord.t0);
	hitRecord.normal = Vector3(1.0, 0.0, 0.0); // Arbitrary
	hitRecord.geometricNormal = hitRecord.normal;
	hitRecord.material = this->_phaseFunction.get();

	return (true);
}

bool	ConstantVolume::hitAny(Ray& ray, double t_min, double t_max) const
{
	double hitT;

	return (this->sampleScatteringDistance(ray, t_min, t_max, hitT));
}

Color ConstantVolume::shadowTransmittance(Ray& ray, double t_min, double t_max) const
{
	double entryT;
	double exitT;
	if (!this->_boundary || !this->_boundary->hitInterval(ray, -T_MAX, T_MAX, entryT, exitT))
		return (Color(1.0, 1.0, 1.0));
	entryT = std::max(entryT, std::max(0.0, t_min));
	exitT = std::min(exitT, t_max);
	if (entryT >= exitT)
		return (Color(1.0, 1.0, 1.0));
	const double rayLength = Utilities::vectorLength(ray.getDirection());
	if (!std::isfinite(rayLength) || rayLength <= 0.0)
		return (Color(1.0, 1.0, 1.0));
	const double value = std::exp(-this->_density * (exitT - entryT) * rayLength);
	return (Color(value, value, value));
}

// Creates an AABB / bounding box for this ConstantVolume
bool	ConstantVolume::createBoundingBox(AABB& outputBoundingBox) const
{
	return (this->_boundary->createBoundingBox(outputBoundingBox));
}

double	ConstantVolume::getDensity(void) const
{
	return (this->_density);
}
