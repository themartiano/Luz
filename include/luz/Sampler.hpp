#pragma once

#include "Vector3.hpp"
#include <cstddef>
#include <cstdint>

namespace Sampler
{
	struct	Sample2D
	{
		float	x = 0.0f;
		float	y = 0.0f;
	};

	enum	Dimension : std::uint32_t
	{
		DIM_CAMERA = 0,
		DIM_LENS = 1,
		DIM_LIGHT_STRATEGY = 2,
		DIM_LIGHT_SELECTION = 3,
		DIM_LIGHT_SURFACE_SELECTION = 4,
		DIM_LIGHT_SURFACE_POINT = 5,
		DIM_BSDF_DIRECTION = 6,
		DIM_MATERIAL_DECISION = 7,
		DIM_MATERIAL_FUZZ = 8,
		DIM_RUSSIAN_ROULETTE = 9,
		DIM_VOLUME_DISTANCE = 10,
		DIM_ATMOSPHERE = 11,
		DIM_ENVIRONMENT_SELECTION = 12,
		DIM_ENVIRONMENT_POINT = 13,
		DIM_ENVIRONMENT_STRATEGY = 14,
		DIM_LIGHT_EMISSION_SIDE = 15,
		DIM_LIGHT_EMISSION_DIRECTION = 16,
		DIM_SUBSURFACE_SPATIAL = 17,
		DIM_SUBSURFACE_PROFILE_COMPONENT = 18,
		DIM_SUBSURFACE_THIN_SIDE = 19,
		DIM_VOLUME_GUIDING = 20,
		DIM_VOLUME_ACCEPTANCE = 21,
		DIM_VOLUME_LEARNED_LOBE = 22,
		DIM_VOLUME_LEARNED_DIRECTION = 23
	};

	void	setRenderSeed(std::uint32_t seed);
	void	beginPixelSample(std::size_t x, std::size_t y, std::uint32_t sampleIndex);
	void	beginPixelSample(std::size_t x, std::size_t y, std::uint32_t sampleIndex, std::uint32_t stream);
	void	setBounce(std::uint32_t bounce);
	std::uint32_t	currentBounce(void);
	void	setFeatureSampling(bool enabled);
	bool	isFeatureSampling(void);
	void	setVolumeControlSampling(bool enabled);
	bool	isVolumeControlSampling(void);
	void	setDirectionalShadowSampling(bool enabled);
	bool	isDirectionalShadowSampling(void);
	void setReferenceVolumeTransport(bool enabled);
	bool isReferenceVolumeTransport(void);
	void	endPixelSample(void);
	bool	isActive(void);

	double	sample1D(std::uint32_t dimension);
	Sample2D	sample2D(std::uint32_t dimension);
	Vector3	cosineHemisphere(std::uint32_t dimension);
	Vector3	sphereDirection(std::uint32_t dimension);
	Vector3	unitBall(std::uint32_t dimension);
	Vector3	unitDisk(std::uint32_t dimension);
}
