#include "Renderer/Renderer.hpp"
#include "RendererInternal.hpp"
#include "ANSIColors.hpp"
#include "Clock.hpp"
#include "Defaults.hpp"
#include "Renderer/CausticPhotonMap.hpp"
#include "Random.hpp"
#include "TerminalProgress.hpp"

// Renders the image using all the information present on 'scene'. (Objects, cameras, lights, settings, etc)
bool	Renderer::render(Scene& scene)
{
	if (!scene.hasCamera())
	{
		std::cerr << CLR_RED << "No camera on the Scene." << CLR_RESET << std::endl;
		return (false);
	}

	SceneRenderStats stats = scene.getRenderStats();
	Clock sceneBuildClock;
	TerminalProgress::PhaseProgress sceneBuildProgress(
		"Scene build",
		!scene.getBenchmarkMode()
	);
	sceneBuildClock.start();
	scene.syncAtmosphereSunDirection();
	sceneBuildProgress.update(20);
	scene.updateLights();
	sceneBuildProgress.update(40);
	scene.updateAccelerationStructure();
	sceneBuildProgress.update(70);

	if (scene.getCausticsEnabled() && scene.getCausticPhotonCount() > 0)
	{
		const std::uint32_t renderSeed = hasRandomSeed()
			? static_cast<std::uint32_t>(randomSeedValue())
			: randomEngine.integer();
		auto causticPhotonMap = std::make_shared<CausticPhotonMap>();
		causticPhotonMap->build(scene, renderSeed);
		scene.setCausticPhotonMap(causticPhotonMap);
	}
	else
	{
		scene.setCausticPhotonMap(nullptr);
	}
	stats = scene.getRenderStats();
	stats.sceneBuildMS += sceneBuildClock.elapsedMS();
	scene.setRenderStats(stats);
	sceneBuildProgress.finish(stats.sceneBuildMS);
	if (!scene.getBenchmarkMode())
	{
		std::cout << std::endl << CLR_YELLOW << "Rendering with:" << CLR_RESET << std::endl;
		std::cout << CLR_GREEN << scene.getRenderingThreads() << CLR_BLUE << " threads;" << CLR_RESET << std::endl;
		std::cout << CLR_GREEN << scene.getSampleCount() << CLR_BLUE << " max samples per pixel;" << CLR_RESET << std::endl;
		if (scene.getAdaptiveSampling())
		{
			std::cout
				<< CLR_GREEN << "adaptive" << CLR_BLUE << " sampling; "
				<< CLR_GREEN << scene.getAdaptiveMinSamples() << CLR_BLUE << " min spp; "
				<< CLR_GREEN << scene.getAdaptiveThreshold() << CLR_BLUE << " threshold;"
				<< CLR_RESET << std::endl;
		}
		std::cout << CLR_GREEN << scene.getImage()->getWidth() << CLR_BLUE << " x " << CLR_GREEN << scene.getImage()->getHeight() << CLR_RESET << std::endl;
	}

	internal::_manageThreads(scene);

	stats = scene.getRenderStats();
	stats.totalMS = stats.modelLoadMS
		+ stats.sceneBuildMS
		+ stats.renderMS
		+ stats.denoiseMS
		+ stats.postProcessMS;
	scene.setRenderStats(stats);

	if (!scene.getBenchmarkMode())
	{
		std::cout
			<< std::endl
			<< CLR_GREEN_BRIGHT << "Render done. "
			<< CLR_BLUE_BRIGHT << "Total: " << CLR_WHITE
			<< TerminalProgress::formatDuration(stats.totalMS)
			<< CLR_BLUE_BRIGHT << ", Render " << CLR_WHITE
			<< TerminalProgress::formatDuration(stats.renderMS);
		std::cout << CLR_RESET << "\n\n";
	} else {
		std::cout
			<< "stats"
			<< " rendered_samples=" << stats.renderedSamples
			<< " avg_spp=" << stats.averageSamplesPerPixel
			<< " model_load_ms=" << stats.modelLoadMS
			<< " scene_build_ms=" << stats.sceneBuildMS
			<< " render_ms=" << stats.renderMS
			<< " denoise_ms=" << stats.denoiseMS
			<< " postprocess_ms=" << stats.postProcessMS
			<< " total_ms=" << stats.totalMS
			<< std::endl;
		std::cout << stats.totalMS << std::endl;
	}

	return (true);
}
