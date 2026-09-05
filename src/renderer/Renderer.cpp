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
			if (
				scene.getAdaptiveBackgroundMinSamples() > 0
				|| scene.getAdaptiveVolumeMinSamples() > 0
			)
			{
				const int backgroundMinimum = scene.getAdaptiveBackgroundMinSamples() > 0
					? scene.getAdaptiveBackgroundMinSamples()
					: scene.getAdaptiveMinSamples();
				const int volumeMinimum = scene.getAdaptiveVolumeMinSamples() > 0
					? scene.getAdaptiveVolumeMinSamples()
					: scene.getAdaptiveMinSamples();
				std::cout
					<< CLR_GREEN << backgroundMinimum << CLR_BLUE << " background min spp; "
					<< CLR_GREEN << volumeMinimum << CLR_BLUE << " volume min spp;"
					<< CLR_RESET << std::endl;
			}
		}
		std::cout << CLR_GREEN << scene.getImage()->getWidth() << CLR_BLUE << " x " << CLR_GREEN << scene.getImage()->getHeight() << CLR_RESET << std::endl;
	}

	internal::_manageThreads(scene);

	stats = scene.getRenderStats();
	stats.totalMS = stats.modelLoadMS
		+ stats.sceneBuildMS
		+ stats.volumeGuideMS
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
		if (stats.volumeGuideMS > 0.0)
		{
			std::cout
				<< CLR_BLUE_BRIGHT << ", Guide training " << CLR_WHITE
				<< TerminalProgress::formatDuration(stats.volumeGuideMS);
		}
		std::cout << CLR_RESET << "\n\n";
		if (stats.displayDiagnosticsValid)
		{
			std::cout
				<< CLR_GREEN_BRIGHT << "Display luminance p01 / p50 / p99: "
				<< CLR_WHITE << stats.displayLuminanceP01 << " / "
				<< stats.displayLuminanceP50 << " / "
				<< stats.displayLuminanceP99
				<< CLR_BLUE_BRIGHT << "; clipped pixels: "
				<< CLR_WHITE << stats.displayClippedPixelFraction * 100.0 << "%"
				<< CLR_RESET << "\n\n";
		}
	} else {
		std::cout
			<< "stats"
			<< " rendered_samples=" << stats.renderedSamples
			<< " avg_spp=" << stats.averageSamplesPerPixel
			<< " model_load_ms=" << stats.modelLoadMS
			<< " scene_build_ms=" << stats.sceneBuildMS
			<< " volume_guide_ms=" << stats.volumeGuideMS
			<< " render_ms=" << stats.renderMS
			<< " denoise_ms=" << stats.denoiseMS
			<< " postprocess_ms=" << stats.postProcessMS
			<< " total_ms=" << stats.totalMS
			<< " display_valid=" << stats.displayDiagnosticsValid
			<< " display_p01=" << stats.displayLuminanceP01
			<< " display_p50=" << stats.displayLuminanceP50
			<< " display_p99=" << stats.displayLuminanceP99
			<< " display_near_black=" << stats.displayNearBlackPixelFraction
			<< " display_near_white=" << stats.displayNearWhitePixelFraction
			<< " display_clipped=" << stats.displayClippedPixelFraction
			<< std::endl;
		std::cout << stats.totalMS << std::endl;
	}

	return (true);
}
