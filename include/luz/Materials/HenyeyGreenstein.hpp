#pragma once

#include "Materials/Material.hpp"

class	HenyeyGreenstein : public Material
{
	public:
		HenyeyGreenstein(void);
		HenyeyGreenstein(Color color, double anisotropy);
		HenyeyGreenstein(Color color, double anisotropy, double secondaryAnisotropy, double primaryWeight);
		void	setAnisotropy(double anisotropy);
		double	getAnisotropy(void) const;
		void	setSecondaryLobe(double secondaryAnisotropy, double primaryWeight);
		void	setDropletPhase(double dropletSizeMicrons);
		void	setDepthAnisotropyReduction(bool enabled);
		bool	usesDropletPhase(void) const;
		double	getDropletSizeMicrons(void) const;
		double	getSecondaryAnisotropy(void) const;
		double	getPrimaryWeight(void) const;
		bool	scatter(Ray& ray, HitRecord& hitRecord, ScatterRecord& scatterRecord);
		double	scatteringPDF(
			const Ray& ray,
			const HitRecord& hitRecord,
			const Vector3& scatteredDirection
		) const;
		MaterialType	getType(void) const;

	private:
		double	_anisotropy;
		double	_secondaryAnisotropy;
		double	_primaryWeight;
		bool	_useDraine;
		double	_draineAlpha;
		double	_draineWeight;
		double	_dropletSizeMicrons;
		bool	_reduceAnisotropyWithDepth;
};
