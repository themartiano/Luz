#include "RendererInternal.hpp"
#include "Renderer/CausticPhotonMap.hpp"
#include "Materials/Principled.hpp"
#include "Utilities.hpp"
#include "Defaults.hpp"
#include "SkyTypes.hpp"
#include "Sampler.hpp"
#include "ONB.hpp"
#include "Hittables/DirectionalLight.hpp"
#include "Hittables/DensityVolume.hpp"
#include "VolumeGuidingField.hpp"
#include <array>
#include <cmath>
#include <memory>
#include <vector>
#include <algorithm>
#include <cstdint>

namespace
{
	constexpr int		RUSSIAN_ROULETTE_START_BOUNCE = 3;
	constexpr int		VOLUME_RUSSIAN_ROULETTE_START_BOUNCE = 8;
	constexpr double	PATH_THROUGHPUT_EPSILON = 1e-6;
	constexpr double	RUSSIAN_ROULETTE_MIN_SURVIVAL = 0.05;
	constexpr double	RUSSIAN_ROULETTE_MAX_SURVIVAL = 0.95;
	constexpr double	VOLUME_RUSSIAN_ROULETTE_MAX_SURVIVAL = 0.99;
	constexpr int		DIRECT_LIGHT_FULL_SAMPLE_BOUNCES = 2;
	constexpr int		VOLUME_DIRECT_LIGHT_FULL_SAMPLE_BOUNCES = 12;
	constexpr double	DIRECT_LIGHT_LATE_SAMPLE_PROBABILITY = 0.5;
	constexpr double	VOLUME_SUN_GUIDE_PROBABILITY = 0.25;
	constexpr double	VOLUME_SKY_GUIDE_PROBABILITY = 0.25;
	constexpr std::size_t	ENVIRONMENT_DIFFUSE_DIRECTION_COUNT = 8;
	constexpr std::size_t	ENVIRONMENT_DIFFUSE_KNOT_COUNT = 4;
	constexpr double	MAX_PHASE_ANISOTROPY = 0.9999;
	const double		FULL_SPHERE_PDF = 1.0 / (4.0 * D_PI);
	const double		HEMISPHERE_PDF = 1.0 / (2.0 * D_PI);

	double henyeyGreensteinPDF(double anisotropy, double cosTheta)
	{
		const double g = std::clamp(anisotropy, -MAX_PHASE_ANISOTROPY, MAX_PHASE_ANISOTROPY);
		const double g2 = g * g;
		const double denominator = std::max(1e-12, 1.0 + g2 - (2.0 * g * cosTheta));
		return ((1.0 - g2) / (4.0 * D_PI * denominator * std::sqrt(denominator)));
	}

	double drainePDF(double anisotropy, double alpha, double cosTheta)
	{
		const double g = std::clamp(anisotropy, -MAX_PHASE_ANISOTROPY, MAX_PHASE_ANISOTROPY);
		const double g2 = g * g;
		const double denominator = std::max(1e-12, 1.0 + g2 - 2.0 * g * cosTheta);
		const double normalization = 1.0 + alpha * (1.0 + 2.0 * g2) / 3.0;
		return ((1.0 - g2) * (1.0 + alpha * cosTheta * cosTheta)
			/ (4.0 * D_PI * normalization * denominator * std::sqrt(denominator)));
	}

	struct	LightDistribution
	{
		const std::vector<double>*	cumulativeWeights = nullptr;
		double						totalWeight = 0.0;
	};

	struct	LightSample
	{
		Vector3	direction;
		Color	emitted;
		double	pdf = 0.0;
		double	tMax = 0.0;
		bool	valid = false;
		bool	directional = false;
	};

	struct	ScatterDirectionSample
	{
		Vector3	direction;
		double	physicalPDF = 0.0;
		double	proposalPDF = 0.0;
	};

	class	ScopedDirectionalShadowSampling
	{
		public:
			explicit ScopedDirectionalShadowSampling(bool enabled)
				: _previous(Sampler::isDirectionalShadowSampling())
			{
				if (enabled)
					Sampler::setDirectionalShadowSampling(true);
			}

			~ScopedDirectionalShadowSampling(void)
			{
				Sampler::setDirectionalShadowSampling(this->_previous);
			}

		private:
			bool	_previous;
	};

	double	scatterPDFValue(
		const ScatterRecord& scatterRecord,
		const HitRecord& hitRecord,
		const Vector3& direction
	)
	{
		switch (scatterRecord.pdfType)
		{
			case SCATTER_PDF_COSINE:
			{
				double cosine = Utilities::dot(direction, scatterRecord.cosineBasis.getW());

				return ((cosine <= 0.0) ? 0.0 : cosine / D_PI);
			}
			case SCATTER_PDF_SPHERE:
				return (1.0 / (4.0 * D_PI));
			case SCATTER_PDF_HENYEY_GREENSTEIN:
			{
				const double directionLengthSquared = Utilities::vectorLengthSquared(direction);
				const double phaseDirectionLengthSquared = Utilities::vectorLengthSquared(scatterRecord.phaseDirection);
				if (
					directionLengthSquared <= 0.0
					|| phaseDirectionLengthSquared <= 0.0
					|| !std::isfinite(directionLengthSquared)
					|| !std::isfinite(phaseDirectionLengthSquared)
				)
				{
					return (0.0);
				}

				const Vector3 normalizedDirection = direction / std::sqrt(directionLengthSquared);
				const Vector3 phaseDirection = scatterRecord.phaseDirection / std::sqrt(phaseDirectionLengthSquared);
				const double cosTheta = std::max(-1.0, std::min(1.0, Utilities::dot(normalizedDirection, phaseDirection)));
				const double primaryWeight = std::max(0.0, std::min(1.0, scatterRecord.phasePrimaryWeight));
				if (scatterRecord.phaseUsesDraine)
				{
					const double draineWeight = std::clamp(scatterRecord.phaseDraineWeight, 0.0, 1.0);
					return (
						(1.0 - draineWeight) * henyeyGreensteinPDF(scatterRecord.phaseAnisotropy, cosTheta)
						+ draineWeight * drainePDF(
							scatterRecord.phaseSecondaryAnisotropy,
							scatterRecord.phaseDraineAlpha,
							cosTheta
						)
					);
				}
				return (primaryWeight * henyeyGreensteinPDF(scatterRecord.phaseAnisotropy, cosTheta)
					+ (1.0 - primaryWeight) * henyeyGreensteinPDF(scatterRecord.phaseSecondaryAnisotropy, cosTheta));
			}
			case SCATTER_PDF_BSDF:
				if (!scatterRecord.bsdfMaterial)
				{
					return (0.0);
				}
				return (scatterRecord.bsdfMaterial->scatteringPDF(
					scatterRecord.incidentRay,
					hitRecord,
					direction
				));
			default:
				return (0.0);
		}
	}

	double scatterBaseProposalPDF(
		const ScatterRecord& scatterRecord,
		const HitRecord& hitRecord,
		const Vector3& direction
	)
	{
		if (scatterRecord.pdfType != SCATTER_PDF_HENYEY_GREENSTEIN || !scatterRecord.phaseUsesDraine)
			return (scatterPDFValue(scatterRecord, hitRecord, direction));
		const double directionLengthSquared = Utilities::vectorLengthSquared(direction);
		const double phaseDirectionLengthSquared = Utilities::vectorLengthSquared(scatterRecord.phaseDirection);
		if (directionLengthSquared <= 0.0 || phaseDirectionLengthSquared <= 0.0)
			return (0.0);
		const double cosTheta = std::clamp(Utilities::dot(
			direction / std::sqrt(directionLengthSquared),
			scatterRecord.phaseDirection / std::sqrt(phaseDirectionLengthSquared)
		), -1.0, 1.0);
		const double primaryWeight = std::clamp(scatterRecord.phasePrimaryWeight, 0.0, 1.0);
		return (primaryWeight * henyeyGreensteinPDF(scatterRecord.phaseAnisotropy, cosTheta)
			+ (1.0 - primaryWeight) * henyeyGreensteinPDF(scatterRecord.phaseSecondaryAnisotropy, cosTheta));
	}

	Color	scatterBSDFCos(
		const ScatterRecord& scatterRecord,
		const HitRecord& hitRecord,
		const Vector3& direction
	)
	{
		if (scatterRecord.pdfType == SCATTER_PDF_BSDF)
		{
			if (!scatterRecord.bsdfMaterial)
			{
				return (Color(0.0, 0.0, 0.0));
			}
			return (scatterRecord.bsdfMaterial->evaluateBSDFCos(
				scatterRecord.incidentRay,
				hitRecord,
				direction
			));
		}
		return (scatterRecord.attenuation * scatterPDFValue(scatterRecord, hitRecord, direction));
	}

	Vector3	henyeyGreensteinDirection(const ScatterRecord& scatterRecord)
	{
		const double phaseDirectionLengthSquared = Utilities::vectorLengthSquared(scatterRecord.phaseDirection);
		if (phaseDirectionLengthSquared <= 0.0 || !std::isfinite(phaseDirectionLengthSquared))
		{
			return (Sampler::sphereDirection(Sampler::DIM_BSDF_DIRECTION));
		}

		const Vector3 phaseDirection = scatterRecord.phaseDirection / std::sqrt(phaseDirectionLengthSquared);
		const double primaryWeight = std::max(0.0, std::min(1.0, scatterRecord.phasePrimaryWeight));
		const double sampledAnisotropy = Sampler::sample1D(Sampler::DIM_MATERIAL_DECISION) < primaryWeight
			? scatterRecord.phaseAnisotropy
			: scatterRecord.phaseSecondaryAnisotropy;
		const double g = std::clamp(sampledAnisotropy, -MAX_PHASE_ANISOTROPY, MAX_PHASE_ANISOTROPY);
		const Sampler::Sample2D sample = Sampler::sample2D(Sampler::DIM_BSDF_DIRECTION);
		const double phi = 2.0 * D_PI * sample.x;
		double cosTheta;

		if (std::fabs(g) < 1e-3)
		{
			cosTheta = 1.0 - (2.0 * sample.y);
		}
		else
		{
			const double remapped = (1.0 - (g * g)) / (1.0 - g + (2.0 * g * sample.y));
			cosTheta = (1.0 + (g * g) - (remapped * remapped)) / (2.0 * g);
			cosTheta = std::max(-1.0, std::min(1.0, cosTheta));
		}

		const double sinTheta = std::sqrt(std::max(0.0, 1.0 - (cosTheta * cosTheta)));
		const ONB phaseBasis(phaseDirection);

		return (phaseBasis.local(
			sinTheta * std::cos(phi),
			sinTheta * std::sin(phi),
			cosTheta
		));
	}

	Vector3	scatterPDFGenerate(const ScatterRecord& scatterRecord)
	{
		switch (scatterRecord.pdfType)
		{
			case SCATTER_PDF_COSINE:
				return (scatterRecord.cosineBasis.local(Sampler::cosineHemisphere(Sampler::DIM_BSDF_DIRECTION)));
			case SCATTER_PDF_SPHERE:
				return (Sampler::sphereDirection(Sampler::DIM_BSDF_DIRECTION));
			case SCATTER_PDF_HENYEY_GREENSTEIN:
				return (henyeyGreensteinDirection(scatterRecord));
			case SCATTER_PDF_BSDF:
				return (scatterRecord.sampledDirection);
			default:
				return (Vector3(0.0, 0.0, 0.0));
		}
	}

