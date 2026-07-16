#include "Atmosphere.hpp"
#include "Defaults.hpp"
#include "ColorManagement.hpp"
#include "ColorScience.hpp"
#include "LightUnits.hpp"
#include "ONB.hpp"
#include "Utilities.hpp"
#include "SystemSpecifics.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace
{
	constexpr double kMieG = 0.76;
	const double kRayleighPhaseScale = 3.0 / (16.0 * D_PI);
	const double kMiePhaseScale = 3.0 / (8.0 * D_PI);
	constexpr double kMieAbsorptionScale = 1.1;

	Vector3	exponentialAttenuation(const Vector3& tau)
	{
		return (Vector3(std::exp(-tau.getX()), std::exp(-tau.getY()), std::exp(-tau.getZ())));
	}

	Color	atmosphereTransmittanceToACEScg(const Vector3& linearSRGB)
	{
		const Color converted = ColorManagement::acescgFromLinearSRGB(Color(
			linearSRGB.getX(),
			linearSRGB.getY(),
			linearSRGB.getZ()
		));
		return (Color(
			std::clamp(converted.getRed(), 0.0, 1.0),
			std::clamp(converted.getGreen(), 0.0, 1.0),
			std::clamp(converted.getBlue(), 0.0, 1.0)
		));
	}

	double	densityAtHeight(double height, double inverseScaleHeight)
	{
		if (height < 0.0 || !std::isfinite(height))
		{
			return (0.0);
		}
		return (std::exp(-height * inverseScaleHeight));
	}

	Vector3	normalizedSunDirection(Vector3 sunDirection)
	{
		if (
			!std::isfinite(sunDirection.getX())
			|| !std::isfinite(sunDirection.getY())
			|| !std::isfinite(sunDirection.getZ())
			|| Utilities::vectorLengthSquared(sunDirection) <= 0.0
		)
		{
			throw std::invalid_argument("Atmosphere sun direction must be finite and non-zero.");
		}
		return (Utilities::normalize(sunDirection));
	}

	void	requireFiniteNonNegativeColor(const Color& color, const std::string& description)
	{
		if (
			!std::isfinite(color.getRed())
			|| !std::isfinite(color.getGreen())
			|| !std::isfinite(color.getBlue())
			|| color.getRed() < 0.0
			|| color.getGreen() < 0.0
			|| color.getBlue() < 0.0
		)
		{
			throw std::invalid_argument(description + " must be finite and non-negative.");
		}
	}
}

struct Atmosphere::DiffuseSkyCache
{
	std::mutex mutex;
	bool valid = false;
	Vector3 position;
	Color radiance;
};

/*
	Constructors
*/

// Constructs the Atmosphere with default values
Atmosphere::Atmosphere(void)
{
	this->_diffuseSkyCache = std::make_shared<DiffuseSkyCache>();
	this->_sunAngle = -0.4;
	this->_earthRadius = D_EARTH_RADIUS;
	this->_atmosphereRadius = D_ATMOSPHERE_RADIUS;
	this->_hR = D_HR;
	this->_hM = D_HM;
	this->_samples = 16;
	this->_lightSamples = 8;
	this->_starsBrightness = 0.5;
	this->_sunRadiance = LightUnits::solarDirectionalIrradiance(ColorScience::solar(), 1.0);
	this->_sunRadianceScale = 1.0;
	this->_metersPerUnit = 1.0;
	updateSunDirectionVector();
}

