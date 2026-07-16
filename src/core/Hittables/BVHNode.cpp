#include "Hittables/BVHNode.hpp"
#include "Utilities.hpp"
#include "Defaults.hpp"
#include "Materials/Lambertian.hpp"
#include "Random.hpp"
#include <algorithm>
#include <array>

namespace
{
	constexpr std::size_t	BVH_LEAF_CHILD_COUNT = 8;
	constexpr std::size_t	BVH_ROOT_LEAF_CHILD_COUNT = 16;

	struct	ChildTraversal
	{
		std::size_t	index;
		double		near;
	};

	bool	boxHitDistance(const AABB& box, Ray& ray, double tMax, double& tNear)
	{
		double tMin = T_MIN;
		const Vector3& minimum = box.getMinimum();
		const Vector3& maximum = box.getMaximum();
		const Vector3& origin = ray.getOrigin();
		const Vector3& direction = ray.getDirection();
		const Vector3& inverseDirection = ray.getInverseDirection();

		if (direction[0] == 0.0 && direction[1] == 0.0 && direction[2] == 0.0)
		{
			return (false);
		}
		for (int axis = 0; axis < 3; axis++)
		{
			const double invD = inverseDirection[axis];
			if (invD == 0.0)
			{
				if (origin[axis] < minimum[axis] || origin[axis] > maximum[axis])
				{
					return (false);
				}
				continue;
			}
			double t0 = (minimum[axis] - origin[axis]) * invD;
			double t1 = (maximum[axis] - origin[axis]) * invD;
			if (invD < 0.0)
			{
				std::swap(t0, t1);
			}

			tMin = t0 > tMin ? t0 : tMin;
			tMax = t1 < tMax ? t1 : tMax;
			if (tMax <= tMin)
			{
				return (false);
			}
		}

		tNear = tMin;
		return (true);
	}

	void	insertNearFirst(
		std::array<ChildTraversal, BVH_LEAF_CHILD_COUNT>& children,
		std::size_t& count,
		ChildTraversal child
	)
	{
		std::size_t index = count;

		while (index > 0 && children[index - 1].near > child.near)
		{
			children[index] = children[index - 1];
			index--;
		}
		children[index] = child;
		count++;
	}

	Material*	debugBoundingBoxMaterial(void)
	{
		static Lambertian material;

		return (&material);
	}

	AABB	boundingBoxForRange(
		const std::vector<std::shared_ptr<Hittable>>& hittables,
		size_t start,
		size_t end
	)
	{
		AABB result;
		bool hasBox = false;

		for (size_t i = start; i < end; i++)
		{
			AABB childBox;

			if (!hittables[i]->createBoundingBox(childBox))
			{
				continue;
			}
			result = hasBox ? Utilities::mergeBoundingBoxes(result, childBox) : childBox;
			hasBox = true;
		}
		return (result);
	}

	unsigned int	largestExtentAxis(const AABB& boundingBox)
	{
		const Vector3 extent = boundingBox.getMaximum() - boundingBox.getMinimum();

		if (extent.getX() >= extent.getY() && extent.getX() >= extent.getZ())
		{
			return (0);
		}
		if (extent.getY() >= extent.getZ())
		{
			return (1);
		}
		return (2);
	}
}

// Static function prototypes
static bool	boxXCompare(const std::shared_ptr<Hittable>& hittable1, const std::shared_ptr<Hittable>& hittable2);
static bool	boxYCompare(const std::shared_ptr<Hittable>& hittable1, const std::shared_ptr<Hittable>& hittable2);
static bool	boxZCompare(const std::shared_ptr<Hittable>& hittable1, const std::shared_ptr<Hittable>& hittable2);
static bool	boxCompare(const std::shared_ptr<Hittable>& hittable1, const std::shared_ptr<Hittable>& hittable2, int axis);

/*
	Constructors
*/

// Constructor overload, only calls the actual constructor
BVHNode::BVHNode(std::vector<std::shared_ptr<Hittable>> hittables)
	: BVHNode(hittables, 0, hittables.size(), true)
{}