	bool	primaryDirectionalGuide(
		const std::vector<std::shared_ptr<Hittable>>& lights,
		Vector3& guideDirection
	)
	{
		const DirectionalLight* selectedLight = nullptr;
		double selectedWeight = 0.0;

		for (const std::shared_ptr<Hittable>& light : lights)
		{
			const DirectionalLight* directionalLight = dynamic_cast<const DirectionalLight*>(light.get());
			if (!directionalLight)
			{
				continue;
			}

			const double weight = directionalLight->lightSelectionWeight();
			if (!selectedLight || weight > selectedWeight)
			{
				selectedLight = directionalLight;
				selectedWeight = weight;
			}
		}
		if (!selectedLight)
		{
			return (false);
		}

		guideDirection = selectedLight->getDirection() * -1.0;
		const double lengthSquared = Utilities::vectorLengthSquared(guideDirection);
		if (lengthSquared <= 0.0 || !std::isfinite(lengthSquared))
		{
			return (false);
		}
		guideDirection = guideDirection / std::sqrt(lengthSquared);
		return (true);
	}

	double	scatterProposalPDF(
		const ScatterRecord& scatterRecord,
		const HitRecord& hitRecord,
		const Vector3& direction,
		bool hasSunGuide,
		const Vector3& sunGuideDirection,
		bool hasSkyGuide,
		const Vector3& skyGuideDirection,
		const VolumeGuidingField* learnedGuide,
		double learnedStrength
	)
	{
		const double physicalPDF = scatterBaseProposalPDF(scatterRecord, hitRecord, direction);
		const bool guidedVolume = scatterRecord.pdfType == SCATTER_PDF_HENYEY_GREENSTEIN
			|| scatterRecord.pdfType == SCATTER_PDF_SPHERE;
		if (!guidedVolume)
		{
			return (physicalPDF);
		}

		const bool analyticDirectionalGuides = scatterRecord.pdfType == SCATTER_PDF_HENYEY_GREENSTEIN;
		const double sunWeight = analyticDirectionalGuides && hasSunGuide
			? VOLUME_SUN_GUIDE_PROBABILITY : 0.0;
		const double skyWeight = analyticDirectionalGuides && hasSkyGuide
			? VOLUME_SKY_GUIDE_PROBABILITY : 0.0;
		double proposalPDF = (1.0 - sunWeight - skyWeight) * physicalPDF;
		if (hasSunGuide)
		{
			ScatterRecord sunRecord = scatterRecord;
			sunRecord.phaseDirection = sunGuideDirection;
			proposalPDF += sunWeight * scatterBaseProposalPDF(sunRecord, hitRecord, direction);
		}
		if (hasSkyGuide)
		{
			ScatterRecord skyRecord = scatterRecord;
			skyRecord.phaseDirection = skyGuideDirection;
			proposalPDF += skyWeight * scatterBaseProposalPDF(skyRecord, hitRecord, direction);
		}
		const bool hasLearnedGuide = learnedGuide != nullptr
			&& learnedGuide->isFrozen()
			&& learnedGuide->contains(hitRecord.position)
			&& learnedStrength > 0.0;
		if (hasLearnedGuide)
		{
			const double weight = std::clamp(learnedStrength, 0.0, 0.75);
			proposalPDF = (1.0 - weight) * proposalPDF
				+ weight * learnedGuide->pdf(hitRecord.position, direction);
		}
		return (proposalPDF);
	}

	ScatterDirectionSample	sampleScatterDirection(
		const ScatterRecord& scatterRecord,
		const HitRecord& hitRecord,
		bool hasSunGuide,
		const Vector3& sunGuideDirection,
		bool hasSkyGuide,
		const Vector3& skyGuideDirection,
		const VolumeGuidingField* learnedGuide,
		double learnedStrength
	)
	{
		ScatterDirectionSample sample;

		const bool guidedVolume = scatterRecord.pdfType == SCATTER_PDF_HENYEY_GREENSTEIN
			|| scatterRecord.pdfType == SCATTER_PDF_SPHERE;
		if (!guidedVolume)
		{
			sample.direction = scatterPDFGenerate(scatterRecord);
			sample.physicalPDF = scatterPDFValue(scatterRecord, hitRecord, sample.direction);
			sample.proposalPDF = scatterBaseProposalPDF(scatterRecord, hitRecord, sample.direction);
			return (sample);
		}

		const bool hasLearnedGuide = learnedGuide != nullptr
			&& learnedGuide->isFrozen()
			&& learnedGuide->contains(hitRecord.position)
			&& learnedStrength > 0.0;
		const double learnedWeight = hasLearnedGuide
			? std::clamp(learnedStrength, 0.0, 0.75)
			: 0.0;
		const bool analyticDirectionalGuides = scatterRecord.pdfType == SCATTER_PDF_HENYEY_GREENSTEIN;
		const double sunWeight = analyticDirectionalGuides && hasSunGuide
			? VOLUME_SUN_GUIDE_PROBABILITY : 0.0;
		const double skyWeight = analyticDirectionalGuides && hasSkyGuide
			? VOLUME_SKY_GUIDE_PROBABILITY : 0.0;
		const double guideChoice = Sampler::sample1D(Sampler::DIM_VOLUME_GUIDING);
		if (guideChoice < learnedWeight)
		{
			const VolumeGuidingField::Sample learnedSample = learnedGuide->sample(
				hitRecord.position,
				Sampler::sample1D(Sampler::DIM_VOLUME_LEARNED_LOBE),
				Sampler::sample2D(Sampler::DIM_VOLUME_LEARNED_DIRECTION)
			);
			if (learnedSample.valid)
				sample.direction = learnedSample.direction;
			else
				sample.direction = scatterPDFGenerate(scatterRecord);
		}
		else if (
			(guideChoice - learnedWeight) / std::max(1e-12, 1.0 - learnedWeight)
			< sunWeight
		)
		{
			ScatterRecord sunRecord = scatterRecord;
			sunRecord.phaseDirection = sunGuideDirection;
			sample.direction = henyeyGreensteinDirection(sunRecord);
		}
		else if (
			(guideChoice - learnedWeight) / std::max(1e-12, 1.0 - learnedWeight)
			< sunWeight + skyWeight
		)
		{
			ScatterRecord skyRecord = scatterRecord;
			skyRecord.phaseDirection = skyGuideDirection;
			sample.direction = henyeyGreensteinDirection(skyRecord);
		}
		else
			sample.direction = scatterPDFGenerate(scatterRecord);
		sample.physicalPDF = scatterPDFValue(scatterRecord, hitRecord, sample.direction);
		sample.proposalPDF = scatterProposalPDF(
			scatterRecord,
			hitRecord,
			sample.direction,
			hasSunGuide,
			sunGuideDirection,
			hasSkyGuide,
			skyGuideDirection,
			learnedGuide,
			learnedStrength
		);
		return (sample);
	}

	bool	hasWeightedLightDistribution(const LightDistribution& lightDistribution)
	{
		return (
			lightDistribution.cumulativeWeights != nullptr
			&& !lightDistribution.cumulativeWeights->empty()
			&& lightDistribution.totalWeight > 0.0
		);
	}

	double	lightSelectionProbability(
		const LightDistribution& lightDistribution,
		const std::vector<std::shared_ptr<Hittable>>& lights,
		std::size_t lightIndex
	)
	{
		if (lights.empty())
		{
			return (0.0);
		}
		if (!hasWeightedLightDistribution(lightDistribution))
		{
			return (1.0 / static_cast<double>(lights.size()));
		}

		const std::vector<double>& cumulativeWeights = *lightDistribution.cumulativeWeights;
		const double previousWeight = lightIndex == 0 ? 0.0 : cumulativeWeights[lightIndex - 1];
		const double weight = cumulativeWeights[lightIndex] - previousWeight;
		if (weight <= 0.0 || lightDistribution.totalWeight <= 0.0)
		{
			return (0.0);
		}
		return (weight / lightDistribution.totalWeight);
	}

	std::size_t	selectLightIndex(
		const LightDistribution& lightDistribution,
		const std::vector<std::shared_ptr<Hittable>>& lights
	)
	{
		if (lights.size() <= 1)
		{
			return (0);
		}
		if (hasWeightedLightDistribution(lightDistribution))
		{
			const std::vector<double>& cumulativeWeights = *lightDistribution.cumulativeWeights;
			const double target = Sampler::sample1D(Sampler::DIM_LIGHT_SELECTION) * lightDistribution.totalWeight;
			const auto weightIt = std::upper_bound(
				cumulativeWeights.begin(),
				cumulativeWeights.end(),
				target
			);

			return (std::min<std::size_t>(
				static_cast<std::size_t>(weightIt - cumulativeWeights.begin()),
				lights.size() - 1
			));
		}
		const std::size_t randomIndex = std::min<std::size_t>(
			static_cast<std::size_t>(Sampler::sample1D(Sampler::DIM_LIGHT_SELECTION) * lights.size()),
			lights.size() - 1
		);

		return (randomIndex);
	}

	double	lightPDFValue(
		const LightDistribution& lightDistribution,
		const std::vector<std::shared_ptr<Hittable>>& lights,
		const Vector3& origin,
		const Vector3& direction
	)
	{
		if (lights.empty())
		{
			return (0.0);
		}
		if (lights.size() == 1)
		{
			return (lights[0]->pdfValue(origin, direction));
		}

		double sum = 0.0;

		for (std::size_t i = 0; i < lights.size(); i++)
		{
			sum += lightSelectionProbability(lightDistribution, lights, i) * lights[i]->pdfValue(origin, direction);
		}

		return (sum);
	}

	Color	clampRayColor(Color color)
	{
		const double luminance = Utilities::luminance(color);

		if (!std::isfinite(luminance))
		{
			return (Color(0.0, 0.0, 0.0));
		}
		if (luminance > D_MAX_RAY_COLOR_LUMINANCE)
		{
			color = color * (D_MAX_RAY_COLOR_LUMINANCE / luminance);
		}

		return (color);
	}

	Color	clampColorLuminance(Color color, double maxLuminance)
	{
		const double luminance = Utilities::luminance(color);

		if (!std::isfinite(luminance))
		{
			return (Color(0.0, 0.0, 0.0));
		}
		if (maxLuminance > 0.0 && luminance > maxLuminance)
		{
			color = color * (maxLuminance / luminance);
		}
		return (color);
	}

	Color	clampConstantBackgroundMiss(Scene& scene, Color contribution)
	{
		constexpr double BACKGROUND_MISS_HEADROOM = 4.0;
		const double backgroundLuminance = Utilities::luminance(scene.getBackgroundColor());

		if (!std::isfinite(backgroundLuminance) || backgroundLuminance <= 0.0)
		{
			return (contribution);
		}
		return (clampColorLuminance(contribution, backgroundLuminance * BACKGROUND_MISS_HEADROOM));
	}

