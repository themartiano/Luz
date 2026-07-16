#pragma once

#include "Color.hpp"
#include "Ray/Ray.hpp"
#include "Vector3.hpp"

// Shared transport interface for procedural and authored heterogeneous media.
// Implementations remain ordinary Hittables; this interface only exposes the
// continuous density field needed by deterministic primary-light integration.
class DensityVolume
{
	public:
		virtual ~DensityVolume(void) = default;

		virtual bool integrationInterval(
			const Ray& ray,
			double t_min,
			double t_max,
			double& entryT,
			double& exitT
		) const = 0;
		virtual double extinctionAt(const Vector3& position) const = 0;
		virtual Color singleScatteringCoefficientAt(
			const Vector3& position,
			const Vector3& incidentDirection,
			const Vector3& scatteredDirection
		) const = 0;
		virtual Color volumeAlbedo(void) const = 0;
		virtual double volumeFeatureScale(void) const = 0;
		virtual double multipleScatteringFalloff(void) const = 0;
		virtual double multipleScatteringCompensation(void) const = 0;
		// Returns a deterministic extinction optical depth from position toward
		// the directional source when the volume has a suitable cache. Procedural
		// media may decline and use the transmittance-only fallback.
		virtual bool directionalOpticalDepth(
			const Vector3& position,
			const Vector3& direction,
			double& opticalDepth
		) const
		{
			(void)position;
			(void)direction;
			(void)opticalDepth;
			return (false);
		}
};