// Constructs the Atmosphere with custom values
Atmosphere::Atmosphere(double sunAngle, double earthRadius, double atmosphereRadius, double hR, double hM, int samples, int lightSamples, double starsBrightness)
{
	this->_diffuseSkyCache = std::make_shared<DiffuseSkyCache>();
	this->_sunAngle = -0.4;
	this->_earthRadius = D_EARTH_RADIUS;
	this->_atmosphereRadius = D_ATMOSPHERE_RADIUS;
	this->_hR = D_HR;
	this->_hM = D_HM;
	this->_samples = 16;
	this->_lightSamples = 8;
	this->_starsBrightness = 0.5;
	this->_sunRadiance = LightUnits::solarDirectionalIrradiance(ColorScience::solar(), 1.0);
	this->_sunRadianceScale = 1.0;
	this->_metersPerUnit = 1.0;
	setSunAngle(sunAngle);
	if (!std::isfinite(earthRadius) || earthRadius <= 0.0)
	{
		throw std::invalid_argument("Earth radius must be positive.");
	}
	if (!std::isfinite(atmosphereRadius) || atmosphereRadius <= earthRadius)
	{
		throw std::invalid_argument("Atmosphere radius must be larger than Earth radius.");
	}
	this->_earthRadius = earthRadius;
	this->_atmosphereRadius = atmosphereRadius;
	setHR(hR);
	setHM(hM);
	setSamples(samples);
	setLightSamples(lightSamples);
	setStarsBrightness(starsBrightness);
}

// Earth molecular scattering coefficients from the production atmosphere
// model (Hillaire 2020), in the linear-sRGB spectral approximation.
const Vector3 Atmosphere::betaR(5.802e-6, 13.558e-6, 33.1e-6);
// Clear-air Mie scattering. The previous 21e-6 value described heavy aerosol
// haze and desaturated every daytime sky; absorption is applied separately.
const Vector3 Atmosphere::betaM(3.996e-6, 3.996e-6, 3.996e-6);

// Returns the Earth Radius
double  Atmosphere::getEarthRadius(void) const
{
	return (this->_earthRadius);
}

//Returns the Atmosphere Radius
double  Atmosphere::getAtmosphereRadius(void) const
{
	return (this->_atmosphereRadius);
}

// Returns the Earth Radius
double  Atmosphere::getStarsBrightness(void) const
{
	return (this->_starsBrightness);
}

Vector3	Atmosphere::getSunDirection(void) const
{
	return (this->_sunDirection);
}

Color	Atmosphere::getSunRadiance(void) const
{
	return (this->_sunRadiance);
}

double	Atmosphere::getSunRadianceScale(void) const
{
	return (this->_sunRadianceScale);
}

double	Atmosphere::getMetersPerUnit(void) const
{
	return (this->_metersPerUnit);
}

double	Atmosphere::sceneUnitsToMeters(double sceneUnits) const
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

double	Atmosphere::metersToSceneUnits(double meters) const
{
	return (meters / this->_metersPerUnit);
}

//Returns the Sun Angle (double)
double  Atmosphere::getSunAngle(void) const
{
	return (this->_sunAngle);
}

//Sets the Sun Angle and updates internal variables
void	Atmosphere::setSunAngle(double newAngle)
{
	if (!std::isfinite(newAngle))
	{
		throw std::invalid_argument("Sun angle must be finite.");
	}
	this->_sunAngle = newAngle;
	updateSunDirectionVector();
}

void	Atmosphere::setSunDirection(Vector3 sunDirection)
{
	this->_sunDirection = normalizedSunDirection(sunDirection);
	this->invalidateDiffuseSkyCache();
}

void	Atmosphere::setSunRadiance(Color sunRadiance)
{
	requireFiniteNonNegativeColor(sunRadiance, "Atmosphere sun radiance");
	this->_sunRadiance = sunRadiance;
	this->invalidateDiffuseSkyCache();
}

void	Atmosphere::setSunRadianceScale(double sunRadianceScale)
{
	if (!std::isfinite(sunRadianceScale) || sunRadianceScale < 0.0)
	{
		throw std::invalid_argument("Atmosphere sun radiance scale must be finite and non-negative.");
	}
	this->_sunRadianceScale = sunRadianceScale;
	this->invalidateDiffuseSkyCache();
}

