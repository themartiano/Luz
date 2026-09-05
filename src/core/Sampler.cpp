#include "Sampler.hpp"
#include "Defaults.hpp"
#include "Random.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace
{
	constexpr double	GOLDEN_RATIO_CONJUGATE = 0.6180339887498948;
	constexpr std::uint32_t	BOUNCE_DIMENSION_STRIDE = 32;
	constexpr std::uint32_t	AUXILIARY_DIMENSION_OFFSET = 0x10000u;
	constexpr std::size_t	CACHED_DIMENSION_COUNT = 1024;
	constexpr std::size_t	CACHED_COMPONENT_COUNT = 2;

	struct	SampleContext
	{
		bool	active = false;
		std::size_t	x = 0;
		std::size_t	y = 0;
		std::uint32_t	sampleIndex = 0;
		std::uint32_t	stream = 0;
		std::uint32_t	bounce = 0;
		bool	featureSampling = false;
		bool	volumeControlSampling = false;
		bool	directionalShadowSampling = false;
		bool referenceVolumeTransport = false;
		std::uint64_t	baseKey = 0;
	};

	std::uint32_t	g_renderSeed = 0x9e3779b9u;
	thread_local SampleContext	g_context;

	std::uint64_t	splitMix64(std::uint64_t value)
	{
		value += 0x9e3779b97f4a7c15ull;
		value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
		value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
		return (value ^ (value >> 31));
	}

	std::uint32_t	hash32(std::uint64_t value)
	{
		return (static_cast<std::uint32_t>(splitMix64(value) >> 32));
	}

	double	unitFromHash(std::uint32_t hash)
	{
		return (static_cast<double>(hash >> 8) * (1.0 / 16777216.0));
	}

	double	unitFromHash(std::uint64_t hash)
	{
		return (static_cast<double>(hash >> 11) * (1.0 / 9007199254740992.0));
	}

	double	fract(double value)
	{
		return (value - std::floor(value));
	}

	double	dimensionStep(std::uint32_t dimension, std::uint32_t component)
	{
		static const std::array<std::array<double, CACHED_COMPONENT_COUNT>, CACHED_DIMENSION_COUNT> cachedSteps = [] {
			std::array<std::array<double, CACHED_COMPONENT_COUNT>, CACHED_DIMENSION_COUNT> steps = {};

			for (std::size_t dimensionIndex = 0; dimensionIndex < CACHED_DIMENSION_COUNT; dimensionIndex++)
			{
				for (std::size_t componentIndex = 0; componentIndex < CACHED_COMPONENT_COUNT; componentIndex++)
				{
					const std::uint64_t key =
						(static_cast<std::uint64_t>(dimensionIndex) + 1ull) * 0x9e3779b97f4a7c15ull
						^ (static_cast<std::uint64_t>(componentIndex) + 1ull) * 0xda942042e4dd58b5ull;
					double step = unitFromHash(hash32(key));

					if (step < 0.05 || step > 0.95)
					{
						step = fract(step + GOLDEN_RATIO_CONJUGATE);
					}
					steps[dimensionIndex][componentIndex] = step;
				}
			}
			return (steps);
		}();

		if (dimension < CACHED_DIMENSION_COUNT && component < CACHED_COMPONENT_COUNT)
		{
			return (cachedSteps[dimension][component]);
		}

		const std::uint64_t key =
			(static_cast<std::uint64_t>(dimension) + 1ull) * 0x9e3779b97f4a7c15ull
			^ (static_cast<std::uint64_t>(component) + 1ull) * 0xda942042e4dd58b5ull;
		double step = unitFromHash(hash32(key));

		if (step < 0.05 || step > 0.95)
		{
			step = fract(step + GOLDEN_RATIO_CONJUGATE);
		}
		return (step);
	}

	std::uint64_t	contextKey(std::uint32_t dimension)
	{
		std::uint64_t key = g_context.baseKey;

		key ^= (static_cast<std::uint64_t>(dimension) + 1ull) * 0x9e3779b97f4a7c15ull;
		return (key);
	}

	std::uint32_t	dimensionForBounce(std::uint32_t dimension)
	{
		return (dimension + (g_context.bounce * BOUNCE_DIMENSION_STRIDE));
	}

	bool	usesProgressiveSequence(std::uint32_t dimension)
	{
		const std::uint32_t baseDimension = dimension % BOUNCE_DIMENSION_STRIDE;

		return (
			baseDimension == Sampler::DIM_CAMERA
			|| baseDimension == Sampler::DIM_LENS
			|| baseDimension == Sampler::DIM_LIGHT_STRATEGY
			|| baseDimension == Sampler::DIM_BSDF_DIRECTION
			|| baseDimension == Sampler::DIM_MATERIAL_DECISION
			|| baseDimension == Sampler::DIM_RUSSIAN_ROULETTE
			|| baseDimension == Sampler::DIM_VOLUME_DISTANCE
			|| baseDimension == Sampler::DIM_LIGHT_SURFACE_SELECTION
			|| baseDimension == Sampler::DIM_LIGHT_SURFACE_POINT
			|| baseDimension == Sampler::DIM_ENVIRONMENT_SELECTION
			|| baseDimension == Sampler::DIM_ENVIRONMENT_POINT
			|| baseDimension == Sampler::DIM_ENVIRONMENT_STRATEGY
			|| baseDimension == Sampler::DIM_VOLUME_GUIDING
			|| baseDimension == Sampler::DIM_VOLUME_ACCEPTANCE
			|| baseDimension == Sampler::DIM_VOLUME_LEARNED_LOBE
			|| baseDimension == Sampler::DIM_VOLUME_LEARNED_DIRECTION
			|| baseDimension == Sampler::DIM_LIGHT_EMISSION_DIRECTION
		);
	}

	double	hashedSample(std::uint32_t dimension, std::uint32_t component)
	{
		std::uint64_t key = contextKey(dimension);

		key ^= (static_cast<std::uint64_t>(g_context.sampleIndex) + 1ull) * 0x94d049bb133111ebull;
		key ^= (static_cast<std::uint64_t>(component) + 1ull) * 0xbf58476d1ce4e5b9ull;
		return (unitFromHash(splitMix64(key)));
	}

	Sampler::Sample2D	fallback2D(void)
	{
		return (Sampler::Sample2D{
			static_cast<float>(randomEngine.doubleFloat()),
			static_cast<float>(randomEngine.doubleFloat())
		});
	}

	double	fallback1D(void)
	{
		return (randomEngine.doubleFloat());
	}
}

