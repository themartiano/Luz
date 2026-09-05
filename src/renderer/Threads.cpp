#include "RendererInternal.hpp"
#include "Defaults.hpp"
#include "ANSIColors.hpp"
#include "Utilities.hpp"
#include "Clock.hpp"
#include "TerminalProgress.hpp"
#include "Blur/Gaussian.hpp"
#include "Denoise/NFOR.hpp"
#include "ColorManagement.hpp"
#include "Random.hpp"
#include "Sampler.hpp"
#include "VolumeGuidingField.hpp"
#include "Hittables/DensityVolume.hpp"
#include <thread>
#include <future>
#include <vector>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <array>
#include <stdexcept>

namespace
{
	using PrimaryRayClass = Renderer::internal::PrimaryRayClass;

	constexpr unsigned int DENOISE_GUIDE_SAMPLE_COUNT = 4;
	constexpr std::uint32_t VOLUME_GUIDING_STREAM = 0x47554944u;

	double	radicalInverse(std::uint64_t index, std::uint32_t base)
	{
		double inverseBase = 1.0 / static_cast<double>(base);
		double place = inverseBase;
		double result = 0.0;
		while (index > 0)
		{
			result += static_cast<double>(index % base) * place;
			index /= base;
			place *= inverseBase;
		}
		return (result);
	}

	bool	volumeGuidingBounds(const Scene& scene, Vector3& minimum, Vector3& maximum)
	{
		bool found = false;
		for (const std::shared_ptr<Hittable>& hittable : scene.getHittables())
		{
			if (!hittable)
				continue;
			const Material* material = hittable->getMaterial();
			const bool volumePhase = material != nullptr
				&& (material->getType() == HENYEY_GREENSTEIN || material->getType() == ISOTROPIC);
			if (dynamic_cast<const DensityVolume*>(hittable.get()) == nullptr && !volumePhase)
				continue;
			AABB bounds;
			if (!hittable->createBoundingBox(bounds))
				continue;
			if (!found)
			{
				minimum = bounds.getMinimum();
				maximum = bounds.getMaximum();
				found = true;
				continue;
			}
			minimum = Vector3(
				std::min(minimum.getX(), bounds.getMinimum().getX()),
				std::min(minimum.getY(), bounds.getMinimum().getY()),
				std::min(minimum.getZ(), bounds.getMinimum().getZ())
			);
			maximum = Vector3(
				std::max(maximum.getX(), bounds.getMaximum().getX()),
				std::max(maximum.getY(), bounds.getMaximum().getY()),
				std::max(maximum.getZ(), bounds.getMaximum().getZ())
			);
		}
		return (found);
	}

	void	trainVolumeGuide(
		Scene& scene,
		const Renderer::internal::RenderCamera& renderCamera,
		std::size_t width,
		std::size_t height,
		std::size_t threadCount
	)
	{
		const int configuredSamples = scene.getVolumeGuidingTrainingSamples();
		scene.setVolumeGuidingField(nullptr);
		if (configuredSamples <= 0 || scene.getVolumeGuidingStrength() <= 0.0 || width == 0 || height == 0)
			return;

		Vector3 minimum;
		Vector3 maximum;
		if (!volumeGuidingBounds(scene, minimum, maximum))
			return;
		constexpr std::size_t MAX_GUIDE_BYTES = 512ull * 1024ull * 1024ull;
		const std::size_t resolution = scene.getVolumeGuidingResolution();
		const std::size_t lobes = scene.getVolumeGuidingLobes();
		const std::size_t estimatedBytes = resolution * resolution * resolution * lobes
			* (sizeof(std::atomic<std::uint64_t>) + sizeof(double))
			+ lobes * sizeof(Vector3);
		if (estimatedBytes > MAX_GUIDE_BYTES)
			throw std::runtime_error("Volume guiding field exceeds the 512 MiB safety limit.");
		auto guide = std::make_shared<VolumeGuidingField>(
			minimum,
			maximum,
			scene.getVolumeGuidingResolution(),
			scene.getVolumeGuidingLobes(),
			scene.getVolumeGuidingAnisotropy()
		);
		scene.setVolumeGuidingField(guide);

		const std::size_t trainingSamples = static_cast<std::size_t>(configuredSamples);
		std::atomic<std::size_t> nextSample(0);
		std::vector<std::future<void>> workers;
		workers.reserve(threadCount);
		for (std::size_t thread = 0; thread < threadCount; thread++)
		{
			workers.push_back(std::async(std::launch::async, [&]() {
				while (true)
				{
					const std::size_t index = nextSample.fetch_add(1, std::memory_order_relaxed);
					if (index >= trainingSamples)
						break;
					const std::uint64_t sequenceIndex = static_cast<std::uint64_t>(index) + 1ull;
					const std::size_t x = std::min(
						width - 1,
						static_cast<std::size_t>(radicalInverse(sequenceIndex, 2) * static_cast<double>(width))
					);
					const std::size_t y = std::min(
						height - 1,
						static_cast<std::size_t>(radicalInverse(sequenceIndex, 3) * static_cast<double>(height))
					);
					Sampler::beginPixelSample(
						x,
						y,
						static_cast<std::uint32_t>(index),
						VOLUME_GUIDING_STREAM
					);
					(void)Renderer::internal::_calculatePixelColor(scene, renderCamera, x, y);
					Sampler::endPixelSample();
				}
			}));
		}
		for (std::future<void>& worker : workers)
			worker.get();
		guide->freeze();
	}

