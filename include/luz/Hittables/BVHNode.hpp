#pragma once

#include "Hittables/Hittable.hpp"
#include "AABB.hpp"
#include "Materials/Material.hpp"
#include <vector>
#include <memory>

class   BVHNode : public Hittable
{
	public:
		BVHNode(void) = default;
		BVHNode(std::vector<std::shared_ptr<Hittable>> hittables);
		virtual ~BVHNode(void) = default;
		virtual bool	hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const override;
		virtual bool	hitAny(Ray& ray, double t_min, double t_max) const override;
		virtual Color	shadowTransmittance(Ray& ray, double t_min, double t_max) const override;
		virtual bool	createBoundingBox(AABB& outputBoundingBox) const override;
		virtual Material* getMaterial(void) const override;

	private:
		BVHNode(std::vector<std::shared_ptr<Hittable>>& hittables, size_t start, size_t end, bool isRoot);

		std::vector<std::shared_ptr<Hittable>>	_childs;
		std::vector<AABB>				_childBoundingBoxes;
		AABB						_boundingBox;
};