	double	maxChannel(const Color& color)
	{
		return (std::max(color.getRed(), std::max(color.getGreen(), color.getBlue())));
	}

	double	powerHeuristic(double firstPDF, double secondPDF)
	{
		const double first = firstPDF * firstPDF;
		const double second = secondPDF * secondPDF;
		const double sum = first + second;

		if (sum <= 0.0 || !std::isfinite(sum))
		{
			return (0.0);
		}
		return (first / sum);
	}

	bool	isTerminatedThroughput(const Color& throughput)
	{
		const double throughputMax = maxChannel(throughput);

		return (!std::isfinite(throughputMax) || throughputMax <= PATH_THROUGHPUT_EPSILON);
	}

	bool	materialCanTransmitThroughGeometry(const Material* material)
	{
		if (!material)
		{
			return (false);
		}
		const MaterialType type = material->getType();
		if (
			type == DIELECTRIC
			|| type == ISOTROPIC
			|| type == HENYEY_GREENSTEIN
		)
		{
			return (true);
		}
		if (type != PRINCIPLED)
		{
			return (false);
		}

		const Principled* principled = dynamic_cast<const Principled*>(material);
		return (
			principled != nullptr
			&& (
				principled->getTransmission() > 0.0
				|| principled->usesThinSubsurface()
			)
		);
	}

	bool	isVolumePhaseMaterial(const Material* material)
	{
		if (!material)
		{
			return (false);
		}
		const MaterialType type = material->getType();

		return (type == ISOTROPIC || type == HENYEY_GREENSTEIN);
	}

	double	nearestCameraSurfaceT(Scene& scene, const Ray& cameraRay)
	{
		double closestT = T_MAX;

		for (const std::shared_ptr<Hittable>& hittable : scene.getHittables())
		{
			if (
				!hittable
				|| dynamic_cast<const DensityVolume*>(hittable.get()) != nullptr
				|| isVolumePhaseMaterial(hittable->getMaterial())
			)
			{
				continue;
			}

			Ray surfaceRay = cameraRay;
			HitRecord surfaceHit;
			if (!hittable->hit(surfaceRay, surfaceHit, T_MIN, closestT))
			{
				continue;
			}
			if (
				isVolumePhaseMaterial(surfaceHit.material)
				|| !std::isfinite(surfaceHit.t0)
				|| surfaceHit.t0 <= T_MIN
			)
			{
				continue;
			}
			closestT = surfaceHit.t0;
		}
		return (closestT);
	}

	bool	leavesOpaqueGeometricSurface(const HitRecord& hitRecord, const Vector3& direction)
	{
		if (materialCanTransmitThroughGeometry(hitRecord.material))
		{
			return (true);
		}

		Vector3 geometricNormal = hitRecord.geometricNormal;
		if (Utilities::vectorLengthSquared(geometricNormal) <= 1e-12)
		{
			geometricNormal = hitRecord.normal;
		}
		if (Utilities::vectorLengthSquared(geometricNormal) <= 1e-12)
		{
			return (true);
		}
		return (Utilities::dot(geometricNormal, direction) > 1e-7);
	}

	double	subsurfaceSampleRadiusMeters(const ScatterRecord& scatterRecord)
	{
		const double diffusionLength = maxChannel(scatterRecord.subsurfaceRadiusMeters);
		if (!std::isfinite(diffusionLength) || diffusionLength <= 0.0)
		{
			return (0.0);
		}

		const double componentSample = Sampler::sample1D(Sampler::DIM_SUBSURFACE_PROFILE_COMPONENT);
		const Sampler::Sample2D radiusSample = Sampler::sample2D(Sampler::DIM_SUBSURFACE_SPATIAL);
		const double u = std::max(1e-12, std::min(1.0 - 1e-12, static_cast<double>(radiusSample.y)));
		const double exponentialScale = componentSample < 0.25
			? diffusionLength
			: 3.0 * diffusionLength;

		return (-exponentialScale * std::log(1.0 - u));
	}

	bool	refreshSubsurfaceScatterAtExit(HitRecord& hitRecord, ScatterRecord& scatterRecord)
	{
		if (!scatterRecord.bsdfMaterial)
		{
			return (false);
		}

		const ONB basis(hitRecord.normal);
		const Vector3 direction = basis.local(Sampler::cosineHemisphere(Sampler::DIM_BSDF_DIRECTION));
		const double pdfValue = scatterRecord.bsdfMaterial->scatteringPDF(
			scatterRecord.incidentRay,
			hitRecord,
			direction
		);
		const Color bsdfCos = scatterRecord.bsdfMaterial->evaluateBSDFCos(
			scatterRecord.incidentRay,
			hitRecord,
			direction
		);
		if (
			pdfValue <= 0.0
			|| !std::isfinite(pdfValue)
			|| maxChannel(bsdfCos) <= 0.0
		)
		{
			return (false);
		}

		scatterRecord.sampledDirection = direction;
		scatterRecord.sampledPDF = pdfValue;
		scatterRecord.attenuation = bsdfCos / pdfValue;
		return (true);
	}

	void	relocateSubsurfaceExit(Scene& scene, HitRecord& hitRecord, ScatterRecord& scatterRecord)
	{
		if (!scatterRecord.hasSubsurface || scatterRecord.subsurfaceThin || !hitRecord.material)
		{
			return;
		}

		const HitRecord entryHit = hitRecord;
		const ScatterRecord entryScatter = scatterRecord;
		const double sampleRadiusMeters = subsurfaceSampleRadiusMeters(scatterRecord);
		const double metersPerUnit = scene.getMetersPerUnit();
		if (
			sampleRadiusMeters <= 0.0
			|| !std::isfinite(sampleRadiusMeters)
			|| metersPerUnit <= 0.0
			|| !std::isfinite(metersPerUnit)
		)
		{
			return;
		}

		const double sampleRadiusScene = sampleRadiusMeters / metersPerUnit;
		if (sampleRadiusScene <= T_MIN)
		{
			return;
		}

		Vector3 normal = hitRecord.normal;
		if (Utilities::vectorLengthSquared(normal) <= 1e-12)
		{
			normal = hitRecord.geometricNormal;
		}
		if (Utilities::vectorLengthSquared(normal) <= 1e-12)
		{
			return;
		}
		normal = Utilities::normalize(normal);

		const ONB basis(normal);
		const Sampler::Sample2D diskSample = Sampler::sample2D(Sampler::DIM_SUBSURFACE_SPATIAL);
		const double angle = 2.0 * D_PI * static_cast<double>(diskSample.x);
		const Vector3 offset = basis.local(
			std::cos(angle) * sampleRadiusScene,
			std::sin(angle) * sampleRadiusScene,
			0.0
		);
		const Vector3 candidate = hitRecord.position + offset;
		const double probeDistance = std::max(
			sampleRadiusScene + (4.0 * maxChannel(scatterRecord.subsurfaceRadiusMeters) / metersPerUnit),
			16.0 * T_MIN
		);

		Ray probeRay(
			candidate + normal * probeDistance,
			normal * -1.0
		);
		HitRecord exitHit;
		if (!Renderer::internal::_checkHits(scene, probeRay, exitHit))
		{
			return;
		}
		if (
			exitHit.material != entryHit.material
			|| exitHit.t0 <= T_MIN
			|| exitHit.t0 > (2.0 * probeDistance + T_MIN)
		)
		{
			return;
		}

		hitRecord = exitHit;
		if (!refreshSubsurfaceScatterAtExit(hitRecord, scatterRecord))
		{
			hitRecord = entryHit;
			scatterRecord = entryScatter;
		}
	}

	bool	hasEnvironmentLight(Scene& scene)
	{
		return (
			scene.hasEnvironmentMap()
			&& scene.getEnvironmentLighting()
			&& scene.getEnvironmentStrength() > 0.0
		);
	}

	bool	hasConstantBackgroundLight(Scene& scene)
	{
		return (
			!hasEnvironmentLight(scene)
			&& scene.getRenderSky() == SKY_NONE
			&& maxChannel(scene.getBackgroundColor()) > 0.0
		);
	}

	bool hasProceduralAtmosphereLight(Scene& scene)
	{
		return (scene.getRenderSky() == SKY_ATMOSPHERE && !hasEnvironmentLight(scene));
	}

	bool	hasSampledInfiniteLight(Scene& scene)
	{
		return (
			hasEnvironmentLight(scene)
			|| hasProceduralAtmosphereLight(scene)
			|| hasConstantBackgroundLight(scene)
		);
	}

	bool	supportsConstantBackgroundDirectLighting(const ScatterRecord& scatterRecord)
	{
		return (scatterRecord.pdfType != SCATTER_PDF_BSDF);
	}

	bool	samplesVisibleInfiniteLightForScatter(
		Scene& scene,
		SkyTypes skyType,
		const ScatterRecord& scatterRecord
	)
	{
		return (
			((skyType == SKY_ENVIRONMENT || skyType == SKY_ATMOSPHERE) && hasEnvironmentLight(scene))
			|| (skyType == SKY_ATMOSPHERE && hasProceduralAtmosphereLight(scene))
			|| (
				hasConstantBackgroundLight(scene)
				&& supportsConstantBackgroundDirectLighting(scatterRecord)
			)
		);
	}

	bool	hasVisibleEnvironment(Scene& scene)
	{
		return (scene.hasEnvironmentMap() && scene.getEnvironmentStrength() > 0.0);
	}

	Color	sampleEnvironmentRadiance(Scene& scene, const Vector3& direction)
	{
		if (!hasVisibleEnvironment(scene))
		{
			return (Color(0.0, 0.0, 0.0));
		}
		return (
			scene.getEnvironmentMap()->sampleDirection(
				direction,
				scene.getEnvironmentRotation()
			) * scene.getEnvironmentStrength()
		);
	}

	Color	sampleProceduralStars(const Atmosphere& atmosphere, const Color& atmosphereColor)
	{
		double random = Sampler::sample1D(Sampler::DIM_ATMOSPHERE);
		if (random < 0.9996)
		{
			return (Color(0.0, 0.0, 0.0));
		}

		double starSample = Sampler::sample1D(Sampler::DIM_ATMOSPHERE + 1);
		double diff = (atmosphere.getStarsBrightness() - 0.2 + (0.4 * starSample))
			- ((atmosphereColor.getRed() + atmosphereColor.getGreen() + atmosphereColor.getBlue()) / 3.0);
		if (diff < 0.0)
		{
			diff = 0.0;
		}
		else if (diff > 1.0)
		{
			diff = 1.0;
		}
		return (Color(diff, diff, diff));
	}

	Color	sampleAtmosphereSky(Scene& scene, Ray& ray, double environmentMISWeight)
	{
		const Atmosphere& atmosphere = scene.getAtmosphere();
		const AtmosphereSample atmosphereSample = atmosphere.sampleSegment(ray, T_MAX);
		Color background(0.0, 0.0, 0.0);
		Color inScattering = atmosphereSample.inScattering;

		if (hasVisibleEnvironment(scene))
		{
			background = sampleEnvironmentRadiance(scene, ray.getDirection()) * environmentMISWeight;
		}
		else
		{
			// The procedural atmosphere is itself the sampled infinite light. Its
			// continuation hit must therefore use the same MIS weight as NEE.
			inScattering = inScattering * environmentMISWeight;
			background = sampleProceduralStars(atmosphere, atmosphereSample.inScattering);
		}
		return (inScattering + (atmosphereSample.transmittance * background));
	}

