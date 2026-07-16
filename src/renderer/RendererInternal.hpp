#pragma once

#include "Color.hpp"
#include "Denoise/NFOR.hpp"
#include "Ray/Ray.hpp"
#include "Scene/Scene.hpp"
#include "Vector3.hpp"
#include <cstddef>

namespace Renderer::internal
{
	enum class	PrimaryRayClass
	{
		Background,
		Surface,
		Volume
	};

	struct	RenderCamera
	{
		double	width = 0.0;
		double	height = 0.0;
		double	inverseWidthMinusOne = 0.0;
		double	inverseHeightMinusOne = 0.0;
		double	lensRadius = 0.0;
		Vector3	position;
		Vector3	u;
		Vector3	v;
		Vector3	horizontal;
		Vector3	vertical;
		Vector3	lowerLeftCorner;
	};

	struct	RenderSample
	{
		Color	color;
		Color	primarySingleScattering;
		double	primaryVolumeOpacity = 0.0;
		PrimaryRayClass	primaryClass = PrimaryRayClass::Background;
		Denoise::FeatureVector	features;
	};

	unsigned int	_threadRender(Scene& scene, const RenderCamera& renderCamera, std::size_t x, std::size_t y);
	Color	_calculatePixelColor(Scene& scene, const RenderCamera& renderCamera, std::size_t x, std::size_t y);
	RenderSample	_calculatePixelSample(
		Scene& scene,
		const RenderCamera& renderCamera,
		std::size_t x,
		std::size_t y,
		bool calculatePrimarySingleScattering = true
	);
	bool	_checkHits(Scene& scene, Ray& ray, HitRecord& hitRecord);
	Color	_calculateLightRaysColor(const Ray& ray, Scene& scene);
	Color	_computeAtmosphereColor(Scene& scene, Ray& ray);
	Color	_calculateSkyInterpolation(Scene& scene, Ray& ray);
	void	_manageThreads(Scene& scene);
	RenderCamera	_prepareRenderCamera(Scene& scene);
	Ray		_generateRay(const RenderCamera& renderCamera, std::size_t x, std::size_t y);
}