// Constructs the BVHNode
BVHNode::BVHNode(std::vector<std::shared_ptr<Hittable>>& hittables, size_t start, size_t end, bool isRoot)
{
	const AABB rangeBoundingBox = boundingBoxForRange(hittables, start, end);
	unsigned int axis = largestExtentAxis(rangeBoundingBox);
	auto comparator = (axis == 0) ? boxXCompare : (axis == 1) ? boxYCompare : boxZCompare;

	size_t	hittableCount = end - start;
	this->_childs.reserve(hittableCount);

	if (
		hittableCount <= BVH_LEAF_CHILD_COUNT
		|| (isRoot && hittableCount <= BVH_ROOT_LEAF_CHILD_COUNT)
	)
	{
		for (size_t i = start; i < end; i++)
		{
			this->_childs.push_back(hittables[i]);
		}
	}
	else
	{
		std::sort(hittables.begin() + start, hittables.begin() + end, comparator);

		auto mid = start + hittableCount / 2;
		this->_childs.push_back(std::shared_ptr<Hittable>(new BVHNode(hittables, start, mid, false)));
		this->_childs.push_back(std::shared_ptr<Hittable>(new BVHNode(hittables, mid, end, false)));
	}

	this->_childBoundingBoxes.clear();
	this->_childBoundingBoxes.reserve(this->_childs.size());

	for (auto& child : this->_childs)
	{
		AABB	childBoundingBox;
		if (!child->createBoundingBox(childBoundingBox))
		{
			//std::cerr << "Error: BVHNode::BVHNode() - createBoundingBox() failed" << std::endl;
			return;
		}
		this->_childBoundingBoxes.push_back(childBoundingBox);
	}

	this->_boundingBox = Utilities::mergeBoundingBoxes(this->_childBoundingBoxes);
}

// Comparator for the BVH box at the X axis
static bool	boxXCompare(const std::shared_ptr<Hittable>& hittable1, const std::shared_ptr<Hittable>& hittable2)
{
	return (boxCompare(hittable1, hittable2, 0));
}

// Comparator for the BVH box at the Y axis
static bool	boxYCompare(const std::shared_ptr<Hittable>& hittable1, const std::shared_ptr<Hittable>& hittable2)
{
	return (boxCompare(hittable1, hittable2, 1));
}

// Comparator for the BVH box at the Z axis
static bool	boxZCompare(const std::shared_ptr<Hittable>& hittable1, const std::shared_ptr<Hittable>& hittable2)
{
	return (boxCompare(hittable1, hittable2, 2));
}

// Compares 'hittable1' and 'hittable2'
static bool	boxCompare(const std::shared_ptr<Hittable>& hittable1, const std::shared_ptr<Hittable>& hittable2, int axis)
{
	AABB	boxA;
	AABB	boxB;

	if (hittable1->createBoundingBox(boxA) && hittable2->createBoundingBox(boxB))
	{
		return (boxA.getMinimum()[axis] < boxB.getMinimum()[axis]);
	}

	// Error
	return (false);
}

// Checks if the BVH Node is hit, then checks if the left or right nodes are hit
bool	BVHNode::hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const
{
	if (this->_childBoundingBoxes.size() != this->_childs.size())
	{
		return (false);
	}
	if (!this->_boundingBox.hit(ray, hitRecord, t_max))
	{
		return (false);
	}

	if (RENDER_AABB)
	{
		hitRecord.material = debugBoundingBoxMaterial();

		return (true);
	}
	else
	{
		bool hitAnything = false;
		double closestHit = t_max;

		if (this->_childs.size() != 2 && this->_childs.size() <= 3)
		{
			for (const auto& child : this->_childs)
			{
				HitRecord childHitRecord;
				if (child->hit(ray, childHitRecord, t_min, closestHit))
				{
					hitRecord = childHitRecord;
					closestHit = childHitRecord.t0;
					hitAnything = true;
				}
			}
			return (hitAnything);
		}

		if (this->_childs.size() == 2)
		{
			double firstNear = 0.0;
			double secondNear = 0.0;
			const bool hitFirstBox = boxHitDistance(this->_childBoundingBoxes[0], ray, closestHit, firstNear);
			const bool hitSecondBox = boxHitDistance(this->_childBoundingBoxes[1], ray, closestHit, secondNear);
			std::size_t firstIndex = 0;
			std::size_t secondIndex = 1;

			if (hitFirstBox && hitSecondBox && secondNear < firstNear)
			{
				std::swap(firstNear, secondNear);
				std::swap(firstIndex, secondIndex);
			}
			if ((firstIndex == 0 ? hitFirstBox : hitSecondBox))
			{
				HitRecord childHitRecord;
				if (this->_childs[firstIndex]->hit(ray, childHitRecord, t_min, closestHit))
				{
					hitRecord = childHitRecord;
					closestHit = childHitRecord.t0;
					hitAnything = true;
				}
			}
			if ((secondIndex == 0 ? hitFirstBox : hitSecondBox) && secondNear < closestHit)
			{
				HitRecord childHitRecord;
				if (this->_childs[secondIndex]->hit(ray, childHitRecord, t_min, closestHit))
				{
					hitRecord = childHitRecord;
					hitAnything = true;
				}
			}
			return (hitAnything);
		}

		std::array<ChildTraversal, BVH_LEAF_CHILD_COUNT> orderedChildren;
		std::size_t orderedCount = 0;

		for (std::size_t i = 0; i < this->_childs.size(); i++)
		{
			double childNear = 0.0;
			if (boxHitDistance(this->_childBoundingBoxes[i], ray, closestHit, childNear))
			{
				insertNearFirst(orderedChildren, orderedCount, {i, childNear});
			}
		}

		for (std::size_t i = 0; i < orderedCount; i++)
		{
			const ChildTraversal& child = orderedChildren[i];
			if (child.near >= closestHit)
			{
				continue;
			}
			HitRecord childHitRecord;
			if (this->_childs[child.index]->hit(ray, childHitRecord, t_min, closestHit))
			{
				hitRecord = childHitRecord;
				closestHit = childHitRecord.t0;
				hitAnything = true;
			}
		}

		return (hitAnything);
	}
}

