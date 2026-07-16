#pragma once

#include "Hittables/Hittable.hpp"

class	ConstantVolume : public Hittable
{
	public:
		ConstantVolume(void);
		ConstantVolume(std::shared_ptr<Hittable> boundary, std::shared_ptr<Material> phaseFunction, double density);
		virtual Material*	getMaterial(void) const override;
		virtual bool	hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const override;
		virtual bool	hitAny(Ray& ray, double t_min, double t_max) const override;
		virtual Color	shadowTransmittance(Ray& ray, double t_min, double t_max) const override;
		virtual bool	createBoundingBox(AABB& outputBoundingBox) const override;
		double	getDensity(void) const;

	private:
		bool	sampleScatteringDistance(Ray& ray, double t_min, double t_max, double& hitT) const;
		std::shared_ptr<Hittable>	_boundary;
		std::shared_ptr<Material>	_phaseFunction;
		double	_density;
		double	_negativeInverseDensity;
};
