#pragma once

#include "Color.hpp"
#include "Ray/Ray.hpp"
#include "MaterialTypes.hpp"
#include "ONB.hpp"
#include "Hittables/Hittable.hpp"
#include "Texture.hpp"
#include <memory>

struct	HitRecord;
class	Material;

enum	ScatterPDFType
{
	SCATTER_PDF_NONE,
	SCATTER_PDF_COSINE,
	SCATTER_PDF_SPHERE,
	SCATTER_PDF_HENYEY_GREENSTEIN,
	SCATTER_PDF_BSDF
};

struct	ScatterRecord
{
	Ray	specularRay;
	Ray	incidentRay;
	bool	isSpecular = false;
	Color	attenuation;
	ScatterPDFType	pdfType = SCATTER_PDF_NONE;
	const Material*	bsdfMaterial = nullptr;
	Vector3	sampledDirection;
	double	sampledPDF = 0.0;
	ONB	cosineBasis;
	Vector3	phaseDirection;
	double	phaseAnisotropy = 0.0;
	double	phaseSecondaryAnisotropy = 0.0;
	double	phasePrimaryWeight = 1.0;
	bool	phaseUsesDraine = false;
	double	phaseDraineAlpha = 0.0;
	double	phaseDraineWeight = 0.0;
	bool	hasMediumAbsorption = false;
	Color	mediumAbsorptionCoefficient = Color(0.0, 0.0, 0.0);
	bool	hasSubsurface = false;
	bool	subsurfaceThin = false;
	Color	subsurfaceRadiusMeters = Color(0.0, 0.0, 0.0);
};

class	Material
{
	public:
		Material(void);
		Material(Color color);
		virtual ~Material(void) = default;
		virtual Color	getColor(void) const;
		virtual void	setColor(Color color);
		virtual Color	colorAt(const HitRecord& hitRecord) const;
		virtual void	setTexture(std::shared_ptr<Texture> texture);
		virtual bool	scatter(Ray& ray, HitRecord& hitRecord, ScatterRecord& scatterRecord);
		virtual Color	emitted(void);
		virtual Color	evaluateBSDFCos(
			const Ray& ray,
			const HitRecord& hitRecord,
			const Vector3& scatteredDirection
		) const;
		virtual double	scatteringPDF(
			const Ray& ray,
			const HitRecord& hitRecord,
			const Vector3& scatteredDirection
		) const;
		virtual MaterialType	getType(void) const;

	protected:
		Color	_color;
		std::shared_ptr<Texture>	_texture;
};
