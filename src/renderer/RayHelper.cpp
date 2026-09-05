#include "RendererInternal.hpp"
#include "Defaults.hpp"
#include "Sampler.hpp"
#include "Utilities.hpp"
#include <cmath>

namespace
{
	Vector3	cameraUpOrFallback(const Vector3& cameraUpDirection, const Vector3& lookDirection)
	{
		Vector3 viewUp = cameraUpDirection;

		if (
			!std::isfinite(viewUp.getX())
			|| !std::isfinite(viewUp.getY())
			|| !std::isfinite(viewUp.getZ())
			|| Utilities::vectorLengthSquared(viewUp) <= 1e-12
		)
		{
			viewUp = Vector3(0.0, 1.0, 0.0);
		}
		viewUp = Utilities::normalize(viewUp);
		if (Utilities::vectorLengthSquared(Utilities::cross(viewUp, lookDirection)) <= 1e-12)
		{
			viewUp = Vector3(0.0, 0.0, 1.0);
		}
		if (Utilities::vectorLengthSquared(Utilities::cross(viewUp, lookDirection)) <= 1e-12)
		{
			viewUp = Vector3(1.0, 0.0, 0.0);
		}
		return (viewUp);
	}

	void	fitViewportToImageAspect(double& viewportWidth, double& viewportHeight, double imageWidth, double imageHeight)
	{
		const double imageAspect = imageWidth / imageHeight;
		const double viewportAspect = viewportWidth / viewportHeight;

		if (imageAspect > viewportAspect)
		{
			viewportHeight = viewportWidth / imageAspect;
		}
		else if (imageAspect < viewportAspect)
		{
			viewportWidth = viewportHeight * imageAspect;
		}
	}
}

Renderer::internal::RenderCamera	Renderer::internal::_prepareRenderCamera(Scene& scene)
{
	RenderCamera renderCamera;

	renderCamera.width = scene.getImage()->getWidth();
	renderCamera.height = scene.getImage()->getHeight();
	renderCamera.inverseWidth = renderCamera.width > 0.0 ? 1.0 / renderCamera.width : 0.0;
	renderCamera.inverseHeight = renderCamera.height > 0.0 ? 1.0 / renderCamera.height : 0.0;

	const Camera camera = scene.getActiveCamera();

	renderCamera.position = camera.getPosition();
	const Vector3	cameraLookDirection = camera.getDirection();

	const double	focusDistance = camera.getFocusDistanceMeters() / scene.getMetersPerUnit();
	double	viewportWidth = focusDistance * camera.getSensorWidthMeters() / camera.getFocalLengthMeters();
	double	viewportHeight = focusDistance * camera.getSensorHeightMeters() / camera.getFocalLengthMeters();
	fitViewportToImageAspect(viewportWidth, viewportHeight, renderCamera.width, renderCamera.height);

	const Vector3	w = Utilities::normalize(cameraLookDirection);
	const Vector3	viewUp = cameraUpOrFallback(camera.getUpDirection(), w);
	renderCamera.u = Utilities::normalize(Utilities::cross(viewUp, w));
	renderCamera.v = Utilities::cross(w, renderCamera.u);
	renderCamera.lensRadius = camera.getApertureDiameterMeters() / (2.0 * scene.getMetersPerUnit());
	renderCamera.horizontal = renderCamera.u * viewportWidth;
	renderCamera.vertical = renderCamera.v * viewportHeight;
	renderCamera.lowerLeftCorner = renderCamera.position
		+ (renderCamera.horizontal / 2.0)
		+ (renderCamera.vertical / 2.0)
		+ (w * focusDistance);

	return (renderCamera);
}

// Generates a ray for the pixels 'x', 'y'
Ray	Renderer::internal::_generateRay(const RenderCamera& renderCamera, std::size_t x, std::size_t y)
{
	const Sampler::Sample2D cameraSample = Sampler::sample2D(Sampler::DIM_CAMERA);
	double xU = double(static_cast<double>(x) + cameraSample.x) * renderCamera.inverseWidth;
	double yV = double(static_cast<double>(y) + cameraSample.y) * renderCamera.inverseHeight;

	Vector3	offset(0.0, 0.0, 0.0);
	if (renderCamera.lensRadius > 0.0)
	{
		Vector3	rd = Sampler::unitDisk(Sampler::DIM_LENS) * renderCamera.lensRadius;
		offset = renderCamera.u * rd.getX() + renderCamera.v * rd.getY();
	}

	return (
		Ray(renderCamera.position + offset,
			renderCamera.lowerLeftCorner
				- (renderCamera.horizontal * xU)
				- (renderCamera.vertical * yV)
				- renderCamera.position
				- offset)
	);
}