	double	sampleLuminance(Color color);

	void	computeDisplayDiagnostics(const Image& image, SceneRenderStats& stats)
	{
		constexpr std::size_t HISTOGRAM_BINS = 1024;
		constexpr double NEAR_BLACK = 0.01;
		constexpr double NEAR_WHITE = 0.98;
		constexpr double CLIPPED = 1.0 - 1e-9;

		stats.displayDiagnosticsValid = false;
		stats.displayLuminanceP01 = 0.0;
		stats.displayLuminanceP50 = 0.0;
		stats.displayLuminanceP99 = 0.0;
		stats.displayNearBlackPixelFraction = 0.0;
		stats.displayNearWhitePixelFraction = 0.0;
		stats.displayClippedPixelFraction = 0.0;
		if (image.getColorEncoding() != ImageColorEncoding::DisplayEncodedSRGB)
		{
			return;
		}

		const std::size_t pixelCount = image.getWidth() * image.getHeight();
		if (pixelCount == 0)
		{
			return;
		}

		std::array<std::size_t, HISTOGRAM_BINS> histogram{};
		std::size_t nearBlackCount = 0;
		std::size_t nearWhiteCount = 0;
		std::size_t clippedCount = 0;
		for (std::size_t index = 0; index < pixelCount; index++)
		{
			const Color pixel = image.pixels()[index];
			const double luminance = std::clamp(
				0.2126 * pixel.getRed()
					+ 0.7152 * pixel.getGreen()
					+ 0.0722 * pixel.getBlue(),
				0.0,
				1.0
			);
			const std::size_t bin = std::min(
				HISTOGRAM_BINS - 1,
				static_cast<std::size_t>(luminance * static_cast<double>(HISTOGRAM_BINS - 1))
			);
			histogram[bin]++;
			nearBlackCount += luminance < NEAR_BLACK;
			nearWhiteCount += luminance > NEAR_WHITE;
			clippedCount += (
				pixel.getRed() >= CLIPPED
				|| pixel.getGreen() >= CLIPPED
				|| pixel.getBlue() >= CLIPPED
			);
		}

		auto percentile = [&histogram, pixelCount](double fraction) {
			const std::size_t target = std::min(
				pixelCount - 1,
				static_cast<std::size_t>(fraction * static_cast<double>(pixelCount - 1))
			);
			std::size_t cumulative = 0;
			for (std::size_t bin = 0; bin < HISTOGRAM_BINS; bin++)
			{
				cumulative += histogram[bin];
				if (cumulative > target)
				{
					return (static_cast<double>(bin) / static_cast<double>(HISTOGRAM_BINS - 1));
				}
			}
			return (1.0);
		};

		stats.displayDiagnosticsValid = true;
		stats.displayLuminanceP01 = percentile(0.01);
		stats.displayLuminanceP50 = percentile(0.50);
		stats.displayLuminanceP99 = percentile(0.99);
		stats.displayNearBlackPixelFraction = static_cast<double>(nearBlackCount)
			/ static_cast<double>(pixelCount);
		stats.displayNearWhitePixelFraction = static_cast<double>(nearWhiteCount)
			/ static_cast<double>(pixelCount);
		stats.displayClippedPixelFraction = static_cast<double>(clippedCount)
			/ static_cast<double>(pixelCount);
	}

	struct	DenoiseHalfAccumulator
	{
		Color	colorSum;
		Denoise::FeatureVector	featureSum;
		unsigned int	count = 0;
	};

