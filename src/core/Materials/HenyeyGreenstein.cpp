#include "Materials/HenyeyGreenstein.hpp"
#include "Defaults.hpp"
#include "Sampler.hpp"
#include "Utilities.hpp"
#include <cmath>
#include <stdexcept>

namespace
{
	constexpr double	MAX_ABS_ANISOTROPY = 0.99;

	double	sanitizedAnisotropy(double anisotropy)
	{
		if (!std::isfinite(anisotropy) || anisotropy < -MAX_ABS_ANISOTROPY || anisotropy > MAX_ABS_ANISOTROPY)
		{
			throw std::invalid_argument("Volume anisotropy must be finite and between -0.99 and 0.99.");
		}
		return (anisotropy);
	}

	double sanitizedPrimaryWeight(double primaryWeight)
	{
		if (!std::isfinite(primaryWeight) || primaryWeight < 0.0 || primaryWeight > 1.0)
			throw std::invalid_argument("Volume phase primary weight must be between zero and one.");
		return (primaryWeight);
	}

	double henyeyGreensteinPDF(double anisotropy, double cosTheta)
	{
		const double g2 = anisotropy * anisotropy;
		const double denominator = std::max(1e-12, 1.0 + g2 - 2.0 * anisotropy * cosTheta);
		return ((1.0 - g2) / (4.0 * D_PI * denominator * std::sqrt(denominator)));
	}

	double drainePDF(double anisotropy, double alpha, double cosTheta)
	{
		const double g2 = anisotropy * anisotropy;
		const double denominator = std::max(1e-12, 1.0 + g2 - 2.0 * anisotropy * cosTheta);
		const double normalization = 1.0 + alpha * (1.0 + 2.0 * g2) / 3.0;
		return ((1.0 - g2) * (1.0 + alpha * cosTheta * cosTheta)
			/ (4.0 * D_PI * normalization * denominator * std::sqrt(denominator)));
	}

	Vector3	phaseDirectionOrFallback(const Ray& ray)
	{
		const Vector3 direction = ray.getDirection();

		if (Utilities::vectorLengthSquared(direction) <= 0.0)
		{
			return (Vector3(0.0, 0.0, 1.0));
		}
		return (Utilities::normalize(direction));
	}

	double depthReducedAnisotropy(double anisotropy, bool enabled)
	{
		if (!enabled)
			return (anisotropy);
		const double magnitude = std::pow(
			std::fabs(anisotropy),
			1.0 + static_cast<double>(Sampler::currentBounce())
		);
		return (std::copysign(magnitude, anisotropy));
	}
}

HenyeyGreenstein::HenyeyGreenstein(void)
{
	this->_color = Color(0.6, 0.6, 0.6);
	this->_anisotropy = 0.0;
	this->_secondaryAnisotropy = 0.0;
	this->_primaryWeight = 1.0;
	this->_useDraine = false;
	this->_draineAlpha = 0.0;
	this->_draineWeight = 0.0;
	this->_dropletSizeMicrons = 0.0;
	this->_reduceAnisotropyWithDepth = false;
}

HenyeyGreenstein::HenyeyGreenstein(Color color, double anisotropy)
{
	this->_color = color;
	this->_anisotropy = sanitizedAnisotropy(anisotropy);
	this->_secondaryAnisotropy = this->_anisotropy;
	this->_primaryWeight = 1.0;
	this->_useDraine = false;
	this->_draineAlpha = 0.0;
	this->_draineWeight = 0.0;
	this->_dropletSizeMicrons = 0.0;
	this->_reduceAnisotropyWithDepth = false;
}

HenyeyGreenstein::HenyeyGreenstein(Color color, double anisotropy, double secondaryAnisotropy, double primaryWeight)
{
	this->_color = color;
	this->_anisotropy = sanitizedAnisotropy(anisotropy);
	this->_secondaryAnisotropy = sanitizedAnisotropy(secondaryAnisotropy);
	this->_primaryWeight = sanitizedPrimaryWeight(primaryWeight);
	this->_useDraine = false;
	this->_draineAlpha = 0.0;
	this->_draineWeight = 0.0;
	this->_dropletSizeMicrons = 0.0;
	this->_reduceAnisotropyWithDepth = false;
}

void	HenyeyGreenstein::setAnisotropy(double anisotropy)
{
	this->_anisotropy = sanitizedAnisotropy(anisotropy);
}

double	HenyeyGreenstein::getAnisotropy(void) const
{
	return (this->_anisotropy);
}