void	Sampler::setRenderSeed(std::uint32_t seed)
{
	g_renderSeed = seed;
}

void	Sampler::beginPixelSample(std::size_t x, std::size_t y, std::uint32_t sampleIndex)
{
	beginPixelSample(x, y, sampleIndex, 0);
}

void	Sampler::beginPixelSample(std::size_t x, std::size_t y, std::uint32_t sampleIndex, std::uint32_t stream)
{
	g_context.active = true;
	g_context.x = x;
	g_context.y = y;
	g_context.sampleIndex = sampleIndex;
	g_context.stream = stream;
	g_context.bounce = 0;
	g_context.featureSampling = false;
	g_context.volumeControlSampling = false;
	g_context.directionalShadowSampling = false;
	g_context.referenceVolumeTransport = false;
	g_context.baseKey = g_renderSeed;
	g_context.baseKey ^= (static_cast<std::uint64_t>(x) + 1ull) * 0xbf58476d1ce4e5b9ull;
	g_context.baseKey ^= (static_cast<std::uint64_t>(y) + 1ull) * 0x94d049bb133111ebull;
	g_context.baseKey ^= (static_cast<std::uint64_t>(stream) + 1ull) * 0xda942042e4dd58b5ull;
}

void	Sampler::setBounce(std::uint32_t bounce)
{
	if (g_context.active)
	{
		g_context.bounce = bounce;
	}
}

std::uint32_t Sampler::currentBounce(void)
{
	return (g_context.bounce);
}

void	Sampler::setFeatureSampling(bool enabled)
{
	if (g_context.active)
	{
		g_context.featureSampling = enabled;
	}
}

bool	Sampler::isFeatureSampling(void)
{
	return (g_context.active && g_context.featureSampling);
}

void Sampler::setVolumeControlSampling(bool enabled)
{
	if (g_context.active)
		g_context.volumeControlSampling = enabled;
}

bool Sampler::isVolumeControlSampling(void)
{
	return (g_context.active && g_context.volumeControlSampling);
}

