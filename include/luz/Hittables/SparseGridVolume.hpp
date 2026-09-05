#pragma once

#include "Hittables/DensityVolume.hpp"
#include "Hittables/Hittable.hpp"
#include "SparseVolumeGrid.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct GridVolumeParameters
{
	std::string fileName;
	Vector3 position = Vector3(0.0, 0.0, 0.0);
	Vector3 rotationDegrees = Vector3(0.0, 0.0, 0.0);
	// Zero on all axes preserves the physical aspect and dimensions stored in
	// the grid. A scene may otherwise place the asset at any explicit size.
	Vector3 size = Vector3(0.0, 0.0, 0.0);
	double extinction = 1.0;
	double densityThreshold = 0.0;
	double densityGamma = 1.0;
	Color albedo = Color(0.999, 0.999, 0.998);
	double anisotropy = 0.82;
	double backscatter = -0.25;
	double forwardWeight = 0.92;
	double dropletSizeMicrons = 20.0;
	double multipleScatteringFalloff = 1.0;
	double multipleScatteringCompensation = 0.72;
	double metersPerUnit = 1.0;
	int shadowSamplesPerBrick = 4;
	double primaryDetail = 1.0;
	bool directionalCache = true;
};

// Authored heterogeneous medium backed by Luz's dependency-free sparse grid.
class SparseGridVolume : public Hittable, public DensityVolume
{
	public:
		explicit SparseGridVolume(const GridVolumeParameters& parameters);

		Material* getMaterial(void) const override;
		bool hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const override;
		bool hitAny(Ray& ray, double t_min, double t_max) const override;
		Color shadowTransmittance(Ray& ray, double t_min, double t_max) const override;
		bool createBoundingBox(AABB& outputBoundingBox) const override;

		bool integrationInterval(
			const Ray& ray,
			double t_min,
			double t_max,
			double& entryT,
			double& exitT
		) const override;
		double extinctionAt(const Vector3& position) const override;
		Color singleScatteringCoefficientAt(
			const Vector3& position,
			const Vector3& incidentDirection,
			const Vector3& scatteredDirection
		) const override;
		Color singleScatteringWithExtinction(
			const Vector3& position, const Vector3& incidentDirection,
			const Vector3& scatteredDirection, double extinction
		) const override;
		Color volumeAlbedo(void) const override;
		double volumeFeatureScale(void) const override;
		double multipleScatteringFalloff(void) const override;
		double multipleScatteringCompensation(void) const override;
		bool directionalOpticalDepth(
			const Vector3& position,
			const Vector3& direction,
			double& opticalDepth
		) const override;

		double densityAt(const Vector3& position) const;
		const GridVolumeParameters& getParameters(void) const;
		const SparseVolumeGrid& getGrid(void) const;

	private:
		struct BrickSegment
		{
			std::int32_t x = 0;
			std::int32_t y = 0;
			std::int32_t z = 0;
			double exitT = 0.0;
			float minimum = 0.0f;
			float maximum = 0.0f;
		};
		struct BrickTraversal
		{
			explicit BrickTraversal(const Ray& ray) : localRay(ray) {}

			Ray localRay;
			BrickSegment segment;
			std::array<double, 3> boundaryT = {0.0, 0.0, 0.0};
			std::array<int, 3> step = {0, 0, 0};
			bool initialized = false;
		};
		struct DirectionalTransmittanceCache
		{
			Vector3 direction;
			std::array<std::uint32_t, 3> cells = {0u, 0u, 0u};
			std::array<std::uint32_t, 3> points = {0u, 0u, 0u};
			std::vector<float> opticalDepth;
		};
		using DirectionalCacheList = std::vector<
			std::shared_ptr<const DirectionalTransmittanceCache>
		>;

		bool boundsInterval(const Ray& ray, double t_min, double t_max, double& entryT, double& exitT) const;
		bool segmentAtLocal(BrickTraversal& traversal, double t, double volumeExitT, BrickSegment& segment) const;
		bool sampleCollision(Ray& ray, double t_min, double t_max, double& hitT) const;
		bool stableFeatureCollision(
			const Ray& ray,
			double t_min,
			double t_max,
			double& hitT,
			double& featureDensity
		) const;
		Vector3 rotate(const Vector3& vector) const;
		Vector3 inverseRotate(const Vector3& vector) const;
		Vector3 worldToLocalPoint(const Vector3& position) const;
		Vector3 localToWorldPoint(const Vector3& position) const;
		Ray worldToLocalRay(const Ray& ray) const;
		Vector3 normalizedLocalPosition(const Vector3& position) const;
		Vector3 normalizedPosition(const Vector3& position) const;
		double densityAtLocal(const Vector3& position) const;
		double remapDensity(double density) const;
		void buildDensityRemap(void);
		const DirectionalTransmittanceCache* directionalCache(
			const Vector3& direction
		) const;
		std::shared_ptr<const DirectionalTransmittanceCache> buildDirectionalCache(
			const Vector3& direction
		) const;
		double integratedDirectionalOpticalDepth(const Ray& ray) const;
		double cachedDirectionalOpticalDepth(
			const DirectionalTransmittanceCache& cache,
			const Vector3& position
		) const;

		GridVolumeParameters _parameters;
		SparseVolumeGrid _grid;
		Vector3 _minimum;
		Vector3 _maximum;
		AABB _boundingBox;
		double _cosX = 1.0;
		double _sinX = 0.0;
		double _cosY = 1.0;
		double _sinY = 0.0;
		double _cosZ = 1.0;
		double _sinZ = 0.0;
		double _extinctionSceneUnits = 0.0;
		double _featureScale = 1.0;
		std::vector<float> _densityRemap;
		double _densityRemapScale = 0.0;
		double _densityRemapThreshold = 0.0;
		std::uint32_t _samplingStream = 0;
		std::shared_ptr<Material> _phaseFunction;
		mutable std::mutex _directionalCacheMutex;
		mutable std::shared_ptr<const DirectionalCacheList> _directionalCacheSnapshot;
};