void HenyeyGreenstein::setSecondaryLobe(double secondaryAnisotropy, double primaryWeight)
{
	this->_secondaryAnisotropy = sanitizedAnisotropy(secondaryAnisotropy);
	this->_primaryWeight = sanitizedPrimaryWeight(primaryWeight);
	this->_useDraine = false;
	this->_draineAlpha = 0.0;
	this->_draineWeight = 0.0;
	this->_dropletSizeMicrons = 0.0;
}

void HenyeyGreenstein::setDropletPhase(double dropletSizeMicrons)
{
	if (!std::isfinite(dropletSizeMicrons) || dropletSizeMicrons < 5.0 || dropletSizeMicrons > 50.0)
		throw std::invalid_argument("Cloud droplet size must be between 5 and 50 microns.");
	const double d = dropletSizeMicrons;
	this->_anisotropy = std::exp(-(0.0990567 / (d - 1.67154)));
	this->_secondaryAnisotropy = std::exp(-(2.20679 / (d + 3.91029)) - 0.428934);
	this->_draineAlpha = std::exp(3.62489 - (8.29288 / (d + 5.52825)));
	this->_draineWeight = std::exp(-(0.599085 / (d - 0.641583)) - 0.665888);
	// The proposal samples the two HG kernels. The physical evaluation replaces
	// the secondary kernel with Draine and is corrected by f / proposal.
	this->_primaryWeight = 1.0 - this->_draineWeight;
	this->_useDraine = true;
	this->_dropletSizeMicrons = dropletSizeMicrons;
}

bool HenyeyGreenstein::usesDropletPhase(void) const
{
	return (this->_useDraine);
}

double HenyeyGreenstein::getDropletSizeMicrons(void) const
{
	return (this->_dropletSizeMicrons);
}

void HenyeyGreenstein::setDepthAnisotropyReduction(bool enabled)
{
	this->_reduceAnisotropyWithDepth = enabled;
}

double HenyeyGreenstein::getSecondaryAnisotropy(void) const
{
	return (this->_secondaryAnisotropy);
}

double HenyeyGreenstein::getPrimaryWeight(void) const
{
	return (this->_primaryWeight);
}

bool	HenyeyGreenstein::scatter(Ray& ray, HitRecord& hitRecord, ScatterRecord& scatterRecord)
{
	scatterRecord.isSpecular = false;
	scatterRecord.attenuation = this->colorAt(hitRecord);
	scatterRecord.pdfType = SCATTER_PDF_HENYEY_GREENSTEIN;
	scatterRecord.phaseDirection = phaseDirectionOrFallback(ray);
	scatterRecord.phaseAnisotropy = depthReducedAnisotropy(
		this->_anisotropy,
		this->_reduceAnisotropyWithDepth
	);
	scatterRecord.phaseSecondaryAnisotropy = depthReducedAnisotropy(
		this->_secondaryAnisotropy,
		this->_reduceAnisotropyWithDepth
	);
	scatterRecord.phasePrimaryWeight = this->_primaryWeight;
	scatterRecord.phaseUsesDraine = this->_useDraine;
	scatterRecord.phaseDraineAlpha = this->_draineAlpha;
	scatterRecord.phaseDraineWeight = this->_draineWeight;

	return (true);
}

double	HenyeyGreenstein::scatteringPDF(
	const Ray& ray,
	const HitRecord& hitRecord,
	const Vector3& scatteredDirection
	) const
{
	(void)hitRecord;
	if (Utilities::vectorLengthSquared(ray.getDirection()) <= 0.0 || Utilities::vectorLengthSquared(scatteredDirection) <= 0.0)
		return (0.0);
	const double cosTheta = std::clamp(Utilities::dot(
		Utilities::normalize(ray.getDirection()),
		Utilities::normalize(scatteredDirection)
	), -1.0, 1.0);
	const double primaryAnisotropy = depthReducedAnisotropy(
		this->_anisotropy,
		this->_reduceAnisotropyWithDepth
	);
	const double secondaryAnisotropy = depthReducedAnisotropy(
		this->_secondaryAnisotropy,
		this->_reduceAnisotropyWithDepth
	);
	if (this->_useDraine)
	{
		return (
			(1.0 - this->_draineWeight) * henyeyGreensteinPDF(primaryAnisotropy, cosTheta)
			+ this->_draineWeight * drainePDF(secondaryAnisotropy, this->_draineAlpha, cosTheta)
		);
	}
	return (this->_primaryWeight * henyeyGreensteinPDF(primaryAnisotropy, cosTheta)
		+ (1.0 - this->_primaryWeight) * henyeyGreensteinPDF(secondaryAnisotropy, cosTheta));
}

MaterialType	HenyeyGreenstein::getType(void) const
{
	return (HENYEY_GREENSTEIN);
}