void	Atmosphere::setMetersPerUnit(double metersPerUnit)
{
	if (!std::isfinite(metersPerUnit) || metersPerUnit <= 0.0)
	{
		throw std::invalid_argument("Atmosphere meters per unit must be finite and positive.");
	}
	this->_metersPerUnit = metersPerUnit;
	this->invalidateDiffuseSkyCache();
}

// Sets the EarthRadius
void	Atmosphere::setEarthRadius(double earthRadius)
{
	if (!std::isfinite(earthRadius) || earthRadius <= 0.0)
	{
		throw std::invalid_argument("Earth radius must be positive.");
	}
	if (earthRadius >= this->_atmosphereRadius)
	{
		throw std::invalid_argument("Earth radius must be smaller than atmosphere radius.");
	}
	this->_earthRadius = earthRadius;
	this->invalidateDiffuseSkyCache();
}

// Sets the AtmosphereRadius
void	Atmosphere::setAtmosphereRadius(double atmosphereRadius)
{
	if (!std::isfinite(atmosphereRadius) || atmosphereRadius <= 0.0)
	{
		throw std::invalid_argument("Atmosphere radius must be positive.");
	}
	if (atmosphereRadius <= this->_earthRadius)
	{
		throw std::invalid_argument("Atmosphere radius must be larger than Earth radius.");
	}
	this->_atmosphereRadius = atmosphereRadius;
	this->invalidateDiffuseSkyCache();
}

// Sets the HR value
void	Atmosphere::setHR(double hR)
{
	if (!std::isfinite(hR) || hR <= 0.0)
	{
		throw std::invalid_argument("Atmosphere HR must be positive.");
	}
	this->_hR = hR;
	this->invalidateDiffuseSkyCache();
}

// Sets the HM value
void	Atmosphere::setHM(double hM)
{
	if (!std::isfinite(hM) || hM <= 0.0)
	{
		throw std::invalid_argument("Atmosphere HM must be positive.");
	}
	this->_hM = hM;
	this->invalidateDiffuseSkyCache();
}

// Sets the Sample count
void	Atmosphere::setSamples(int samples)
{
	if (samples <= 0)
	{
		throw std::invalid_argument("Atmosphere sample count must be positive.");
	}
	this->_samples = samples;
	this->invalidateDiffuseSkyCache();
}

// Sets the Light Sample count
void	Atmosphere::setLightSamples(int lightSamples)
{
	if (lightSamples <= 0)
	{
		throw std::invalid_argument("Atmosphere light sample count must be positive.");
	}
	this->_lightSamples = lightSamples;
	this->invalidateDiffuseSkyCache();
}

// Sets the Stars Brightness
void	Atmosphere::setStarsBrightness(double starsBrightness)
{
	if (!std::isfinite(starsBrightness) || starsBrightness < 0.0)
	{
		throw std::invalid_argument("Stars brightness must be non-negative.");
	}
	this->_starsBrightness = starsBrightness;
}

void	Atmosphere::invalidateDiffuseSkyCache(void)
{
	// Atmosphere remains cheaply copyable, but a mutated copy must never publish
	// its sky estimate into another Atmosphere instance with different settings.
	if (!this->_diffuseSkyCache || this->_diffuseSkyCache.use_count() != 1)
	{
		this->_diffuseSkyCache = std::make_shared<DiffuseSkyCache>();
		return;
	}
	std::lock_guard<std::mutex> lock(this->_diffuseSkyCache->mutex);
	this->_diffuseSkyCache->valid = false;
}

//Updates Sun Direction (Vector3) using Sun Angle (double)
void	Atmosphere::updateSunDirectionVector(void)
{
	double angle = D_PI * this->_sunAngle;
	this->setSunDirection(Vector3(0.0, std::cos(angle), -std::sin(angle)));
}