	struct	AdaptiveAccumulator
	{
		Color	colorSum;
		Color	colorSquareSum;
		double	luminanceSum = 0.0;
		double	luminanceSquareSum = 0.0;
		double	maxLuminance = 0.0;

		void	add(Color color)
		{
			const double luminance = sampleLuminance(color);

			this->colorSum += color;
			this->colorSquareSum += Color(
				color.getRed() * color.getRed(),
				color.getGreen() * color.getGreen(),
				color.getBlue() * color.getBlue()
			);
			this->luminanceSum += luminance;
			this->luminanceSquareSum += luminance * luminance;
			this->maxLuminance = std::max(this->maxLuminance, luminance);
		}
	};

	void	applyBloom(Image& image)
	{
		auto brightnessImage = image.extractBloom(D_BLOOM_THRESHOLD, D_BLOOM_SOFT_KNEE);
		Image bloomImage(image.getWidth(), image.getHeight());

		bloomImage.initialize();
		Gaussian::blur(*brightnessImage, bloomImage, 9, 2.0);
		Color* imagePixels = image.pixels();
		const Color* bloomPixels = bloomImage.pixels();
		const std::size_t width = image.getWidth();
		const std::size_t height = image.getHeight();
		for (std::size_t y = 0; y < height; y++)
		{
			for (std::size_t x = 0; x < width; x++)
			{
				const std::size_t index = y * width + x;

				imagePixels[index] += bloomPixels[index] * D_BLOOM_INTENSITY;
			}
		}
	}

	void	applyPostProcessing(Scene& scene, Image& image)
	{
		image.applyExposure(scene.getExposure());
		if (scene.getBloom())
		{
			applyBloom(image);
		}
		const ViewTransform viewTransform = scene.getViewTransform();
		if (viewTransform == ViewTransform::Raw)
		{
			return;
		}
		if (scene.getContrast() == D_CONTRAST)
		{
			image.applyViewTransformAndEncodeSRGB(viewTransform);
			image.suppressIsolatedFireflies();
			return;
		}
		image.applyViewTransform(viewTransform);
		image.applyContrast(scene.getContrast());
		image.gammaCorrect();
		image.suppressIsolatedFireflies();
	}

	void	updateDenoiseProgress(unsigned int percentage, void* userData)
	{
		TerminalProgress::PhaseProgress* progress =
			static_cast<TerminalProgress::PhaseProgress*>(userData);

		if (progress != nullptr)
		{
			progress->update(percentage);
		}
	}

	Color	cleanColor(Color color)
	{
		if (!std::isfinite(color.getRed()))
		{
			color.setRed(0.0);
		}
		if (!std::isfinite(color.getGreen()))
		{
			color.setGreen(0.0);
		}
		if (!std::isfinite(color.getBlue()))
		{
			color.setBlue(0.0);
		}
		return (color);
	}

	double	sampleLuminance(Color color)
	{
		const double luminance = Utilities::luminance(color);

		if (!std::isfinite(luminance))
		{
			return (0.0);
		}
		return (luminance);
	}

	double	sampleMeanVariance(double sum, double squareSum, unsigned int count)
	{
		if (count <= 1)
		{
			return (0.0);
		}

		const double n = static_cast<double>(count);
		const double variance = (squareSum - (sum * sum / n)) / (n - 1.0);

		return (std::max(0.0, variance / n));
	}

	double	confidenceInterval95(double sum, double squareSum, unsigned int count)
	{
		const double meanVariance = sampleMeanVariance(sum, squareSum, count);

		if (!std::isfinite(meanVariance))
		{
			return (std::numeric_limits<double>::infinity());
		}
		return (1.96 * std::sqrt(meanVariance));
	}

	bool	channelConverged(double mean, double confidenceInterval, double threshold)
	{
		constexpr double DISPLAY_ERROR_FLOOR = 0.02;
		const double target = threshold * std::max(std::fabs(mean), DISPLAY_ERROR_FLOOR);

		return (confidenceInterval <= target);
	}

	PrimaryRayClass	mergePrimaryRayClass(PrimaryRayClass current, PrimaryRayClass sample)
	{
		return (
			static_cast<int>(sample) > static_cast<int>(current)
				? sample
				: current
		);
	}