void Sampler::setDirectionalShadowSampling(bool enabled)
{
	if (g_context.active)
		g_context.directionalShadowSampling = enabled;
}

bool Sampler::isDirectionalShadowSampling(void)
{
	return (g_context.active && g_context.directionalShadowSampling);
}

void	Sampler::endPixelSample(void)
{
	g_context.active = false;
}

bool	Sampler::isActive(void)
{
	return (g_context.active);
}

double	Sampler::sample1D(std::uint32_t dimension)
{
	if (!g_context.active)
	{
		return (fallback1D());
	}

	const std::uint32_t dimensionKey = dimensionForBounce(dimension);
	if (!usesProgressiveSequence(dimension))
	{
		return (hashedSample(dimensionKey, 0));
	}
	const double shift = unitFromHash(hash32(contextKey(dimensionKey)));
	const double step = dimensionStep(dimensionKey, 0);

	return (fract(0.5 + (static_cast<double>(g_context.sampleIndex) + 1.0) * step + shift));
}

Sampler::Sample2D	Sampler::sample2D(std::uint32_t dimension)
{
	if (!g_context.active)
	{
		return (fallback2D());
	}

	const std::uint32_t dimensionKey = dimensionForBounce(dimension);
	if (!usesProgressiveSequence(dimension))
	{
		return (Sample2D{
			static_cast<float>(hashedSample(dimensionKey, 0)),
			static_cast<float>(hashedSample(dimensionKey, 1))
		});
	}
	const std::uint64_t key = contextKey(dimensionKey);
	const double shiftX = unitFromHash(hash32(key));
	const double shiftY = unitFromHash(hash32(key ^ 0xda942042e4dd58b5ull));
	const double stepX = dimensionStep(dimensionKey, 0);
	const double stepY = dimensionStep(dimensionKey, 1);
	const double sample = static_cast<double>(g_context.sampleIndex) + 1.0;

	return (Sample2D{
		static_cast<float>(fract(0.5 + sample * stepX + shiftX)),
		static_cast<float>(fract(0.5 + sample * stepY + shiftY))
	});
}

Vector3	Sampler::cosineHemisphere(std::uint32_t dimension)
{
	const Sample2D sample = sample2D(dimension);
	const float phi = static_cast<float>(2.0 * D_PI) * sample.x;
	const float r = std::sqrt(sample.y);
	const float z = std::sqrt(std::max(0.0f, 1.0f - sample.y));

	return (Vector3(r * std::cos(phi), r * std::sin(phi), z));
}

Vector3	Sampler::sphereDirection(std::uint32_t dimension)
{
	const Sample2D sample = sample2D(dimension);
	const float z = 1.0f - (2.0f * sample.y);
	const float r = std::sqrt(std::max(0.0f, 1.0f - z * z));
	const float phi = static_cast<float>(2.0 * D_PI) * sample.x;

	return (Vector3(r * std::cos(phi), r * std::sin(phi), z));
}

Vector3	Sampler::unitBall(std::uint32_t dimension)
{
	const Vector3 direction = sphereDirection(dimension);
	const float radius = std::cbrt(static_cast<float>(sample1D(dimension + AUXILIARY_DIMENSION_OFFSET)));

	return (direction * radius);
}

Vector3	Sampler::unitDisk(std::uint32_t dimension)
{
	const Sample2D sample = sample2D(dimension);
	const float sx = (2.0f * sample.x) - 1.0f;
	const float sy = (2.0f * sample.y) - 1.0f;

	if (sx == 0.0f && sy == 0.0f)
	{
		return (Vector3(0.0, 0.0, 0.0));
	}

	float radius;
	float theta;
	if (std::fabs(sx) > std::fabs(sy))
	{
		radius = sx;
		theta = (static_cast<float>(D_PI) / 4.0f) * (sy / sx);
	}
	else
	{
		radius = sy;
		theta = (static_cast<float>(D_PI) / 2.0f) - ((static_cast<float>(D_PI) / 4.0f) * (sx / sy));
	}

	return (Vector3(radius * std::cos(theta), radius * std::sin(theta), 0.0));
}

void Sampler::setReferenceVolumeTransport(bool enabled)
{
	g_context.referenceVolumeTransport = enabled;
}

bool Sampler::isReferenceVolumeTransport(void)
{
	return g_context.active && g_context.referenceVolumeTransport;
}