// (Sphere) Hit function for planets and atmospheres (sets both t0 and t1 on the Hit Record). Returns true if hit occurs, false otherwise
bool planetaryHit(double radius, const Ray& ray, HitRecord& hitRecord)
{
	if (!std::isfinite(radius) || radius <= 0.0)
	{
		return (false);
	}

	const double a = Utilities::dot(ray.getDirection(), ray.getDirection());
	if (!std::isfinite(a) || a <= 0.0)
	{
		return (false);
	}

	const double halfB = Utilities::dot(ray.getOrigin(), ray.getDirection());
	const double c = Utilities::dot(ray.getOrigin(), ray.getOrigin()) - radius * radius;
	const double discriminant = halfB * halfB - a * c;

	if (!std::isfinite(discriminant) || discriminant < 0.0)
	{
		return (false);
	}

	const double sqrtDiscriminant = std::sqrt(std::max(0.0, discriminant));
	hitRecord.t0 = (-halfB - sqrtDiscriminant) / a;
	hitRecord.t1 = (-halfB + sqrtDiscriminant) / a;

	if (hitRecord.t0 > hitRecord.t1)
	{
		std::swap(hitRecord.t0, hitRecord.t1);
	}

	return (true);
}

// Samples atmosphere in-scattering and view transmittance for 'ray'.
AtmosphereSample	Atmosphere::sampleSegment(const Ray& ray, double t_max) const
{
	AtmosphereSample sample;

	if (!std::isfinite(t_max) || t_max <= T_MIN)
	{
		return (sample);
	}
	const double tMaxMeters = this->sceneUnitsToMeters(t_max);
	const double tMinMeters = this->sceneUnitsToMeters(T_MIN);
	if (!std::isfinite(tMaxMeters) || tMaxMeters <= tMinMeters)
	{
		return (sample);
	}
	const Ray meterRay(
		ray.getOrigin() * this->_metersPerUnit,
		ray.getDirection()
	);
	HitRecord atmosphereHitRecord;
	if (!planetaryHit(this->_atmosphereRadius, meterRay, atmosphereHitRecord) || atmosphereHitRecord.t1 <= tMinMeters)
	{
		return (sample);
	}

	HitRecord earthHitRecord;
	double tMax = tMaxMeters;
	if (planetaryHit(this->_earthRadius, meterRay, earthHitRecord) && earthHitRecord.t1 > tMinMeters)
	{
		tMax = std::min(tMax, std::max(0.0, earthHitRecord.t0));
	}

	const double t_min = std::max(tMinMeters, atmosphereHitRecord.t0);
	tMax = std::min(tMax, atmosphereHitRecord.t1);
	if (!std::isfinite(t_min) || !std::isfinite(tMax) || tMax <= t_min)
	{
		return (sample);
	}

	const double segmentLength = (tMax - t_min) / this->_samples;
	if (!std::isfinite(segmentLength) || segmentLength <= 0.0)
	{
		return (sample);
	}

	const double inverseHR = 1.0 / this->_hR;
	const double inverseHM = 1.0 / this->_hM;
	Vector3 sumR(0.0, 0.0, 0.0);
	Vector3 sumM(0.0, 0.0, 0.0);
	double  opticalDepthR = 0.0;
	double  opticalDepthM = 0.0;
	const double mu = std::clamp(Utilities::dot(meterRay.getDirection(), this->_sunDirection), -1.0, 1.0);
	const double muSquared = mu * mu;
	const double phaseR = kRayleighPhaseScale * (1.0 + muSquared);
	const double mieDenominatorBase = std::max(1.0 + kMieG * kMieG - 2.0 * kMieG * mu, 1.0e-6);
	const double mieDenominator = mieDenominatorBase * std::sqrt(mieDenominatorBase);
	const double phaseM = kMiePhaseScale * (
		(1.0 - kMieG * kMieG) * (1.0 + muSquared)
		/ ((2.0 + kMieG * kMieG) * mieDenominator)
	);

	for (int i = 0; i < this->_samples; i++)
	{
		const double viewSampleT = t_min + (static_cast<double>(i) + 0.5) * segmentLength;
		const Vector3 samplePosition = meterRay.getOrigin() + viewSampleT * meterRay.getDirection();
		const double height = Utilities::vectorLength(samplePosition) - this->_earthRadius;
		const double densityR = densityAtHeight(height, inverseHR);
		const double densityM = densityAtHeight(height, inverseHM);

		if (densityR == 0.0 && densityM == 0.0)
		{
			continue;
		}

		const double sampleOpticalDepthR = densityR * segmentLength;
		const double sampleOpticalDepthM = densityM * segmentLength;
		opticalDepthR += sampleOpticalDepthR;
		opticalDepthM += sampleOpticalDepthM;

		Ray ray2(samplePosition, this->_sunDirection);
		HitRecord hitRecord2;
		if (!planetaryHit(this->_atmosphereRadius, ray2, hitRecord2) || hitRecord2.t1 <= 0.0)
		{
			continue;
		}

		const double segmentLengthLight = hitRecord2.t1 / this->_lightSamples;
		if (!std::isfinite(segmentLengthLight) || segmentLengthLight <= 0.0)
		{
			continue;
		}

		double  opticalDepthLightR = 0.0;
		double  opticalDepthLightM = 0.0;
		bool	reachesSun = true;

		for (int j = 0; j < this->_lightSamples; j++)
		{
			const double lightSampleT = (static_cast<double>(j) + 0.5) * segmentLengthLight;
			const Vector3 samplePositionLight = samplePosition + lightSampleT * this->_sunDirection;
			const double heightLight = Utilities::vectorLength(samplePositionLight) - this->_earthRadius;
			if (heightLight < 0.0)
			{
				reachesSun = false;
				break;
			}
			opticalDepthLightR += densityAtHeight(heightLight, inverseHR) * segmentLengthLight;
			opticalDepthLightM += densityAtHeight(heightLight, inverseHM) * segmentLengthLight;
		}

		if (reachesSun)
		{
			const Vector3 tau = betaR * (opticalDepthR + opticalDepthLightR)
				+ betaM * kMieAbsorptionScale * (opticalDepthM + opticalDepthLightM);
			const Vector3 attenuation = exponentialAttenuation(tau);
			sumR += attenuation * sampleOpticalDepthR;
			sumM += attenuation * sampleOpticalDepthM;
		}
	}

	const Vector3 viewTau = betaR * opticalDepthR + betaM * kMieAbsorptionScale * opticalDepthM;
	const Vector3 viewTransmittance = exponentialAttenuation(viewTau);
	// The compact betaR/betaM triplets are sampled linear-sRGB coefficients.
	// Keep their channel arithmetic in that basis, then cross the color-space
	// boundary once into Luz's scene-linear ACEScg working space.
	const Vector3 sunRadiance = static_cast<Vector3>(
		ColorManagement::linearSRGBFromACEScg(this->_sunRadiance)
		* this->_sunRadianceScale
	);
	Vector3 result = (sumR * betaR * phaseR + sumM * betaM * phaseM) * sunRadiance;
	sample.inScattering = ColorManagement::acescgFromLinearSRGB(Color(
		result.getX(),
		result.getY(),
		result.getZ()
	));
	sample.transmittance = atmosphereTransmittanceToACEScg(viewTransmittance);
	return (sample);
}