	Color	sampleSceneSky(Scene& scene, Ray& ray)
	{
		switch(scene.getRenderSky())
		{
			case (SKY_ATMOSPHERE):
				return (sampleAtmosphereSky(scene, ray, 1.0));
			case (SKY_LINEAR):
				return (Renderer::internal::_calculateSkyInterpolation(scene, ray));
			case (SKY_ENVIRONMENT):
				if (hasVisibleEnvironment(scene))
					return (sampleEnvironmentRadiance(scene, ray.getDirection()));
				return (scene.getBackgroundColor());
			default:
				return (scene.getBackgroundColor());
		}
	}

	void	compositePrimaryAtmosphereSegment(Scene& scene, const Ray& ray, double tMax, Color& accumulatedColor, Color& throughput)
	{
		if (scene.getRenderSky() != SKY_ATMOSPHERE)
		{
			return;
		}

		const AtmosphereSample atmosphereSample = scene.getAtmosphere().sampleSegment(ray, tMax);
		accumulatedColor += clampRayColor(throughput * atmosphereSample.inScattering);
		throughput = clampRayColor(throughput * atmosphereSample.transmittance);
	}

	double	infiniteLightPDF(Scene& scene, const Vector3& origin, const Vector3& direction)
	{
		if (hasEnvironmentLight(scene))
		{
			return (scene.getEnvironmentMap()->pdf(direction, scene.getEnvironmentRotation()));
		}
		if (hasProceduralAtmosphereLight(scene))
		{
			Vector3 up = Utilities::vectorLengthSquared(origin) > 1e-12
				? Utilities::normalize(origin)
				: Vector3(0.0, 1.0, 0.0);
			return (Utilities::dot(up, direction) >= 0.0 ? HEMISPHERE_PDF : 0.0);
		}
		if (hasConstantBackgroundLight(scene))
		{
			return (FULL_SPHERE_PDF);
		}
		return (0.0);
	}

	bool	applyRussianRoulette(Color& throughput, int bounces, bool highAlbedoVolume = false)
	{
		const int startBounce = highAlbedoVolume
			? VOLUME_RUSSIAN_ROULETTE_START_BOUNCE
			: RUSSIAN_ROULETTE_START_BOUNCE;
		if (bounces < startBounce)
		{
			return (true);
		}

		double survivalProbability = maxChannel(throughput);
		survivalProbability = std::max(
			RUSSIAN_ROULETTE_MIN_SURVIVAL,
			std::min(
				highAlbedoVolume ? VOLUME_RUSSIAN_ROULETTE_MAX_SURVIVAL : RUSSIAN_ROULETTE_MAX_SURVIVAL,
				survivalProbability
			)
		);
		if (Sampler::sample1D(Sampler::DIM_RUSSIAN_ROULETTE) > survivalProbability)
		{
			return (false);
		}
		throughput /= survivalProbability;

		return (true);
	}

	Color	mediumTransmittance(const ScatterRecord& scatterRecord, double distanceMeters)
	{
		if (!scatterRecord.hasMediumAbsorption || !std::isfinite(distanceMeters) || distanceMeters <= 0.0)
		{
			return (Color(1.0, 1.0, 1.0));
		}

		return (Color(
			std::exp(-scatterRecord.mediumAbsorptionCoefficient.getRed() * distanceMeters),
			std::exp(-scatterRecord.mediumAbsorptionCoefficient.getGreen() * distanceMeters),
			std::exp(-scatterRecord.mediumAbsorptionCoefficient.getBlue() * distanceMeters)
		));
	}

	Color	effectiveScatterAttenuation(
		Scene& scene,
		const HitRecord& hitRecord,
		const ScatterRecord& scatterRecord
	)
	{
		if (hitRecord.frontFace)
		{
			return (scatterRecord.attenuation);
		}
		return (
			scatterRecord.attenuation
			* mediumTransmittance(scatterRecord, scene.sceneUnitsToMeters(hitRecord.t0))
		);
	}

	double	normalizedColorChannel(double value)
	{
		if (!std::isfinite(value) || value <= 0.0)
		{
			return (0.0);
		}
		return (std::min(1.0, std::log1p(value) / std::log1p(D_MAX_RAY_COLOR_LUMINANCE)));
	}

	void	setPrimaryCoordinates(
		Denoise::FeatureVector& features,
		const Renderer::internal::RenderCamera& renderCamera,
		std::size_t x,
		std::size_t y
	)
	{
		features[0] = renderCamera.width > 1.0 ? static_cast<double>(x) / (renderCamera.width - 1.0) : 0.0;
		features[1] = renderCamera.height > 1.0 ? static_cast<double>(y) / (renderCamera.height - 1.0) : 0.0;
	}

	Denoise::FeatureVector	primaryMissFeatures(
		Scene& scene,
		const Renderer::internal::RenderCamera& renderCamera,
		Ray& ray,
		std::size_t x,
		std::size_t y
	)
	{
		Denoise::FeatureVector features;
		const Color background = sampleSceneSky(scene, ray);

		setPrimaryCoordinates(features, renderCamera, x, y);
		features[2] = 0.0;
		features[3] = 1.0;
		features[4] = 0.5;
		features[5] = 0.5;
		features[6] = 0.5;
		features[7] = normalizedColorChannel(background.getRed());
		features[8] = normalizedColorChannel(background.getGreen());
		features[9] = normalizedColorChannel(background.getBlue());
		features[10] = 0.0;
		return (features);
	}

	Denoise::FeatureVector	primaryHitFeatures(
		const HitRecord& hitRecord,
		const Renderer::internal::RenderCamera& renderCamera,
		std::size_t x,
		std::size_t y
	)
	{
		Denoise::FeatureVector features;

		Vector3 normal = hitRecord.normal;
		if (Utilities::vectorLengthSquared(normal) > 0.0)
		{
			normal = Utilities::normalize(normal);
		}
		const Color albedo = hitRecord.material ? hitRecord.material->colorAt(hitRecord) : Color(0.0, 0.0, 0.0);
		const MaterialType hitMaterialType = hitRecord.material ? hitRecord.material->getType() : LAMBERTIAN;
		const bool volumeHit = hitMaterialType == HENYEY_GREENSTEIN || hitMaterialType == ISOTROPIC;
		const double materialType = hitRecord.material
			? static_cast<double>(hitRecord.material->getType()) / static_cast<double>(PRINCIPLED)
			: 0.0;
		const double featureDepth = volumeHit && hitRecord.t1 > 0.0 ? hitRecord.t1 : hitRecord.t0;

		setPrimaryCoordinates(features, renderCamera, x, y);
		features[2] = 1.0;
		features[3] = featureDepth > 0.0
			? (volumeHit
				? std::min(1.0, std::log1p(featureDepth) / std::log(1000001.0))
				: featureDepth / (1.0 + featureDepth))
			: 0.0;
		features[4] = normal.getX() * 0.5 + 0.5;
		features[5] = normal.getY() * 0.5 + 0.5;
		features[6] = normal.getZ() * 0.5 + 0.5;
		features[7] = volumeHit ? std::clamp(hitRecord.u, 0.0, 1.0) : normalizedColorChannel(albedo.getRed());
		features[8] = normalizedColorChannel(albedo.getGreen());
		features[9] = normalizedColorChannel(albedo.getBlue());
		features[10] = materialType;
		return (features);
	}

	Color shadowTransmittance(Scene& scene, Ray& shadowRay, double tMax)
	{
		if (tMax <= T_MIN)
			return (Color(1.0, 1.0, 1.0));
		Color transmittance(1.0, 1.0, 1.0);

		const auto& accelerationStructure = scene.getAccelerationStructure();
		if (accelerationStructure)
		{
			transmittance = transmittance * accelerationStructure->shadowTransmittance(
				shadowRay,
				T_MIN,
				tMax
			);
			if (maxChannel(transmittance) <= 1e-8)
				return (Color(0.0, 0.0, 0.0));
		}

		const auto& unacceleratedHittables = scene.getUnacceleratedHittables();
		const auto& hittables = (!accelerationStructure && unacceleratedHittables.empty())
			? scene.getHittables()
			: unacceleratedHittables;

		for (const std::shared_ptr<Hittable>& hittable : hittables)
		{
			transmittance = transmittance * hittable->shadowTransmittance(shadowRay, T_MIN, tMax);
			if (maxChannel(transmittance) <= 1e-8)
				return (Color(0.0, 0.0, 0.0));
		}
		return (transmittance);
	}

	Color geometricShadowTransmittance(
		Scene& scene,
		const Vector3& origin,
		const Vector3& direction
	)
	{
		Color transmittance(1.0, 1.0, 1.0);
		Ray shadowRay = Ray::fromNormalizedDirection(origin, direction);

		// The diffuse environment control handles participating media separately
		// through one cache-friendly upward optical-depth query. Walking the original
		// scene list here keeps opaque geometry in the estimate without asking a BVH
		// that may also contain those media to attenuate them a second time.
		for (const std::shared_ptr<Hittable>& hittable : scene.getHittables())
		{
			if (
				!hittable
				|| dynamic_cast<const DensityVolume*>(hittable.get()) != nullptr
				|| isVolumePhaseMaterial(hittable->getMaterial())
			)
			{
				continue;
			}
			transmittance = transmittance * hittable->shadowTransmittance(
				shadowRay,
				T_MIN,
				T_MAX
			);
			if (maxChannel(transmittance) <= 1e-8)
				return (Color(0.0, 0.0, 0.0));
		}
		return (transmittance);
	}

	bool upwardCloudOpticalDepth(
		const std::vector<const DensityVolume*>& clouds,
		const Vector3& position,
		const Vector3& direction,
		double& opticalDepth
	)
	{
		opticalDepth = 0.0;
		for (const DensityVolume* cloud : clouds)
		{
			double cloudOpticalDepth = 0.0;
			if (cloud->directionalOpticalDepth(position, direction, cloudOpticalDepth))
			{
				if (!std::isfinite(cloudOpticalDepth) || cloudOpticalDepth < 0.0)
					return (false);
				opticalDepth += cloudOpticalDepth;
				continue;
			}

			// Procedural density fields intentionally do not allocate a directional
			// cache. Their deterministic shadow integrator is the bounded fallback.
			const Hittable* hittable = dynamic_cast<const Hittable*>(cloud);
			if (!hittable)
				return (false);
			Ray shadowRay = Ray::fromNormalizedDirection(position, direction);
			const Color transmittance = hittable->shadowTransmittance(
				shadowRay,
				T_MIN,
				T_MAX
			);
			const double scalarTransmittance = std::clamp(
				Utilities::luminance(transmittance),
				0.0,
				1.0
			);
			if (!std::isfinite(scalarTransmittance))
				return (false);
			cloudOpticalDepth = scalarTransmittance > std::exp(-20.0)
				? -std::log(scalarTransmittance)
				: 20.0;
			opticalDepth += cloudOpticalDepth;
		}
		return (std::isfinite(opticalDepth));
	}