	unsigned int	adaptiveMinimumSamples(const Scene& scene, PrimaryRayClass primaryClass)
	{
		int configuredMinimum = scene.getAdaptiveMinSamples();

		if (
			primaryClass == PrimaryRayClass::Background
			&& scene.getAdaptiveBackgroundMinSamples() > 0
		)
		{
			configuredMinimum = scene.getAdaptiveBackgroundMinSamples();
		}
		else if (
			primaryClass == PrimaryRayClass::Volume
			&& scene.getAdaptiveVolumeMinSamples() > 0
		)
		{
			configuredMinimum = scene.getAdaptiveVolumeMinSamples();
		}
		if (!scene.getVolumeReference())
		{
			configuredMinimum = std::max(
				configuredMinimum,
				static_cast<int>(scene.getVolumePrimarySamples())
			);
		}
		return (static_cast<unsigned int>(
			std::min(configuredMinimum, scene.getSampleCount())
		));
	}

	unsigned int	darkMinimumSamples(const Scene& scene, unsigned int minSamples)
	{
		const unsigned int maxSamples = static_cast<unsigned int>(scene.getSampleCount());
		const unsigned int checkInterval = static_cast<unsigned int>(scene.getAdaptiveCheckInterval());
		const unsigned int darkMin = std::max(256u, minSamples + (3u * checkInterval));

		return (std::min(maxSamples, std::max(minSamples, darkMin)));
	}

	bool	adaptiveSampleConverged(
		const Scene& scene,
		unsigned int samplesUsed,
		const AdaptiveAccumulator& accumulator,
		PrimaryRayClass primaryClass
	)
	{
		const unsigned int maxSamples = static_cast<unsigned int>(scene.getSampleCount());

		if (samplesUsed >= maxSamples)
		{
			return (true);
		}

		const unsigned int minSamples = adaptiveMinimumSamples(scene, primaryClass);
		if (samplesUsed < minSamples)
		{
			return (false);
		}

		const unsigned int checkInterval = static_cast<unsigned int>(scene.getAdaptiveCheckInterval());
		if ((samplesUsed - minSamples) % checkInterval != 0)
		{
			return (false);
		}
		if (samplesUsed <= 1)
		{
			return (false);
		}

		const double n = static_cast<double>(samplesUsed);
		const double luminanceMean = accumulator.luminanceSum / n;
		const double threshold = scene.getAdaptiveThreshold();

		if (
			primaryClass != PrimaryRayClass::Background
			&&
			accumulator.maxLuminance < 0.02
			&& samplesUsed < darkMinimumSamples(scene, minSamples)
		)
		{
			return (false);
		}

		const double luminanceCI = confidenceInterval95(
			accumulator.luminanceSum,
			accumulator.luminanceSquareSum,
			samplesUsed
		);
		const double redCI = confidenceInterval95(
			accumulator.colorSum.getRed(),
			accumulator.colorSquareSum.getRed(),
			samplesUsed
		);
		const double greenCI = confidenceInterval95(
			accumulator.colorSum.getGreen(),
			accumulator.colorSquareSum.getGreen(),
			samplesUsed
		);
		const double blueCI = confidenceInterval95(
			accumulator.colorSum.getBlue(),
			accumulator.colorSquareSum.getBlue(),
			samplesUsed
		);

		return (
			channelConverged(luminanceMean, luminanceCI, threshold)
			&& channelConverged(accumulator.colorSum.getRed() / n, redCI, threshold)
			&& channelConverged(accumulator.colorSum.getGreen() / n, greenCI, threshold)
			&& channelConverged(accumulator.colorSum.getBlue() / n, blueCI, threshold)
		);
	}

	bool	adaptiveSamplingCanStop(const Scene& scene)
	{
		const unsigned int backgroundMinimum = adaptiveMinimumSamples(
			scene,
			PrimaryRayClass::Background
		);
		const unsigned int surfaceMinimum = adaptiveMinimumSamples(
			scene,
			PrimaryRayClass::Surface
		);
		const unsigned int volumeMinimum = adaptiveMinimumSamples(
			scene,
			PrimaryRayClass::Volume
		);
		const unsigned int minimum = std::min(
			backgroundMinimum,
			std::min(surfaceMinimum, volumeMinimum)
		);
		return (
			scene.getAdaptiveSampling()
			&& minimum < static_cast<unsigned int>(scene.getSampleCount())
		);
	}