// Integrates view-ray extinction without the more expensive in-scattering light loop.
Color	Atmosphere::sampleTransmittance(const Ray& ray, double t_max) const
{
	if (!std::isfinite(t_max) || t_max <= T_MIN)
		return (Color(1.0, 1.0, 1.0));
	const double tMaxMeters = this->sceneUnitsToMeters(t_max);
	const double tMinMeters = this->sceneUnitsToMeters(T_MIN);
	if (!std::isfinite(tMaxMeters) || tMaxMeters <= tMinMeters)
		return (Color(1.0, 1.0, 1.0));

	const Ray meterRay(ray.getOrigin() * this->_metersPerUnit, ray.getDirection());
	HitRecord atmosphereHitRecord;
	if (!planetaryHit(this->_atmosphereRadius, meterRay, atmosphereHitRecord) || atmosphereHitRecord.t1 <= tMinMeters)
		return (Color(1.0, 1.0, 1.0));

	double tMax = std::min(tMaxMeters, atmosphereHitRecord.t1);
	HitRecord earthHitRecord;
	if (planetaryHit(this->_earthRadius, meterRay, earthHitRecord) && earthHitRecord.t1 > tMinMeters)
		tMax = std::min(tMax, std::max(0.0, earthHitRecord.t0));
	const double tMin = std::max(tMinMeters, atmosphereHitRecord.t0);
	if (!std::isfinite(tMin) || !std::isfinite(tMax) || tMax <= tMin)
		return (Color(1.0, 1.0, 1.0));

	const double segmentLength = (tMax - tMin) / this->_samples;
	if (!std::isfinite(segmentLength) || segmentLength <= 0.0)
		return (Color(1.0, 1.0, 1.0));
	const double inverseHR = 1.0 / this->_hR;
	const double inverseHM = 1.0 / this->_hM;
	double opticalDepthR = 0.0;
	double opticalDepthM = 0.0;
	for (int i = 0; i < this->_samples; i++)
	{
		const double sampleT = tMin + (static_cast<double>(i) + 0.5) * segmentLength;
		const Vector3 position = meterRay.getOrigin() + sampleT * meterRay.getDirection();
		const double height = Utilities::vectorLength(position) - this->_earthRadius;
		opticalDepthR += densityAtHeight(height, inverseHR) * segmentLength;
		opticalDepthM += densityAtHeight(height, inverseHM) * segmentLength;
	}
	const Vector3 tau = betaR * opticalDepthR + betaM * kMieAbsorptionScale * opticalDepthM;
	return (atmosphereTransmittanceToACEScg(exponentialAttenuation(tau)));
}

