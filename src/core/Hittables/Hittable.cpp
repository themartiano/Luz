#include "Hittables/Hittable.hpp"
#include "Defaults.hpp"
#include "Utilities.hpp"
#include <cmath>

bool Hittable::hitAny(Ray& ray, double t_min, double t_max) const
{
	HitRecord hitRecord;

	return (this->hit(ray, hitRecord, t_min, t_max));
}

Color Hittable::shadowTransmittance(Ray& ray, double t_min, double t_max) const
{
	return (this->hitAny(ray, t_min, t_max)
		? Color(0.0, 0.0, 0.0)
		: Color(1.0, 1.0, 1.0));
}

bool Hittable::hitInterval(Ray& ray, double t_min, double t_max, double& t0, double& t1) const
{
	HitRecord firstHit, secondHit;

	if (!this->hit(ray, firstHit, t_min, t_max))
	{
		return (false);
	}
	if (!this->hit(ray, secondHit, firstHit.t0 + T_MIN, t_max))
	{
		return (false);
	}

	t0 = firstHit.t0;
	t1 = secondHit.t0;
	return (t0 < t1);
}

double Hittable::pdfValue(const Vector3& origin, const Vector3& vec) const
{
	return (0.0);
	(void)origin;
	(void)vec;
}

Vector3 Hittable::random(const Vector3& origin) const
{
	return (Vector3(1.0, 0.0, 0.0));
	(void)origin;
}

bool Hittable::sampleLight(const Vector3& origin, HittableLightSample& sample) const
{
	sample = HittableLightSample();
	sample.direction = this->random(origin);

	const double directionLengthSquared = Utilities::vectorLengthSquared(sample.direction);
	if (!std::isfinite(directionLengthSquared) || directionLengthSquared <= 0.0)
	{
		return (false);
	}

	Ray lightRay(origin, sample.direction);
	HitRecord hitRecord;
	if (!this->hit(lightRay, hitRecord, T_MIN, T_MAX))
	{
		return (false);
	}

	sample.pdf = this->pdfValue(origin, sample.direction);
	sample.tMax = hitRecord.t0;
	sample.material = hitRecord.material;
	if (
		sample.pdf <= 0.0
		|| sample.tMax <= T_MIN
		|| !std::isfinite(sample.pdf)
		|| !std::isfinite(sample.tMax)
	)
	{
		return (false);
	}

	sample.valid = true;
	return (true);
}

bool	Hittable::sampleEmission(HittableEmissionSample& sample) const
{
	sample = HittableEmissionSample();
	return (false);
}

double Hittable::lightSelectionWeight(void) const
{
	Material* material = this->getMaterial();
	if (!material)
	{
		return (0.0);
	}

	const double luminance = Utilities::luminance(material->emitted());
	if (!std::isfinite(luminance) || luminance <= 0.0)
	{
		return (0.0);
	}
	return (luminance);
}