	struct EnvironmentDiffuseKnot
	{
		Color visibleRadiance;
		double upwardOpticalDepth = 0.0;
		bool valid = false;
	};

	std::array<EnvironmentDiffuseKnot, ENVIRONMENT_DIFFUSE_KNOT_COUNT>
	buildEnvironmentDiffuseKnots(
		Scene& scene,
		const Ray& cameraRay,
		double entryT,
		double exitT,
		const Vector3& upwardDirection,
		const std::vector<const DensityVolume*>& clouds
	)
	{
		std::array<EnvironmentDiffuseKnot, ENVIRONMENT_DIFFUSE_KNOT_COUNT> knots{};
		const double inverseRootThree = 1.0 / std::sqrt(3.0);
		const std::array<Vector3, ENVIRONMENT_DIFFUSE_DIRECTION_COUNT> directions = {{
			Vector3(-inverseRootThree, -inverseRootThree, -inverseRootThree),
			Vector3(-inverseRootThree, -inverseRootThree, inverseRootThree),
			Vector3(-inverseRootThree, inverseRootThree, -inverseRootThree),
			Vector3(-inverseRootThree, inverseRootThree, inverseRootThree),
			Vector3(inverseRootThree, -inverseRootThree, -inverseRootThree),
			Vector3(inverseRootThree, -inverseRootThree, inverseRootThree),
			Vector3(inverseRootThree, inverseRootThree, -inverseRootThree),
			Vector3(inverseRootThree, inverseRootThree, inverseRootThree)
		}};
		const std::shared_ptr<EnvironmentMap>& environment = scene.getEnvironmentMap();
		if (!environment)
			return (knots);

		for (std::size_t knotIndex = 0; knotIndex < knots.size(); knotIndex++)
		{
			const double fraction = (static_cast<double>(knotIndex) + 0.5)
				/ static_cast<double>(knots.size());
			const Vector3 position = cameraRay.pointAtRay(
				entryT + fraction * (exitT - entryT)
			);
			Color visibleRadiance(0.0, 0.0, 0.0);
			for (const Vector3& direction : directions)
			{
				visibleRadiance += environment->sampleDirection(
					direction,
					scene.getEnvironmentRotation()
				) * geometricShadowTransmittance(scene, position, direction);
			}
			knots[knotIndex].visibleRadiance = visibleRadiance
				* (scene.getEnvironmentStrength() / static_cast<double>(directions.size()));
			knots[knotIndex].valid = upwardCloudOpticalDepth(
				clouds,
				position,
				upwardDirection,
				knots[knotIndex].upwardOpticalDepth
			);
		}
		return (knots);
	}

	EnvironmentDiffuseKnot interpolateEnvironmentDiffuseKnot(
		const std::array<EnvironmentDiffuseKnot, ENVIRONMENT_DIFFUSE_KNOT_COUNT>& knots,
		double fraction
	)
	{
		const double coordinate = std::clamp(
			fraction * static_cast<double>(knots.size()) - 0.5,
			0.0,
			static_cast<double>(knots.size() - 1)
		);
		const std::size_t first = static_cast<std::size_t>(std::floor(coordinate));
		const std::size_t second = std::min(first + 1, knots.size() - 1);
		const double weight = coordinate - static_cast<double>(first);
		EnvironmentDiffuseKnot result;

		result.visibleRadiance = knots[first].visibleRadiance * (1.0 - weight)
			+ knots[second].visibleRadiance * weight;
		result.upwardOpticalDepth = knots[first].upwardOpticalDepth * (1.0 - weight)
			+ knots[second].upwardOpticalDepth * weight;
		result.valid = knots[first].valid && knots[second].valid;
		return (result);
	}

	std::vector<const DensityVolume*> primaryDensityVolumes(Scene& scene)
	{
		std::vector<const DensityVolume*> volumes;
		for (const std::shared_ptr<Hittable>& hittable : scene.getHittables())
		{
			const DensityVolume* volume = dynamic_cast<const DensityVolume*>(hittable.get());
			if (volume)
				volumes.push_back(volume);
		}
		return (volumes);
	}

	struct PrimaryCloudControl
	{
		Color radiance;
		double opacity = 0.0;
	};