Color	Atmosphere::sampleDiffuseSkyRadiance(const Vector3& position) const
{
	if (!this->_diffuseSkyCache || Utilities::vectorLengthSquared(position) <= 1e-12)
		return (Color(0.0, 0.0, 0.0));
	std::lock_guard<std::mutex> lock(this->_diffuseSkyCache->mutex);
	if (
		this->_diffuseSkyCache->valid
		&& Utilities::vectorLengthSquared(
			this->_diffuseSkyCache->position - position
		) <= 1e-12
	)
		return (this->_diffuseSkyCache->radiance);

	// Equal-solid-angle Fibonacci quadrature over the local upper hemisphere.
	// Twelve directions resolve the broad Rayleigh/Mie sky field while keeping
	// this one-time control calculation negligible beside volume-cache creation.
	constexpr int sampleCount = 12;
	constexpr double goldenAngle = 2.39996322972865332223;
	const Vector3 up = Utilities::normalize(position);
	const ONB basis(up);
	Color radiance(0.0, 0.0, 0.0);
	for (int sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++)
	{
		const double z = (static_cast<double>(sampleIndex) + 0.5)
			/ static_cast<double>(sampleCount);
		const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
		const double phi = goldenAngle * static_cast<double>(sampleIndex);
		const Vector3 direction = basis.local(
			radius * std::cos(phi),
			radius * std::sin(phi),
			z
		);
		const Ray skyRay = Ray::fromNormalizedDirection(position, direction);
		radiance += this->sampleSegment(skyRay, T_MAX).inScattering;
	}
	radiance /= static_cast<double>(sampleCount);
	this->_diffuseSkyCache->position = position;
	this->_diffuseSkyCache->radiance = radiance;
	this->_diffuseSkyCache->valid = true;
	return (radiance);
}

// Returns the sky color for 'ray'
Color   Atmosphere::computeIncidentLight(const Ray& ray, HitRecord& hitRecord, double t_max) const
{
	planetaryHit(this->metersToSceneUnits(this->_atmosphereRadius), ray, hitRecord);
	return (sampleSegment(ray, t_max).inScattering);
}