bool	BVHNode::hitAny(Ray& ray, double t_min, double t_max) const
{
	HitRecord boundingHitRecord;

	if (this->_childBoundingBoxes.size() != this->_childs.size())
	{
		return (false);
	}
	if (!this->_boundingBox.hit(ray, boundingHitRecord, t_max))
	{
		return (false);
	}

	if (RENDER_AABB)
	{
		return (true);
	}

	if (this->_childs.size() != 2 && this->_childs.size() <= 3)
	{
		for (const auto& child : this->_childs)
		{
			if (child->hitAny(ray, t_min, t_max))
			{
				return (true);
			}
		}
		return (false);
	}

	if (this->_childs.size() == 2)
	{
		for (std::size_t i = 0; i < this->_childs.size(); i++)
		{
			double childNear = 0.0;
			if (boxHitDistance(this->_childBoundingBoxes[i], ray, t_max, childNear)
				&& this->_childs[i]->hitAny(ray, t_min, t_max))
			{
				return (true);
			}
		}
		return (false);
	}

	for (std::size_t i = 0; i < this->_childs.size(); i++)
	{
		double childNear = 0.0;
		if (boxHitDistance(this->_childBoundingBoxes[i], ray, t_max, childNear)
			&& this->_childs[i]->hitAny(ray, t_min, t_max))
		{
			return (true);
		}
	}
	return (false);
}

Color BVHNode::shadowTransmittance(Ray& ray, double t_min, double t_max) const
{
	HitRecord boundingHitRecord;
	if (
		this->_childBoundingBoxes.size() != this->_childs.size()
		|| !this->_boundingBox.hit(ray, boundingHitRecord, t_max)
	)
		return (Color(1.0, 1.0, 1.0));
	if (RENDER_AABB)
		return (Color(0.0, 0.0, 0.0));

	Color transmittance(1.0, 1.0, 1.0);
	for (std::size_t i = 0; i < this->_childs.size(); i++)
	{
		double childNear = 0.0;
		if (!boxHitDistance(this->_childBoundingBoxes[i], ray, t_max, childNear))
			continue;
		transmittance = transmittance * this->_childs[i]->shadowTransmittance(ray, t_min, t_max);
		if (
			transmittance.getRed() <= 1e-8
			&& transmittance.getGreen() <= 1e-8
			&& transmittance.getBlue() <= 1e-8
		)
			return (Color(0.0, 0.0, 0.0));
	}
	return (transmittance);
}

// Sets 'outputBoundingBox' to the BVH Node's '_boundingBox'
bool	BVHNode::createBoundingBox(AABB& outputBoundingBox) const
{
	outputBoundingBox = this->_boundingBox;
	return (true);
}

// Returns an empty Material (this function only exists because it must be implemented since the Hittable class has it)
Material*	BVHNode::getMaterial(void) const
{
	return (debugBoundingBoxMaterial());
}