	void	addDenoiseFeatureSample(
		DenoiseHalfAccumulator& half,
		Denoise::FeatureVector& featureSum,
		Denoise::FeatureVector& featureSquareSum,
		const Denoise::FeatureVector& feature
	)
	{
		for (std::size_t i = 0; i < Denoise::NFOR_FEATURE_COUNT; i++)
		{
			const double value = feature[i];

			half.featureSum[i] += value;
			featureSum[i] += value;
			featureSquareSum[i] += value * value;
		}
	}

	Denoise::FeatureVector	meanFeature(
		const Denoise::FeatureVector& sum,
		unsigned int count,
		const Denoise::FeatureVector& fallback
	)
	{
		if (count == 0)
		{
			return (fallback);
		}

		Denoise::FeatureVector result;
		for (std::size_t i = 0; i < Denoise::NFOR_FEATURE_COUNT; i++)
		{
			result[i] = sum[i] / static_cast<double>(count);
		}
		return (result);
	}

	double	colorVariance(Color sum, Color squareSum, unsigned int count)
	{
		return (
			sampleMeanVariance(sum.getRed(), squareSum.getRed(), count)
			+ sampleMeanVariance(sum.getGreen(), squareSum.getGreen(), count)
			+ sampleMeanVariance(sum.getBlue(), squareSum.getBlue(), count)
		) / 3.0;
	}

	Denoise::FeatureVector	featureVariance(
		const Denoise::FeatureVector& sum,
		const Denoise::FeatureVector& squareSum,
		unsigned int count
	)
	{
		Denoise::FeatureVector result;

		for (std::size_t i = 0; i < Denoise::NFOR_FEATURE_COUNT; i++)
		{
			result[i] = sampleMeanVariance(sum[i], squareSum[i], count);
		}
		return (result);
	}

	Color	meanColor(Color sum, unsigned int count, Color fallback)
	{
		if (count == 0)
		{
			return (fallback);
		}
		return (sum / static_cast<double>(count));
	}

	void	storeDenoisePixel(
		Scene& scene,
		std::size_t x,
		std::size_t y,
		const DenoiseHalfAccumulator& halfA,
		const DenoiseHalfAccumulator& halfB,
		Color colorSum,
		Color colorSquareSum,
		const Denoise::FeatureVector& featureSum,
		const Denoise::FeatureVector& featureSquareSum,
		unsigned int sampleCount
	)
	{
		Denoise::NFORBuffers* buffers = scene.getDenoiseBuffers();
		if (buffers == nullptr)
		{
			return;
		}

		const std::size_t index = buffers->index(x, y);
		const Color fallbackColor = meanColor(colorSum, sampleCount, Color());
		const Denoise::FeatureVector fallbackFeature = meanFeature(featureSum, sampleCount, Denoise::FeatureVector());

		buffers->colorA[index] = meanColor(halfA.colorSum, halfA.count, fallbackColor);
		buffers->colorB[index] = meanColor(halfB.colorSum, halfB.count, fallbackColor);
		buffers->colorVariance[index] = colorVariance(colorSum, colorSquareSum, sampleCount);
		buffers->sampleCount[index] = sampleCount;
		buffers->featuresA[index] = meanFeature(halfA.featureSum, halfA.count, fallbackFeature);
		buffers->featuresB[index] = meanFeature(halfB.featureSum, halfB.count, fallbackFeature);
		buffers->featureVariance[index] = featureVariance(featureSum, featureSquareSum, sampleCount);
	}
}