	PrimaryCloudControl primaryDirectionalCloudScattering(
		Scene& scene,
		const Ray& cameraRay,
		const std::vector<const DensityVolume*>& clouds
	)
	{
		if (clouds.empty() || scene.getVolumeReference())
			return (PrimaryCloudControl());
		std::vector<const DirectionalLight*> directionalLights;
		for (const std::shared_ptr<Hittable>& light : scene.getLights())
		{
			const DirectionalLight* directional = dynamic_cast<const DirectionalLight*>(light.get());
			if (directional && directional->getMaterial())
				directionalLights.push_back(directional);
		}
		bool hasReconstructedMultipleScattering = false;
		for (const DensityVolume* cloud : clouds)
		{
			if (
				cloud->multipleScatteringFalloff() < 0.999
				&& cloud->multipleScatteringCompensation() > 0.0
			)
			{
				hasReconstructedMultipleScattering = true;
				break;
			}
		}
		const bool hasDiffuseAtmosphere = hasReconstructedMultipleScattering
			&& hasProceduralAtmosphereLight(scene);
		const bool hasDiffuseEnvironment = hasReconstructedMultipleScattering
			&& hasEnvironmentLight(scene);
		double entryT = T_MAX;
		double exitT = -T_MAX;
		double minimumFeatureScale = T_MAX;
		for (const DensityVolume* cloud : clouds)
		{
			double cloudEntry;
			double cloudExit;
			if (!cloud->integrationInterval(cameraRay, T_MIN, T_MAX, cloudEntry, cloudExit))
				continue;
			entryT = std::min(entryT, std::max(0.0, cloudEntry));
			exitT = std::max(exitT, cloudExit);
			minimumFeatureScale = std::min(
				minimumFeatureScale,
				cloud->volumeFeatureScale()
			);
		}
		exitT = std::min(exitT, nearestCameraSurfaceT(scene, cameraRay));
		if (!(entryT < exitT) || !std::isfinite(entryT) || !std::isfinite(exitT))
			return (PrimaryCloudControl());
		const double rayLength = std::sqrt(Utilities::vectorLengthSquared(cameraRay.getDirection()));
		if (rayLength <= 0.0 || !std::isfinite(rayLength))
			return (PrimaryCloudControl());
		const Vector3 cameraDirection = cameraRay.getDirection() / rayLength;
		const double interval = exitT - entryT;
		const double pathLength = interval * rayLength;
		const int stepCount = static_cast<int>(std::clamp(
			std::ceil(pathLength / std::max(1.0, minimumFeatureScale * 0.05)),
			48.0,
			static_cast<double>(scene.getVolumePrimaryMaxSteps())
		));
		const double stepT = interval / static_cast<double>(stepCount);
		const double stepDistance = stepT * rayLength;
		const Vector3 skyDirection = hasDiffuseAtmosphere
			&& Utilities::vectorLengthSquared(cameraRay.getOrigin()) > 1e-12
			? Utilities::normalize(cameraRay.getOrigin())
			: Vector3(0.0, 1.0, 0.0);
		Color diffuseSkyRadiance(0.0, 0.0, 0.0);
		double diffuseSkyPhaseIntegral = 0.0;
		std::array<EnvironmentDiffuseKnot, ENVIRONMENT_DIFFUSE_KNOT_COUNT>
			environmentDiffuseKnots{};
		if (hasDiffuseEnvironment)
		{
			environmentDiffuseKnots = buildEnvironmentDiffuseKnots(
				scene,
				cameraRay,
				entryT,
				exitT,
				skyDirection,
				clouds
			);
		}
		else if (hasDiffuseAtmosphere)
		{
			diffuseSkyRadiance = scene.getAtmosphere().sampleDiffuseSkyRadiance(
				cameraRay.getOrigin()
			);
			// sampleDiffuseSkyRadiance() is an upper-hemisphere mean.
			diffuseSkyPhaseIntegral = 0.5;
		}
		Color result(0.0, 0.0, 0.0);
		double cameraCloudTransmittance = 1.0;

		std::vector<double> extinctions(clouds.size());
		for (int step = 0; step < stepCount; step++)
		{
			const double sampleT = entryT + (static_cast<double>(step) + 0.5) * stepT;
			const Vector3 position = cameraRay.pointAtRay(sampleT);
			double totalExtinction = 0.0;
			for (std::size_t i = 0; i < clouds.size(); i++)
			{
				extinctions[i] = clouds[i]->extinctionAt(position);
				totalExtinction += extinctions[i];
			}
			if (totalExtinction <= 0.0)
				continue;

			Color source(0.0, 0.0, 0.0);
			for (const DirectionalLight* light : directionalLights)
			{
				Vector3 lightDirection = light->getDirection() * -1.0;
				const double lightLengthSquared = Utilities::vectorLengthSquared(lightDirection);
				if (lightLengthSquared <= 0.0 || !std::isfinite(lightLengthSquared))
					continue;
				lightDirection /= std::sqrt(lightLengthSquared);
				Color scatteringCoefficient(0.0, 0.0, 0.0);
				Color bulkScatteringCoefficient(0.0, 0.0, 0.0);
				double falloffExtinctionSum = 0.0;
				double compensationExtinctionSum = 0.0;
				double bulkExtinctionSum = 0.0;
				double directionalOpticalDepth = 0.0;
				bool hasDirectionalOpticalDepth = false;
				bool missingDirectionalOpticalDepth = false;
				for (std::size_t i = 0; i < clouds.size(); i++)
				{
					const DensityVolume* cloud = clouds[i];
					const double cloudExtinction = extinctions[i];
					scatteringCoefficient += cloud->singleScatteringWithExtinction(
						position,
						cameraDirection,
						lightDirection,
						cloudExtinction
					);
					bulkScatteringCoefficient += cloud->volumeAlbedo()
						* cloudExtinction;
					falloffExtinctionSum += cloudExtinction
						* cloud->multipleScatteringFalloff();
					compensationExtinctionSum += cloudExtinction
						* cloud->multipleScatteringCompensation();
					bulkExtinctionSum += cloudExtinction;
					if (cloudExtinction > 0.0)
					{
						double cloudOpticalDepth = 0.0;
						if (cloud->directionalOpticalDepth(
							position,
							lightDirection,
							cloudOpticalDepth
						))
						{
							directionalOpticalDepth += cloudOpticalDepth;
							hasDirectionalOpticalDepth = true;
						}
						else
							missingDirectionalOpticalDepth = true;
					}
				}
				if (maxChannel(scatteringCoefficient) <= 0.0)
					continue;
				Ray shadowRay = Ray::fromNormalizedDirection(position, lightDirection);
				Color lightTransmittance = shadowTransmittance(scene, shadowRay, T_MAX);
				Color emitted = light->getMaterial()->emitted();
				if (scene.getRenderSky() == SKY_ATMOSPHERE)
				{
					emitted = emitted * scene.getAtmosphere().sampleTransmittance(
						shadowRay,
						T_MAX
					);
				}
				if (maxChannel(lightTransmittance) > 1e-8)
					source += scatteringCoefficient * emitted * lightTransmittance;

				// The depth-falloff mode deliberately removes high-order collision
				// events. Reconstruct their low-frequency energy deterministically:
				// after the first event the phase function rapidly isotropizes, while
				// the effective directional optical depth falls with scattering order.
				// A finite geometric series keeps this bounded and leaves falloff=1 as
				// the fully path-traced, uncompensated mode.
				if (bulkExtinctionSum > 0.0 && maxChannel(bulkScatteringCoefficient) > 0.0)
				{
					const double averageFalloff = std::clamp(
						falloffExtinctionSum / bulkExtinctionSum,
						0.0,
						1.0
					);
					if (averageFalloff < 0.999)
					{
						const double averageAlbedo = std::clamp(
							Utilities::luminance(bulkScatteringCoefficient)
							/ bulkExtinctionSum,
							0.0,
							1.0
						);
						const double ratio = std::min(0.94, averageAlbedo * averageFalloff);
						const double orderGain = ratio > 1e-6
							? ratio * (1.0 - std::pow(ratio, 4.0)) / (1.0 - ratio)
							: 0.0;
						const double directVisibility = std::clamp(
							Utilities::luminance(lightTransmittance),
							0.0,
							1.0
						);
						double diffuseVisibility = 0.12
							+ 0.88 * std::pow(directVisibility, 0.20);
						if (hasDirectionalOpticalDepth && !missingDirectionalOpticalDepth)
						{
							// Diffusion in a highly scattering medium attenuates with
							// exp(-sqrt(3 * absorption) * opticalDepth), substantially
							// slower than direct Beer-Lambert transport. Retaining the
							// cached depth avoids collapsing every optically thick point
							// to the same flat fill after direct transmittance underflows.
							const double diffusionRate = std::clamp(
								std::sqrt(std::max(0.0, 3.0 * (1.0 - averageAlbedo))),
								0.05,
								0.15
							);
							const double diffusionVisibility = std::exp(
								-diffusionRate * directionalOpticalDepth
							);
							diffuseVisibility = 0.09 + 0.91 * std::max(
								diffusionVisibility,
								std::pow(directVisibility, 0.20)
							);
						}
						constexpr double ISOTROPIC_PHASE = 0.07957747154594766788;
						const double compensation = std::clamp(
							compensationExtinctionSum / bulkExtinctionSum,
							0.0,
							2.0
						);
						source += bulkScatteringCoefficient * emitted
							* (ISOTROPIC_PHASE * orderGain * diffuseVisibility * compensation);
					}
				}
			}
			if (
				hasDiffuseEnvironment
				|| (diffuseSkyPhaseIntegral > 0.0 && maxChannel(diffuseSkyRadiance) > 0.0)
			)
			{
				Color diffuseIncidentRadiance = diffuseSkyRadiance;
				double diffusePhaseIntegral = diffuseSkyPhaseIntegral;
				double environmentUpwardOpticalDepth = 0.0;
				bool environmentKnotValid = true;
				if (hasDiffuseEnvironment)
				{
					const EnvironmentDiffuseKnot environmentKnot =
						interpolateEnvironmentDiffuseKnot(
							environmentDiffuseKnots,
							(static_cast<double>(step) + 0.5)
								/ static_cast<double>(stepCount)
						);
					diffuseIncidentRadiance = environmentKnot.visibleRadiance;
					diffusePhaseIntegral = 1.0;
					environmentUpwardOpticalDepth = environmentKnot.upwardOpticalDepth;
					environmentKnotValid = environmentKnot.valid;
				}
				if (!environmentKnotValid)
					diffusePhaseIntegral = 0.0;
				if (diffusePhaseIntegral > 0.0 && maxChannel(diffuseIncidentRadiance) > 0.0)
				{
					Color bulkScatteringCoefficient(0.0, 0.0, 0.0);
					double bulkExtinctionSum = 0.0;
					double falloffExtinctionSum = 0.0;
					double compensationExtinctionSum = 0.0;
					double skyOpticalDepth = environmentUpwardOpticalDepth;
					bool hasSkyOpticalDepth = hasDiffuseEnvironment;
					bool missingSkyOpticalDepth = false;
					for (std::size_t i = 0; i < clouds.size(); i++)
					{
						const DensityVolume* cloud = clouds[i];
						const double cloudExtinction = extinctions[i];
						bulkScatteringCoefficient += cloud->volumeAlbedo() * cloudExtinction;
						bulkExtinctionSum += cloudExtinction;
						falloffExtinctionSum += cloudExtinction
							* cloud->multipleScatteringFalloff();
						compensationExtinctionSum += cloudExtinction
							* cloud->multipleScatteringCompensation();
						if (!hasDiffuseEnvironment && cloudExtinction > 0.0)
						{
							double cloudOpticalDepth = 0.0;
							if (cloud->directionalOpticalDepth(
								position,
								skyDirection,
								cloudOpticalDepth
							))
							{
								skyOpticalDepth += cloudOpticalDepth;
								hasSkyOpticalDepth = true;
							}
							else
								missingSkyOpticalDepth = true;
						}
					}
					if (bulkExtinctionSum > 0.0 && maxChannel(bulkScatteringCoefficient) > 0.0)
					{
						const double averageFalloff = std::clamp(
							falloffExtinctionSum / bulkExtinctionSum,
							0.0,
							1.0
						);
						const double compensation = std::clamp(
							compensationExtinctionSum / bulkExtinctionSum,
							0.0,
							2.0
						);
						if (averageFalloff < 0.999 && compensation > 0.0)
						{
							const double averageAlbedo = std::clamp(
								Utilities::luminance(bulkScatteringCoefficient)
								/ bulkExtinctionSum,
								0.0,
								1.0
							);
							const double ratio = std::min(0.94, averageAlbedo * averageFalloff);
							const double orderGain = ratio > 1e-6
								? ratio * (1.0 - std::pow(ratio, 4.0)) / (1.0 - ratio)
								: 0.0;
							double skyVisibility = hasDiffuseEnvironment ? 0.0 : 0.12;
							if (hasSkyOpticalDepth && !missingSkyOpticalDepth)
							{
								const double diffusionRate = std::clamp(
									std::sqrt(std::max(0.0, 3.0 * (1.0 - averageAlbedo))),
									0.05,
									0.15
								);
								const double diffusionVisibility = std::exp(
									-diffusionRate * skyOpticalDepth
								);
								skyVisibility = hasDiffuseEnvironment
									? diffusionVisibility
									: 0.09 + 0.91 * diffusionVisibility;
							}
							source += bulkScatteringCoefficient * diffuseIncidentRadiance
								* (diffusePhaseIntegral * orderGain * skyVisibility * compensation);
						}
					}
				}
			}
			const double segmentExtinction = -std::expm1(
				-totalExtinction * stepDistance
			);
			if (maxChannel(source) > 0.0)
			{
				Color cameraAtmosphereTransmittance(1.0, 1.0, 1.0);
				if (scene.getRenderSky() == SKY_ATMOSPHERE)
				{
					const Ray atmosphereRay = Ray::fromNormalizedDirection(
						cameraRay.getOrigin(),
						cameraDirection
					);
					cameraAtmosphereTransmittance = scene.getAtmosphere().sampleTransmittance(
						atmosphereRay,
						sampleT * rayLength
					);
				}
				const double integratedSegment = cameraCloudTransmittance
					* segmentExtinction / totalExtinction;
				result += source * cameraAtmosphereTransmittance * integratedSegment;
			}
			cameraCloudTransmittance *= 1.0 - segmentExtinction;
			if (cameraCloudTransmittance <= 1e-7)
				break;
		}
		PrimaryCloudControl control;
		control.radiance = clampRayColor(result);
		control.opacity = std::clamp(1.0 - cameraCloudTransmittance, 0.0, 1.0);
		return (control);
	}

	LightSample	sampleConstantBackgroundLight(Scene& scene, const Vector3& origin)
	{
		(void)origin;
		LightSample sample;

		if (!hasConstantBackgroundLight(scene))
		{
			return (sample);
		}
		sample.direction = Sampler::sphereDirection(Sampler::DIM_ENVIRONMENT_POINT);
		sample.emitted = scene.getBackgroundColor();
		sample.pdf = FULL_SPHERE_PDF;
		sample.tMax = T_MAX;
		if (maxChannel(sample.emitted) <= 0.0)
		{
			return (sample);
		}

		sample.valid = true;
		return (sample);
	}

	LightSample	sampleEnvironmentLight(Scene& scene, const Vector3& origin)
	{
		(void)origin;
		LightSample sample;

		if (!hasEnvironmentLight(scene))
		{
			return (sample);
		}

		const EnvironmentMap::Sample environmentSample = scene.getEnvironmentMap()->sample(
			Sampler::sample1D(Sampler::DIM_ENVIRONMENT_SELECTION),
			Sampler::sample2D(Sampler::DIM_ENVIRONMENT_POINT),
			scene.getEnvironmentRotation()
		);
		if (!environmentSample.valid || environmentSample.pdf <= 0.0 || !std::isfinite(environmentSample.pdf))
		{
			return (sample);
		}

		sample.direction = environmentSample.direction;
		sample.emitted = environmentSample.radiance * scene.getEnvironmentStrength();
		sample.pdf = environmentSample.pdf;
		sample.tMax = T_MAX;
		if (maxChannel(sample.emitted) <= 0.0)
		{
			return (sample);
		}

		sample.valid = true;
		return (sample);
	}

	LightSample sampleProceduralAtmosphereLight(Scene& scene, const Vector3& origin)
	{
		LightSample sample;
		if (!hasProceduralAtmosphereLight(scene))
			return (sample);

		Vector3 up = Utilities::vectorLengthSquared(origin) > 1e-12
			? Utilities::normalize(origin)
			: Vector3(0.0, 1.0, 0.0);
		const Sampler::Sample2D random = Sampler::sample2D(Sampler::DIM_ENVIRONMENT_POINT);
		const double phi = 2.0 * D_PI * random.x;
		const double z = random.y;
		const double radius = std::sqrt(std::max(0.0, 1.0 - z * z));
		sample.direction = ONB(up).local(radius * std::cos(phi), radius * std::sin(phi), z);
		sample.pdf = HEMISPHERE_PDF;
		sample.tMax = T_MAX;
		Ray atmosphereRay = Ray::fromNormalizedDirection(origin, sample.direction);
		sample.emitted = scene.getAtmosphere().sampleSegment(atmosphereRay, T_MAX).inScattering;
		if (maxChannel(sample.emitted) <= 0.0)
			return (sample);

		sample.valid = true;
		return (sample);
	}

	LightSample	sampleInfiniteLight(Scene& scene, const Vector3& origin)
	{
		if (hasEnvironmentLight(scene))
		{
			return (sampleEnvironmentLight(scene, origin));
		}
		if (hasProceduralAtmosphereLight(scene))
		{
			return (sampleProceduralAtmosphereLight(scene, origin));
		}
		return (sampleConstantBackgroundLight(scene, origin));
	}

