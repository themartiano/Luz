#pragma once

#include "AABB.hpp"
#include "Materials/Material.hpp"
#include "Ray/Ray.hpp"
#include "Vector3.hpp"
#include <memory>

class	AABB;
class	Material;

struct	HitRecord
{
	double		t0 = 0.0;
	double		t1 = 0.0;
	Vector3		position;
	Vector3		normal;
	Vector3		geometricNormal;
	bool		frontFace = true;
	double		u = 0.0;
	double		v = 0.0;
	Material*	material = nullptr;

	void	setFaceNormal(const Ray& ray, const Vector3& outwardNormal)
	{
		const Vector3& direction = ray.getDirection();

		this->frontFace = (
			(direction.getX() * outwardNormal.getX())
			+ (direction.getY() * outwardNormal.getY())
			+ (direction.getZ() * outwardNormal.getZ())
		) < 0.0;
		this->normal = this->frontFace ? outwardNormal : outwardNormal * -1.0;
		this->geometricNormal = this->normal;
	}
};

struct	HittableLightSample
{
	Vector3		direction;
	Color		emitted = Color(0.0, 0.0, 0.0);
	double		pdf = 0.0;
	double		tMax = 0.0;
	Material*	material = nullptr;
	bool		hasEmitted = false;
	bool		valid = false;
};

struct	HittableEmissionSample
{
	Vector3		position;
	Vector3		normal;
	Vector3		direction;
	Color		emitted = Color(0.0, 0.0, 0.0);
	double		powerScale = 0.0;
	bool		valid = false;
};

class   Hittable
{
	public:
		virtual ~Hittable(void) = default;
		virtual bool		hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const = 0;
		virtual bool		hitAny(Ray& ray, double t_min, double t_max) const;
		virtual Color	shadowTransmittance(Ray& ray, double t_min, double t_max) const;
		virtual bool		hitInterval(Ray& ray, double t_min, double t_max, double& t0, double& t1) const;
		virtual bool		createBoundingBox(AABB& outputBoundingBox) const = 0;
		virtual Material*	getMaterial(void) const = 0;
		virtual double pdfValue(const Vector3& origin, const Vector3& vec) const;
		virtual Vector3 random(const Vector3& origin) const;
		virtual bool sampleLight(const Vector3& origin, HittableLightSample& sample) const;
		virtual bool sampleEmission(HittableEmissionSample& sample) const;
		virtual double lightSelectionWeight(void) const;
};