void	Renderer::internal::_manageThreads(Scene& scene)
{
	const std::size_t	height = scene.getImage()->getHeight();
	const std::size_t	width = scene.getImage()->getWidth();
	const std::size_t	pixelTotal = width * height;
	const std::size_t	threadCount = std::max<std::size_t>(1, scene.getRenderingThreads());
	const std::size_t	blockSize = 16;
	const RenderCamera	renderCamera = _prepareRenderCamera(scene);
	const std::uint32_t	renderSeed = hasRandomSeed()
		? static_cast<std::uint32_t>(randomSeedValue())
		: randomEngine.integer();
	SceneRenderStats	stats = scene.getRenderStats();

	Sampler::setRenderSeed(renderSeed);
	stats.renderedSamples = 0;
	stats.averageSamplesPerPixel = 0.0;
	stats.renderMS = 0.0;
	stats.volumeGuideMS = 0.0;
	stats.denoiseMS = 0.0;
	stats.postProcessMS = 0.0;
	stats.totalMS = 0.0;
	stats.displayDiagnosticsValid = false;
	scene.setRenderStats(stats);
	scene.clearDenoisedImage();
	Clock guideClock;
	guideClock.start();
	trainVolumeGuide(scene, renderCamera, width, height, threadCount);
	if (scene.getVolumeGuidingField())
		stats.volumeGuideMS = guideClock.elapsedMS();
	scene.setRenderStats(stats);
	if (scene.getDenoise())
	{
		scene.initializeDenoiseBuffers(width, height);
	}
	else
	{
		scene.clearDenoiseBuffers();
	}

	std::atomic<std::size_t> nextRenderPixel(0);
	std::atomic<std::size_t> completedRenderPixels(0);
	std::atomic<std::size_t> completedRenderSamples(0);
	std::vector<std::future<void>> futureVector;
	futureVector.reserve(threadCount);

	Clock renderClock;
	renderClock.start();
	if (!scene.getBenchmarkMode())
	{
		std::cout << std::endl;
	}
	TerminalProgress::PhaseProgress renderProgress(
		"Render",
		!scene.getBenchmarkMode()
	);

	// Creates threads.
	for (std::size_t i = 0; i < threadCount; i++)
	{
		futureVector.push_back(
			std::async(std::launch::async, [&scene, &renderCamera, &nextRenderPixel, &completedRenderPixels, &completedRenderSamples, width, pixelTotal, i]()
			{
				Clock		pixelClock;
				const bool	storePixelRenderTimes = scene.getStorePixelRenderTimes();

				if (hasRandomSeed())
				{
					randomEngine.seed(randomSeedForThread(i));
				}

				while (true)
				{
					const std::size_t startIndex = nextRenderPixel.fetch_add(blockSize);
					if (startIndex >= pixelTotal)
					{
						break;
					}

					const std::size_t stopIndex = std::min(startIndex + blockSize, pixelTotal);
					std::size_t blockSampleCount = 0;
					for (std::size_t index = startIndex; index < stopIndex; index++)
					{
						std::size_t	x = index % width;
						std::size_t	y = index / width;

						pixelClock.start();
						blockSampleCount += _threadRender(scene, renderCamera, x, y);

						if (storePixelRenderTimes)
						{
							scene.setPixelRenderTime(x, y, pixelClock.elapsedUS());
						}
					}
					completedRenderSamples.fetch_add(blockSampleCount);
					completedRenderPixels.fetch_add(stopIndex - startIndex);
				}
			}
		));
	}

	if (scene.getBenchmarkMode())
	{
		for (std::future<void>& future : futureVector)
		{
			future.get();
		}
	}
	else
	{
		// The coordinator reports progress on a throttle; workers never print.
		while (true)
		{
			std::size_t localRenderPixel = completedRenderPixels.load();
			if (localRenderPixel >= pixelTotal)
			{
				break;
			}

			int percentage = (double(localRenderPixel) / double(pixelTotal)) * 100.0;
			renderProgress.update(static_cast<unsigned int>(percentage));

			bool allWorkersFinished = true;
			for (std::future<void>& future : futureVector)
			{
				if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
				{
					allWorkersFinished = false;
					break;
				}
			}
			if (allWorkersFinished)
			{
				break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}

		for (std::future<void>& future : futureVector)
		{
			future.get();
		}
	}

	stats.renderMS = renderClock.elapsedMS();
	stats.renderedSamples = completedRenderSamples.load();
	if (pixelTotal > 0)
	{
		stats.averageSamplesPerPixel = static_cast<double>(stats.renderedSamples)
			/ static_cast<double>(pixelTotal);
	}

	if (!scene.getBenchmarkMode())
		{
			renderProgress.finish(stats.renderMS);
			if (scene.getAdaptiveSampling() && pixelTotal > 0)
			{
				const double averageSamples = static_cast<double>(completedRenderSamples.load())
					/ static_cast<double>(pixelTotal);
				std::cout
					<< std::endl
					<< CLR_GREEN_BRIGHT << "Average samples per pixel: "
					<< CLR_WHITE << averageSamples
					<< CLR_BLUE_BRIGHT << " / " << scene.getSampleCount()
				<< CLR_RESET << std::endl;
		}
	}

	std::unique_ptr<Image> denoisedImage;
	if (scene.getDenoise() && scene.getDenoiseBuffers() != nullptr)
	{
		Denoise::NFORSettings nforSettings;
		Clock denoiseClock;
		if (!scene.getBenchmarkMode())
		{
			std::cout << std::endl;
		}
		TerminalProgress::PhaseProgress denoiseProgress(
			"Denoise",
			!scene.getBenchmarkMode()
		);

		nforSettings.threadCount = scene.getRenderingThreads();
		// Keep the default two-pixel regression radius at low sample counts too.
		// A wider window removes the small cloudlets that the deterministic volume
		// feature pass was specifically designed to preserve.
		if (!scene.getBenchmarkMode())
		{
			nforSettings.progressCallback = updateDenoiseProgress;
			nforSettings.progressUserData = &denoiseProgress;
		}
		denoiseClock.start();
		const Denoise::NFORBuffers* denoiseBuffers = scene.getDenoiseBuffers();
		denoisedImage = Denoise::applyNFOR(*denoiseBuffers, nforSettings);
		if (
			denoisedImage
			&& denoiseBuffers->deterministicColor.size()
				== denoiseBuffers->width * denoiseBuffers->height
		)
		{
			for (std::size_t y = 0; y < denoiseBuffers->height; y++)
			{
				for (std::size_t x = 0; x < denoiseBuffers->width; x++)
				{
					const std::size_t index = denoiseBuffers->index(x, y);
					denoisedImage->setPixelUnchecked(
						x,
						y,
						cleanColor(
							denoisedImage->getPixelUnchecked(x, y)
							+ denoiseBuffers->deterministicColor[index]
						)
					);
				}
			}
		}
		stats.denoiseMS = denoiseClock.elapsedMS();
		if (!scene.getBenchmarkMode())
		{
			denoiseProgress.finish(stats.denoiseMS);
		}
		scene.clearDenoiseBuffers();
	}
	else
	{
		stats.denoiseMS = 0.0;
	}

	Clock postProcessClock;
	if (!scene.getBenchmarkMode() && denoisedImage == nullptr)
	{
		std::cout << std::endl;
	}
	TerminalProgress::PhaseProgress postProcessProgress(
		"Post process",
		!scene.getBenchmarkMode()
	);
	postProcessClock.start();
	if (denoisedImage != nullptr)
	{
		applyPostProcessing(scene, *denoisedImage);
		postProcessProgress.update(50);
		scene.setDenoisedImage(std::move(denoisedImage));
	}
	applyPostProcessing(scene, *scene.getImage());
	computeDisplayDiagnostics(*scene.getImage(), stats);
	stats.postProcessMS = postProcessClock.elapsedMS();
	if (!scene.getBenchmarkMode())
	{
		postProcessProgress.finish(stats.postProcessMS);
	}
	scene.setRenderStats(stats);
}

// Renders the pixel color at X, Y
unsigned int	Renderer::internal::_threadRender(Scene& scene, const RenderCamera& renderCamera, std::size_t x, std::size_t y)
{
	const unsigned int	sampleCount = static_cast<unsigned int>(scene.getSampleCount());
	const bool			adaptiveSampling = adaptiveSamplingCanStop(scene);
	Denoise::NFORBuffers* denoiseBuffers = scene.getDenoiseBuffers();

	if (denoiseBuffers == nullptr)
	{
		Color pixelColor(0.0, 0.0, 0.0);
		Color primarySingleScatteringSum(0.0, 0.0, 0.0);
		unsigned int primarySingleScatteringCount = 0;
		AdaptiveAccumulator adaptiveAccumulator;
		PrimaryRayClass primaryClass = PrimaryRayClass::Background;
		unsigned int samplesUsed = 0;

		for (unsigned int samples = 0; samples < sampleCount; samples++)
		{
			Sampler::beginPixelSample(x, y, samples);
			Color sampleColor;
			if (samples < scene.getVolumePrimarySamples())
			{
				const RenderSample sample = _calculatePixelSample(scene, renderCamera, x, y, true);
				sampleColor = cleanColor(sample.color);
				primarySingleScatteringSum += cleanColor(sample.primarySingleScattering);
				primarySingleScatteringCount++;
				primaryClass = mergePrimaryRayClass(primaryClass, sample.primaryClass);
			}
			else
				sampleColor = cleanColor(_calculatePixelColor(scene, renderCamera, x, y));
			Sampler::endPixelSample();

			pixelColor += sampleColor;
			samplesUsed = samples + 1;
			if (adaptiveSampling)
			{
				adaptiveAccumulator.add(sampleColor);
				if (adaptiveSampleConverged(
					scene,
					samplesUsed,
					adaptiveAccumulator,
					primaryClass
				))
				{
					break;
				}
			}
		}
		if (primarySingleScatteringCount > 0)
		{
			pixelColor += primarySingleScatteringSum
				* (static_cast<double>(samplesUsed) / static_cast<double>(primarySingleScatteringCount));
		}
		pixelColor /= static_cast<double>(samplesUsed);
		scene.getImage()->setPixelUnchecked(x, y, cleanColor(pixelColor));
		return (samplesUsed);
	}

	Color pixelColor(0.0, 0.0, 0.0);
	Color pixelColorSquare(0.0, 0.0, 0.0);
	Color primarySingleScatteringSum(0.0, 0.0, 0.0);
	double primaryVolumeOpacitySum = 0.0;
	unsigned int primarySingleScatteringCount = 0;
	AdaptiveAccumulator adaptiveAccumulator;
	PrimaryRayClass primaryClass = PrimaryRayClass::Background;
	Denoise::FeatureVector featureSum;
	Denoise::FeatureVector featureSquareSum;
	DenoiseHalfAccumulator halfA;
	DenoiseHalfAccumulator halfB;
	unsigned int samplesUsed = 0;

	for (unsigned int samples = 0; samples < sampleCount; samples++)
	{
		const unsigned int halfIndex = samples % 2;
		DenoiseHalfAccumulator& half = (halfIndex == 0) ? halfA : halfB;
		Color sampleColor;
		Denoise::FeatureVector sampleFeatures;

		Sampler::beginPixelSample(x, y, samples, halfIndex + 1);
		if (samples < std::max(DENOISE_GUIDE_SAMPLE_COUNT, scene.getVolumePrimarySamples()))
		{
			const bool calculatePrimarySingleScattering = samples < scene.getVolumePrimarySamples();
			const RenderSample sample = _calculatePixelSample(
				scene,
				renderCamera,
				x,
				y,
				calculatePrimarySingleScattering
			);
			sampleColor = cleanColor(sample.color);
			sampleFeatures = sample.features;
			primaryClass = mergePrimaryRayClass(primaryClass, sample.primaryClass);
			if (calculatePrimarySingleScattering)
			{
				primarySingleScatteringSum += cleanColor(sample.primarySingleScattering);
				primaryVolumeOpacitySum += std::clamp(
					sample.primaryVolumeOpacity,
					0.0,
					1.0
				);
				primarySingleScatteringCount++;
			}
		}
		else
		{
			sampleColor = cleanColor(_calculatePixelColor(scene, renderCamera, x, y));
			sampleFeatures = meanFeature(half.featureSum, half.count, Denoise::FeatureVector());
		}
		Sampler::endPixelSample();

		half.colorSum += sampleColor;
		half.count++;
		addDenoiseFeatureSample(half, featureSum, featureSquareSum, sampleFeatures);
		pixelColor += sampleColor;
		pixelColorSquare += Color(
			sampleColor.getRed() * sampleColor.getRed(),
			sampleColor.getGreen() * sampleColor.getGreen(),
			sampleColor.getBlue() * sampleColor.getBlue()
		);
		samplesUsed = samples + 1;
		if (adaptiveSampling)
		{
			adaptiveAccumulator.add(sampleColor);
			if (adaptiveSampleConverged(
				scene,
				samplesUsed,
				adaptiveAccumulator,
				primaryClass
			))
			{
				break;
			}
		}
	}
	const Color control = primarySingleScatteringCount > 0
		? primarySingleScatteringSum / static_cast<double>(primarySingleScatteringCount)
		: Color(0.0, 0.0, 0.0);
	const Color pixelColorSum = pixelColor;
	storeDenoisePixel(
		scene,
		x,
		y,
		halfA,
		halfB,
		pixelColorSum,
		pixelColorSquare,
		featureSum,
		featureSquareSum,
		samplesUsed
	);
	if (denoiseBuffers->deterministicColor.size() > denoiseBuffers->index(x, y))
		denoiseBuffers->deterministicColor[denoiseBuffers->index(x, y)] = control;
	if (denoiseBuffers->volumeOpacity.size() > denoiseBuffers->index(x, y))
	{
		denoiseBuffers->volumeOpacity[denoiseBuffers->index(x, y)]
			= primarySingleScatteringCount > 0
				? primaryVolumeOpacitySum
					/ static_cast<double>(primarySingleScatteringCount)
				: 0.0;
	}
	pixelColor = cleanColor(pixelColor / static_cast<double>(samplesUsed) + control);

	scene.getImage()->setPixelUnchecked(x, y, pixelColor);
	return (samplesUsed);
}