	double	directLightSampleProbability(int bounces, bool highAlbedoVolume)
	{
		if (
			bounces < DIRECT_LIGHT_FULL_SAMPLE_BOUNCES
			|| (highAlbedoVolume && bounces < VOLUME_DIRECT_LIGHT_FULL_SAMPLE_BOUNCES)
		)
		{
			return (1.0);
		}
		return (DIRECT_LIGHT_LATE_SAMPLE_PROBABILITY);
	}

	LightSample	sampleLight(
		const LightDistribution& lightDistribution,
		const std::vector<std::shared_ptr<Hittable>>& lights,
		const Vector3& origin
	)
	{
		LightSample sample;

		if (lights.empty())
		{
			return (sample);
		}

		const std::size_t lightIndex = selectLightIndex(lightDistribution, lights);
		const double selectionProbability = lightSelectionProbability(lightDistribution, lights, lightIndex);
		if (selectionProbability <= 0.0 || !std::isfinite(selectionProbability))
		{
			return (sample);
		}

		HittableLightSample surfaceSample;
		if (!lights[lightIndex]->sampleLight(origin, surfaceSample) || !surfaceSample.valid)
		{
			return (sample);
		}

		sample.direction = surfaceSample.direction;
		sample.directional = std::dynamic_pointer_cast<DirectionalLight>(lights[lightIndex]) != nullptr;
		sample.pdf = surfaceSample.pdf * selectionProbability;
		sample.tMax = surfaceSample.tMax;
		if (
			sample.pdf <= 0.0
			|| sample.tMax <= T_MIN
			|| !std::isfinite(sample.pdf)
			|| !std::isfinite(sample.tMax)
		)
		{
			return (sample);
		}

		Material* material = surfaceSample.material
			? surfaceSample.material
			: lights[lightIndex]->getMaterial();
		if (!material)
		{
			return (sample);
		}

		sample.emitted = surfaceSample.hasEmitted
			? surfaceSample.emitted
			: material->emitted();
		if (maxChannel(sample.emitted) <= 0.0)
		{
			return (sample);
		}

		sample.valid = true;
		return (sample);
	}

	Color	estimateDirectLighting(
		Scene& scene,
		const std::vector<std::shared_ptr<Hittable>>& lights,
		const LightDistribution& lightDistribution,
		HitRecord& hitRecord,
		const ScatterRecord& scatterRecord,
		int bounces,
		bool hasSunGuide,
		const Vector3& sunGuideDirection,
		bool hasSkyGuide,
		const Vector3& skyGuideDirection,
		bool replacePrimaryDirectionalScattering
	)
	{
		if (lights.empty())
		{
			return (Color(0.0, 0.0, 0.0));
		}

		const MaterialType materialType = hitRecord.material ? hitRecord.material->getType() : LAMBERTIAN;
		const bool highAlbedoVolume = materialType == HENYEY_GREENSTEIN || materialType == ISOTROPIC;
		const double sampleProbability = directLightSampleProbability(bounces, highAlbedoVolume);
		if (sampleProbability <= 0.0)
		{
			return (Color(0.0, 0.0, 0.0));
		}
		if (
			sampleProbability < 1.0
			&& Sampler::sample1D(Sampler::DIM_LIGHT_STRATEGY) >= sampleProbability
		)
		{
			return (Color(0.0, 0.0, 0.0));
		}

		const LightSample lightSample = sampleLight(lightDistribution, lights, hitRecord.position);
		if (!lightSample.valid)
		{
			return (Color(0.0, 0.0, 0.0));
		}
		if (replacePrimaryDirectionalScattering && bounces == 0 && lightSample.directional)
			return (Color(0.0, 0.0, 0.0));
		if (!leavesOpaqueGeometricSurface(hitRecord, lightSample.direction))
		{
			return (Color(0.0, 0.0, 0.0));
		}

		const double scatterSamplePDF = scatterProposalPDF(
			scatterRecord,
			hitRecord,
			lightSample.direction,
			hasSunGuide,
			sunGuideDirection,
			hasSkyGuide,
			skyGuideDirection,
			scene.getVolumeGuidingField().get(),
			bounces >= scene.getVolumeGuidingStartBounce()
				? scene.getVolumeGuidingStrength() : 0.0
		);
		const Color bsdfCos = scatterBSDFCos(scatterRecord, hitRecord, lightSample.direction);
		if (
			scatterSamplePDF <= 0.0
			|| !std::isfinite(scatterSamplePDF)
			|| maxChannel(bsdfCos) <= 0.0
		)
		{
			return (Color(0.0, 0.0, 0.0));
		}
		Ray shadowRay = Ray::fromNormalizedDirection(hitRecord.position, lightSample.direction);
		const ScopedDirectionalShadowSampling directionalShadowScope(lightSample.directional);
		const Color visibility = shadowTransmittance(scene, shadowRay, lightSample.tMax - T_MIN);
		if (maxChannel(visibility) <= 1e-8)
			return (Color(0.0, 0.0, 0.0));

		const double misWeight = lightSample.directional
			? 1.0
			: powerHeuristic(lightSample.pdf, scatterSamplePDF);

		Color emitted = lightSample.emitted;
		if (lightSample.directional && scene.getRenderSky() == SKY_ATMOSPHERE)
		{
			Ray atmosphereRay = Ray::fromNormalizedDirection(hitRecord.position, lightSample.direction);
			emitted = emitted * scene.getAtmosphere().sampleTransmittance(atmosphereRay, T_MAX);
		}
		const Color contribution = (
			bsdfCos
			* emitted
			* visibility
			* (misWeight / (lightSample.pdf * sampleProbability))
		);
		const std::shared_ptr<VolumeGuidingField>& guide = scene.getVolumeGuidingField();
		if (highAlbedoVolume && guide && !guide->isFrozen())
			guide->record(hitRecord.position, lightSample.direction, Utilities::luminance(contribution));
		return (contribution);
	}

	Color	estimateEnvironmentLighting(
		Scene& scene,
		HitRecord& hitRecord,
		const ScatterRecord& scatterRecord,
		int bounces,
		bool hasSunGuide,
		const Vector3& sunGuideDirection,
		bool hasSkyGuide,
		const Vector3& skyGuideDirection
	)
	{
		const bool constantBackgroundLight = hasConstantBackgroundLight(scene);

		if (
			constantBackgroundLight
			&& !supportsConstantBackgroundDirectLighting(scatterRecord)
		)
		{
			return (Color(0.0, 0.0, 0.0));
		}

		const MaterialType materialType = hitRecord.material ? hitRecord.material->getType() : LAMBERTIAN;
		const bool highAlbedoVolume = materialType == HENYEY_GREENSTEIN || materialType == ISOTROPIC;
		const double sampleProbability = directLightSampleProbability(bounces, highAlbedoVolume);
		if (sampleProbability <= 0.0)
		{
			return (Color(0.0, 0.0, 0.0));
		}
		if (
			sampleProbability < 1.0
			&& Sampler::sample1D(Sampler::DIM_ENVIRONMENT_STRATEGY) >= sampleProbability
		)
		{
			return (Color(0.0, 0.0, 0.0));
		}

		const LightSample lightSample = sampleInfiniteLight(scene, hitRecord.position);
		if (!lightSample.valid)
		{
			return (Color(0.0, 0.0, 0.0));
		}
		if (!leavesOpaqueGeometricSurface(hitRecord, lightSample.direction))
		{
			return (Color(0.0, 0.0, 0.0));
		}

		const double scatterSamplePDF = scatterProposalPDF(
			scatterRecord,
			hitRecord,
			lightSample.direction,
			hasSunGuide,
			sunGuideDirection,
			hasSkyGuide,
			skyGuideDirection,
			scene.getVolumeGuidingField().get(),
			bounces >= scene.getVolumeGuidingStartBounce()
				? scene.getVolumeGuidingStrength() : 0.0
		);
		const Color bsdfCos = scatterBSDFCos(scatterRecord, hitRecord, lightSample.direction);
		if (
			scatterSamplePDF <= 0.0
			|| !std::isfinite(scatterSamplePDF)
			|| maxChannel(bsdfCos) <= 0.0
		)
		{
			return (Color(0.0, 0.0, 0.0));
		}

		const double misWeight = powerHeuristic(lightSample.pdf, scatterSamplePDF);
		Ray shadowRay = Ray::fromNormalizedDirection(hitRecord.position, lightSample.direction);
		const Color visibility = shadowTransmittance(scene, shadowRay, lightSample.tMax - T_MIN);
		if (maxChannel(visibility) <= 1e-8)
			return (Color(0.0, 0.0, 0.0));

		Color contribution = (
			bsdfCos
			* lightSample.emitted
			* visibility
			* (misWeight / (lightSample.pdf * sampleProbability))
		);
		if (constantBackgroundLight)
		{
			contribution = clampConstantBackgroundMiss(scene, contribution);
		}
		const std::shared_ptr<VolumeGuidingField>& guide = scene.getVolumeGuidingField();
		if (highAlbedoVolume && guide && !guide->isFrozen())
			guide->record(hitRecord.position, lightSample.direction, Utilities::luminance(contribution));
		return (contribution);
	}

	Color	calculateLightRaysColor(
		const Ray& ray,
		Scene& scene,
		Denoise::FeatureVector* primaryFeatures,
		const Renderer::internal::RenderCamera* renderCamera,
		std::size_t x,
		std::size_t y,
		bool replacePrimaryDirectionalScattering
	);
}

Color	Renderer::internal::_calculatePixelColor(Scene& scene, const RenderCamera& renderCamera, std::size_t x, std::size_t y)
{
	Sampler::setReferenceVolumeTransport(scene.getVolumeReference());
	Ray	ray = internal::_generateRay(renderCamera, x, y);

	return (calculateLightRaysColor(ray, scene, nullptr, nullptr, x, y, !scene.getVolumeReference()));
}

Renderer::internal::RenderSample	Renderer::internal::_calculatePixelSample(
	Scene& scene,
	const RenderCamera& renderCamera,
	std::size_t x,
	std::size_t y,
	bool calculatePrimarySingleScattering
)
{
	Sampler::setReferenceVolumeTransport(scene.getVolumeReference());
	Sampler::setFeatureSampling(true);
	Ray	ray = internal::_generateRay(renderCamera, x, y);
	RenderSample sample;
	Ray featureRay = ray;
	HitRecord featureHit;
	if (Renderer::internal::_checkHits(scene, featureRay, featureHit))
	{
		sample.features = primaryHitFeatures(featureHit, renderCamera, x, y);
		const MaterialType materialType = featureHit.material
			? featureHit.material->getType()
			: LAMBERTIAN;
		sample.primaryClass = (
			materialType == HENYEY_GREENSTEIN || materialType == ISOTROPIC
		)
			? PrimaryRayClass::Volume
			: PrimaryRayClass::Surface;
	}
	else
	{
		sample.features = primaryMissFeatures(scene, renderCamera, featureRay, x, y);
		sample.primaryClass = PrimaryRayClass::Background;
	}
	Sampler::setFeatureSampling(false);
	if (calculatePrimarySingleScattering)
	{
		Sampler::setVolumeControlSampling(true);
		const PrimaryCloudControl control = primaryDirectionalCloudScattering(
			scene,
			ray,
			primaryDensityVolumes(scene)
		);
		sample.primarySingleScattering = control.radiance;
		sample.primaryVolumeOpacity = control.opacity;
		if (control.opacity > 1e-5)
		{
			sample.primaryClass = PrimaryRayClass::Volume;
		}
		Sampler::setVolumeControlSampling(false);
	}
	else
		sample.primarySingleScattering = Color(0.0, 0.0, 0.0);
	sample.color = calculateLightRaysColor(ray, scene, nullptr, nullptr, x, y, !scene.getVolumeReference());
	return (sample);
}

