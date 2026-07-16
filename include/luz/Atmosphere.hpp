#pragma once

#include "Vector3.hpp"
#include "Ray/Ray.hpp"
#include "Color.hpp"
#include "Hittables/Hittable.hpp"
#include <memory>

struct	AtmosphereSample
{
	Color	inScattering;
	Color	transmittance;

	AtmosphereSample(void) :
		inScattering(0.0, 0.0, 0.0),
		transmittance(1.0, 1.0, 1.0)
	{}
};

class   Atmosphere
{
	public:
		Atmosphere(void);
		Atmosphere(double sunAngle, double earthRadius, double atmosphereRadius, double hR, double hM, int samples, int lightSamples, double starsBrightness);
		static const Vector3 betaR;
		static const Vector3 betaM;
		double  getEarthRadius(void) const;
		double  getAtmosphereRadius(void) const;
		double  getStarsBrightness(void) const;
		Vector3	getSunDirection(void) const;
		Color	getSunRadiance(void) const;
		double	getSunRadianceScale(void) const;
		double	getMetersPerUnit(void) const;
		double	sceneUnitsToMeters(double sceneUnits) const;
		double	metersToSceneUnits(double meters) const;
		Color   computeIncidentLight(const Ray& ray, HitRecord& hitRecord, double t_max) const;
		AtmosphereSample	sampleSegment(const Ray& ray, double t_max) const;
		Color	sampleTransmittance(const Ray& ray, double t_max) const;
		// Uniform-hemisphere mean radiance from the procedural sky. This is a
		// deterministic low-frequency control for high-order volume scattering,
		// not a replacement for sampled environment lighting.
		Color	sampleDiffuseSkyRadiance(const Vector3& position) const;
		double  getSunAngle(void) const;
		void	setSunAngle(double newAngle);
		void	setSunDirection(Vector3 sunDirection);
		void	setSunRadiance(Color sunRadiance);
		void	setSunRadianceScale(double sunRadianceScale);
		void	setMetersPerUnit(double metersPerUnit);
		void	setEarthRadius(double earthRadius);
		void	setAtmosphereRadius(double atmosphereRadius);
		void	setHR(double hR);
		void	setHM(double hM);
		void	setSamples(int samples);
		void	setLightSamples(int lightSamples);
		void	setStarsBrightness(double starsBrightness);

	private:
		Vector3 _sunDirection;
		Color	_sunRadiance;
		double	_sunRadianceScale;
		double  _sunAngle;
		double  _earthRadius;
		double  _atmosphereRadius;
		double  _hR; // Thickness of the atmosphere if density was uniform
		double  _hM; // The same as above but for Mie Scattering
		int	 _samples; // Samples per ray
		int	 _lightSamples; // Number of samples per ray sample
		double  _starsBrightness;
		double	_metersPerUnit;
		struct DiffuseSkyCache;
		mutable std::shared_ptr<DiffuseSkyCache> _diffuseSkyCache;
		void	updateSunDirectionVector(void);
		void	invalidateDiffuseSkyCache(void);
};

bool planetaryHit(double radius, const Ray& ray, HitRecord& hitRecord);
