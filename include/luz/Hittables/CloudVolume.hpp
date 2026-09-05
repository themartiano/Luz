#pragma once

#include "Hittables/DensityVolume.hpp"
#include "Hittables/Hittable.hpp"
#include <cstdint>
#include <array>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

enum class CloudType
{
	Cumulus,
	Stratocumulus,
	Stratus,
	Cirrus,
	Cumulonimbus
};

struct CloudParameters
{
	CloudType	type = CloudType::Cumulus;
	Vector3		position = Vector3(0.0, 0.0, 0.0);
	Vector3		size = Vector3(4000.0, 1800.0, 4000.0);
	double		coverage = 0.45;
	double		extinction = 0.012;
	Color		albedo = Color(0.999, 0.999, 0.998);
	double		anisotropy = 0.82;
	double		backscatter = -0.25;
	double		forwardWeight = 0.92;
	// Zero uses the dual-HG controls above. Values from 5 to 50 enable the
	// fitted HG+Draine water-droplet phase model.
	double		dropletSizeMicrons = 0.0;
	double		featureScale = 900.0;
	double		macroScale = 1.0;
	double		detail = 0.65;
	double		erosion = 0.45;
	double		puffiness = 0.82;
	double		towering = 0.72;
	double		dominance = 0.68;
	double		overhang = 0.42;
	double		fineDetail = 0.72;
	// Optional Hyperion-style depth approximation. One is unbiased; lower
	// values reduce effective extinction after each volume bounce.
	double		multipleScatteringFalloff = 1.0;
	// Strength of deterministic finite-order energy reconstruction used only
	// when multipleScatteringFalloff is below one. Zero disables it.
	double		multipleScatteringCompensation = 0.72;
	Vector3		shearDirection = Vector3(0.0, 0.0, 0.0);
	std::uint32_t	seed = 42;
	Vector3		offset = Vector3(0.0, 0.0, 0.0);
	int		detailOctaves = 3;
	int		maxTrackingSteps = 512;
	// Zero disables the approximate directional cache. At most four directions,
	// each with at most 1,048,576 float samples (16 MiB total).
	double directionalCacheResolution = 0.0;
	double primaryDetail = 1.0;
	bool localMajorants = true;
	double weatherVariation = 0.0;
	double baseVariation = 0.0;
	double		metersPerUnit = 1.0;
};

// Procedural heterogeneous cloud medium sampled with delta tracking.
// extinction is the density majorant in inverse meters; densityAt returns [0, 1].
class CloudVolume : public Hittable, public DensityVolume
{
	public:
		explicit CloudVolume(const CloudParameters& parameters);

		static CloudParameters	preset(CloudType type);
		static CloudType		parseType(const std::string& name);
		static const char*		typeName(CloudType type);

		Material*	getMaterial(void) const override;
		bool		hit(Ray& ray, HitRecord& hitRecord, double t_min, double t_max) const override;
		bool		hitAny(Ray& ray, double t_min, double t_max) const override;
		Color		shadowTransmittance(Ray& ray, double t_min, double t_max) const override;
		bool		createBoundingBox(AABB& outputBoundingBox) const override;

		double		densityAt(const Vector3& position) const;
		double densityMajorantAt(const Vector3& position) const;
		bool		integrationInterval(
			const Ray& ray,
			double t_min,
			double t_max,
			double& entryT,
			double& exitT
		) const override;
		double		extinctionAt(const Vector3& position) const override;
		Color		singleScatteringCoefficientAt(
			const Vector3& position,
			const Vector3& incidentDirection,
			const Vector3& scatteredDirection
		) const override;
		Color singleScatteringWithExtinction(
			const Vector3& position, const Vector3& incidentDirection,
			const Vector3& scatteredDirection, double extinction
		) const override;
		Color		volumeAlbedo(void) const override;
		double		volumeFeatureScale(void) const override;
		double		multipleScatteringFalloff(void) const override;
		double		multipleScatteringCompensation(void) const override;
		bool directionalOpticalDepth(const Vector3& position, const Vector3& direction,
			double& opticalDepth) const override;
		const CloudParameters&	getParameters(void) const;

	private:
		struct DirectionalTransmittanceCache
		{
			Vector3 direction;
			std::array<std::uint32_t, 3> cells{}, points{};
			std::vector<float> opticalDepth;
		};
		using DirectionalCacheList = std::vector<std::shared_ptr<const DirectionalTransmittanceCache>>;
		const DirectionalTransmittanceCache* directionalCache(const Vector3& direction) const;
		std::shared_ptr<const DirectionalTransmittanceCache> buildDirectionalCache(const Vector3& direction) const;
		double cachedDirectionalOpticalDepth(const DirectionalTransmittanceCache& cache,
			const Vector3& position) const;
		mutable std::mutex _directionalCacheMutex;
		mutable std::shared_ptr<const DirectionalCacheList> _directionalCacheSnapshot;

		struct Lobe
		{
			Vector3 center;
			Vector3 radius;
			double strength = 1.0;
		};

		bool		boundsInterval(const Ray& ray, double t_min, double t_max, double& entryT, double& exitT) const;
		bool		sampleCollision(Ray& ray, double t_min, double t_max, double& hitT) const;
		void		lobeGridCell(
			const Ray& ray,
			double t,
			double exitT,
			double boundaryEpsilonT,
			double& densityMajorant,
			double& cellExitT
		) const;
		bool		stableFeatureCollision(
			const Ray& ray,
			double t_min,
			double t_max,
			double& hitT,
			double& density
		) const;
		Vector3		featureNormalAt(const Vector3& position) const;
		void		buildConvectiveLobes(void);
		void		buildLobeGrid(void);
		void buildDensityMajorants(void);
		double		convectiveLobeField(const Vector3& position) const;
		double		verticalProfile(double height, double growthNoise, double coverageMask) const;
		double		cellularPuffs(const Vector3& position, double scale, std::uint32_t salt) const;
		double		gradientNoise(const Vector3& position, std::uint32_t salt) const;
		double		fbm(const Vector3& position, int octaves, std::uint32_t salt) const;

		CloudParameters	_parameters;
		std::uint32_t	_samplingStream;
		Vector3		_minimum;
		Vector3		_maximum;
		double		_majorantSceneUnits;
		std::shared_ptr<Material>	_phaseFunction;
		std::vector<Lobe>	_lobes;
		std::vector<std::vector<std::uint32_t>>	_lobeGrid;
		std::vector<double> _densityMajorants;
		int	_lobeGridX = 0;
		int	_lobeGridY = 0;
		int	_lobeGridZ = 0;
};