// Properly calculates light rays bounces, reflections, refractions, intersection, etc and returns the resulting color
Color	Renderer::internal::_calculateLightRaysColor(const Ray& ray, Scene& scene)
{
	Sampler::setReferenceVolumeTransport(scene.getVolumeReference());
	return (calculateLightRaysColor(ray, scene, nullptr, nullptr, 0, 0, false));
}

namespace
{
	Color	calculateLightRaysColor(
		const Ray& ray,
		Scene& scene,
		Denoise::FeatureVector* primaryFeatures,
		const Renderer::internal::RenderCamera* renderCamera,
		std::size_t x,
		std::size_t y,
		bool replacePrimaryDirectionalScattering
	)
	{
		const int		maxLightBounces = scene.getMaxLightBounces();
		const auto		skyType = scene.getRenderSky();
		const auto&		lights = scene.getLights();
		const auto		lightCount = lights.size();
		Vector3		volumeGuideDirection;
		const bool		hasVolumeGuide = primaryDirectionalGuide(lights, volumeGuideDirection);
		const bool		sampledInfiniteLight = hasSampledInfiniteLight(scene);
		const LightDistribution lightDistribution = {
			&scene.getLightSelectionCumulativeWeights(),
			scene.getLightSelectionTotalWeight()
		};
		const CausticPhotonMap* causticPhotonMap = scene.getCausticPhotonMap().get();
		std::vector<const Material*> primaryCloudMaterials;
		for (const std::shared_ptr<Hittable>& hittable : scene.getHittables())
		{
			if (dynamic_cast<const DensityVolume*>(hittable.get()))
				primaryCloudMaterials.push_back(hittable->getMaterial());
		}
		//static bool		distanceBlueness = scene.getDistanceBlueness();

		Ray		currentRay = ray;
		Color	accumulatedColor(0.0, 0.0, 0.0);
		Color	throughput(1.0, 1.0, 1.0);
		bool	previousBounceSpecular = true;
			Vector3	previousScatterOrigin;
			double	previousScatterPDF = 0.0;
			bool	previousScatterSampledVisibleInfiniteLight = false;
		VolumeGuidingField* trainingGuide = scene.getVolumeGuidingField()
			&& !scene.getVolumeGuidingField()->isFrozen()
			? scene.getVolumeGuidingField().get()
			: nullptr;
		bool pendingVolumeTraining = false;
		Vector3 pendingVolumePosition;
		Vector3 pendingVolumeDirection;

		for (int bounces = 0; bounces <= maxLightBounces; bounces++)
		{
			Sampler::setBounce(static_cast<std::uint32_t>(bounces));
			HitRecord	hitRecord;
			if (!Renderer::internal::_checkHits(scene, currentRay, hitRecord))
			{
				if (bounces == 0 && primaryFeatures != nullptr && renderCamera != nullptr)
				{
					*primaryFeatures = primaryMissFeatures(scene, *renderCamera, currentRay, x, y);
				}
				double environmentMISWeight = 1.0;
				if (
					bounces > 0
					&& !previousBounceSpecular
					&& previousScatterSampledVisibleInfiniteLight
				)
				{
					environmentMISWeight = powerHeuristic(
						previousScatterPDF,
						infiniteLightPDF(scene, previousScatterOrigin, currentRay.getDirection())
					);
				}
				Color skyColor(0.0, 0.0, 0.0);
				switch (skyType)
				{
					case SKY_ATMOSPHERE:
						skyColor = sampleAtmosphereSky(scene, currentRay, environmentMISWeight);
						break;
					case SKY_ENVIRONMENT:
						skyColor = sampleSceneSky(scene, currentRay) * environmentMISWeight;
						break;
					case SKY_LINEAR:
						skyColor = Renderer::internal::_calculateSkyInterpolation(scene, currentRay);
						break;
					default:
						skyColor = scene.getBackgroundColor() * environmentMISWeight;
						break;
				}

				Color skyContribution = throughput * skyColor;
				if (trainingGuide && pendingVolumeTraining)
				{
					trainingGuide->record(
						pendingVolumePosition,
						pendingVolumeDirection,
						Utilities::luminance(skyColor)
					);
				}
				if (bounces > 0 && hasConstantBackgroundLight(scene))
				{
					skyContribution = clampConstantBackgroundMiss(scene, skyContribution);
				}
				return (accumulatedColor + clampRayColor(skyContribution));
			}
			if (bounces == 0 && primaryFeatures != nullptr && renderCamera != nullptr)
			{
				*primaryFeatures = primaryHitFeatures(hitRecord, *renderCamera, x, y);
			}
			if (bounces == 0)
			{
				compositePrimaryAtmosphereSegment(scene, currentRay, hitRecord.t0, accumulatedColor, throughput);
				if (isTerminatedThroughput(throughput))
				{
					return (accumulatedColor);
				}
			}

			ScatterRecord	scatterRecord;
			Color emitted = hitRecord.material->emitted();

			if (!hitRecord.material->scatter(currentRay, hitRecord, scatterRecord))
			{
				if (trainingGuide && pendingVolumeTraining)
				{
					trainingGuide->record(
						pendingVolumePosition,
						pendingVolumeDirection,
						Utilities::luminance(emitted)
					);
				}
				const double previousLightPDF = (!previousBounceSpecular && lightCount > 0)
					? lightPDFValue(lightDistribution, lights, previousScatterOrigin, currentRay.getDirection())
					: 0.0;
				const double emissionMISWeight = previousBounceSpecular
					? 1.0
					: powerHeuristic(previousScatterPDF, previousLightPDF);

				return (accumulatedColor + clampRayColor((throughput * emitted) * emissionMISWeight));
			}
			relocateSubsurfaceExit(scene, hitRecord, scatterRecord);
			const Color attenuation = effectiveScatterAttenuation(scene, hitRecord, scatterRecord);

			if (scatterRecord.isSpecular)
			{
				if (!leavesOpaqueGeometricSurface(hitRecord, scatterRecord.specularRay.getDirection()))
				{
					return (accumulatedColor);
				}
				throughput = clampRayColor(throughput * attenuation);
				if (isTerminatedThroughput(throughput) || !applyRussianRoulette(throughput, bounces))
				{
					return (accumulatedColor);
				}
				currentRay = scatterRecord.specularRay;
				previousBounceSpecular = true;
				previousScatterPDF = 0.0;
				continue;
			}

			accumulatedColor += throughput * emitted;
			Color localRadiance = emitted;
			if (isTerminatedThroughput(throughput * attenuation))
			{
				return (accumulatedColor);
			}
			if (causticPhotonMap)
			{
				const Color causticRadiance = causticPhotonMap->estimate(hitRecord, scatterRecord);
				accumulatedColor += clampRayColor(throughput * causticRadiance);
				localRadiance += causticRadiance;
			}
			Vector3 volumeSkyGuideDirection;
			const double skyGuideLengthSquared = Utilities::vectorLengthSquared(hitRecord.position);
			const bool hasVolumeSkyGuide = skyType == SKY_ATMOSPHERE
				&& skyGuideLengthSquared > 1e-12
				&& std::isfinite(skyGuideLengthSquared);
			if (hasVolumeSkyGuide)
				volumeSkyGuideDirection = hitRecord.position / std::sqrt(skyGuideLengthSquared);

			if (lightCount > 0)
			{
				const bool replaceThisCloudEvent = replacePrimaryDirectionalScattering
					&& bounces == 0
					&& std::find(
						primaryCloudMaterials.begin(),
						primaryCloudMaterials.end(),
						hitRecord.material
					) != primaryCloudMaterials.end();
				const Color directRadiance = estimateDirectLighting(
						scene,
						lights,
						lightDistribution,
						hitRecord,
						scatterRecord,
						bounces,
						hasVolumeGuide,
						volumeGuideDirection,
						hasVolumeSkyGuide,
						volumeSkyGuideDirection,
							replaceThisCloudEvent
						);
				accumulatedColor += clampRayColor(throughput * directRadiance);
				localRadiance += directRadiance;
			}
			if (sampledInfiniteLight)
			{
				const Color environmentRadiance = estimateEnvironmentLighting(
						scene,
						hitRecord,
						scatterRecord,
						bounces,
						hasVolumeGuide,
						volumeGuideDirection,
						hasVolumeSkyGuide,
							volumeSkyGuideDirection
						);
				accumulatedColor += clampRayColor(throughput * environmentRadiance);
				localRadiance += environmentRadiance;
			}
			if (trainingGuide && pendingVolumeTraining)
			{
				trainingGuide->record(
					pendingVolumePosition,
					pendingVolumeDirection,
					Utilities::luminance(localRadiance)
				);
				pendingVolumeTraining = false;
			}

			const ScatterDirectionSample directionSample = sampleScatterDirection(
				scatterRecord,
				hitRecord,
				hasVolumeGuide,
				volumeGuideDirection,
				hasVolumeSkyGuide,
				volumeSkyGuideDirection,
				scene.getVolumeGuidingField().get(),
				bounces >= scene.getVolumeGuidingStartBounce()
					? scene.getVolumeGuidingStrength() : 0.0
			);
			Ray scattered = Ray::fromNormalizedDirection(hitRecord.position, directionSample.direction);
			if (!leavesOpaqueGeometricSurface(hitRecord, scattered.getDirection()))
			{
				return (accumulatedColor);
			}
			if (
				directionSample.physicalPDF <= 0.0
				|| directionSample.proposalPDF <= 0.0
				|| !std::isfinite(directionSample.physicalPDF)
				|| !std::isfinite(directionSample.proposalPDF)
			)
			{
				return (accumulatedColor);
			}

			throughput = clampRayColor(
				throughput
				* attenuation
				* (directionSample.physicalPDF / directionSample.proposalPDF)
			);
			const MaterialType materialType = hitRecord.material->getType();
			const bool highAlbedoVolume = materialType == HENYEY_GREENSTEIN || materialType == ISOTROPIC;
			if (trainingGuide && highAlbedoVolume)
			{
				pendingVolumeTraining = true;
				pendingVolumePosition = hitRecord.position;
				pendingVolumeDirection = directionSample.direction;
			}
			if (
				isTerminatedThroughput(throughput)
				|| !applyRussianRoulette(throughput, bounces, highAlbedoVolume)
			)
			{
				return (accumulatedColor);
			}
			currentRay = scattered;
			previousBounceSpecular = false;
			previousScatterOrigin = hitRecord.position;
			previousScatterPDF = directionSample.proposalPDF;
			previousScatterSampledVisibleInfiniteLight = samplesVisibleInfiniteLightForScatter(
				scene,
				skyType,
				scatterRecord
			);
		}

		return (accumulatedColor);
	}
}
