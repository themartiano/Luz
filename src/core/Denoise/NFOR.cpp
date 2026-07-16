#include "Denoise/NFOR.hpp"
#include "Materials/MaterialTypes.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <future>
#include <vector>

namespace
{
	constexpr std::size_t	REGRESSION_SIZE = Denoise::NFOR_FEATURE_COUNT + 1;
	using RegressionMatrix = std::array<std::array<double, REGRESSION_SIZE>, REGRESSION_SIZE>;
	using RegressionRGB = std::array<std::array<double, 3>, REGRESSION_SIZE>;
	using RegressionBasis = std::array<double, REGRESSION_SIZE>;

	struct RegressionPair
	{
		std::vector<Color>	first;
		std::vector<Color>	second;
	};

	struct ScalarMapPair
	{
		std::vector<double>	first;
		std::vector<double>	second;
	};

	std::size_t	clampCoordinate(long long value, std::size_t limit)
	{
		if (value < 0)
		{
			return (0);
		}
		if (static_cast<std::size_t>(value) >= limit)
		{
			return (limit - 1);
		}
		return (static_cast<std::size_t>(value));
	}

	template <typename Function>
	void	parallelRows(std::size_t height, std::size_t threadCount, Function function)
	{
		const std::size_t workerCount = std::max<std::size_t>(1, std::min(threadCount, height));
		if (workerCount == 1 || height < 8)
		{
			function(0, height);
			return;
		}

		std::vector<std::future<void>> futures;
		const std::size_t rowsPerWorker = (height + workerCount - 1) / workerCount;

		futures.reserve(workerCount);
		for (std::size_t worker = 0; worker < workerCount; worker++)
		{
			const std::size_t startY = worker * rowsPerWorker;
			const std::size_t stopY = std::min(height, startY + rowsPerWorker);
			if (startY >= stopY)
			{
				break;
			}
			futures.push_back(std::async(std::launch::async, [=, &function] {
				function(startY, stopY);
			}));
		}
		for (std::future<void>& future : futures)
		{
			future.get();
		}
	}

	void	reportProgress(const Denoise::NFORSettings& settings, unsigned int percentage)
	{
		if (settings.progressCallback != nullptr)
		{
			settings.progressCallback(std::min(percentage, 100u), settings.progressUserData);
		}
	}

	double	colorDistanceSquared(const Color& a, const Color& b)
	{
		const double red = a.getRed() - b.getRed();
		const double green = a.getGreen() - b.getGreen();
		const double blue = a.getBlue() - b.getBlue();

		return ((red * red) + (green * green) + (blue * blue)) / 3.0;
	}

	double	colorLuminance(const Color& color)
	{
		const double luminance = Utilities::luminance(color);

		if (!std::isfinite(luminance) || luminance <= 0.0)
		{
			return (0.0);
		}
		return (luminance);
	}

	Color	medianColorByLuminance(std::vector<Color>& colors)
	{
		std::sort(
			colors.begin(),
			colors.end(),
			[](Color a, Color b)
			{
				return (colorLuminance(a) < colorLuminance(b));
			}
		);
		return (colors[colors.size() / 2]);
	}

	bool	isDenoiseFirefly(
		double luminance,
		const std::vector<double>& neighborLuminances,
		unsigned int similarNeighbors
	)
	{
		constexpr double MIN_LUMINANCE = 0.30;
		constexpr double LOCAL_OUTLIER_RATIO = 2.5;
		constexpr double LOCAL_OUTLIER_EXCESS = 0.18;
		constexpr unsigned int MIN_SIMILAR_SUPPORT = 2;

		if (luminance < MIN_LUMINANCE || neighborLuminances.empty())
		{
			return (false);
		}
		if (similarNeighbors >= MIN_SIMILAR_SUPPORT)
		{
			return (false);
		}

		std::vector<double> sorted = neighborLuminances;
		std::sort(sorted.begin(), sorted.end());
		const double localMedian = sorted[sorted.size() / 2];
		return (
			luminance >= localMedian * LOCAL_OUTLIER_RATIO
			&& luminance >= localMedian + LOCAL_OUTLIER_EXCESS
		);
	}

	std::vector<Color>	suppressDenoiseFireflies(
		const std::vector<Color>& image,
		std::size_t width,
		std::size_t height
	)
	{
		constexpr int RADIUS = 2;
		constexpr double SIMILAR_NEIGHBOR_RATIO = 0.65;
		std::vector<Color> result = image;

		if (width < 3 || height < 3 || image.size() != width * height)
		{
			return (result);
		}
		for (std::size_t y = 0; y < height; y++)
		{
			for (std::size_t x = 0; x < width; x++)
			{
				const std::size_t center = y * width + x;
				const double luminance = colorLuminance(image[center]);
				std::vector<double> neighborLuminances;
				std::vector<Color> replacementCandidates;
				unsigned int similarNeighbors = 0;

				neighborLuminances.reserve((RADIUS * 2 + 1) * (RADIUS * 2 + 1) - 1);
				replacementCandidates.reserve(neighborLuminances.capacity());
				for (int offsetY = -RADIUS; offsetY <= RADIUS; offsetY++)
				{
					for (int offsetX = -RADIUS; offsetX <= RADIUS; offsetX++)
					{
						if (offsetX == 0 && offsetY == 0)
						{
							continue;
						}
						const long long sampleX = static_cast<long long>(x) + offsetX;
						const long long sampleY = static_cast<long long>(y) + offsetY;
						if (
							sampleX < 0
							|| sampleY < 0
							|| static_cast<std::size_t>(sampleX) >= width
							|| static_cast<std::size_t>(sampleY) >= height
						)
						{
							continue;
						}
						const Color neighbor = image[static_cast<std::size_t>(sampleY) * width + static_cast<std::size_t>(sampleX)];
						const double neighborLuminance = colorLuminance(neighbor);

						neighborLuminances.push_back(neighborLuminance);
						if (neighborLuminance >= luminance * SIMILAR_NEIGHBOR_RATIO)
						{
							similarNeighbors++;
						}
						else
						{
							replacementCandidates.push_back(neighbor);
						}
					}
				}
				if (
					!replacementCandidates.empty()
					&& isDenoiseFirefly(luminance, neighborLuminances, similarNeighbors)
				)
				{
					result[center] = medianColorByLuminance(replacementCandidates);
				}
			}
		}
		return (result);
	}

	double	featureDistanceSquared(const Denoise::FeatureVector& a, const Denoise::FeatureVector& b)
	{
		double distance = 0.0;

		for (std::size_t i = 0; i < Denoise::NFOR_FEATURE_COUNT; i++)
		{
			const double difference = a[i] - b[i];

			distance += difference * difference;
		}
		return (distance / static_cast<double>(Denoise::NFOR_FEATURE_COUNT));
	}

	double	averageFeatureVariance(const Denoise::FeatureVector& featureVariance)
	{
		double variance = 0.0;

		for (std::size_t i = 0; i < Denoise::NFOR_FEATURE_COUNT; i++)
		{
			variance += featureVariance[i];
		}
		return (variance / static_cast<double>(Denoise::NFOR_FEATURE_COUNT));
	}

	double	patchDistance(
		const std::vector<Color>& guide,
		const std::vector<double>& variance,
		std::size_t width,
		std::size_t height,
		std::size_t x1,
		std::size_t y1,
		std::size_t x2,
		std::size_t y2,
		unsigned int patchRadius,
		double minVariance
	)
	{
		double distance = 0.0;
		unsigned int samples = 0;
		const long long radius = static_cast<long long>(patchRadius);

		if (x1 == x2 && y1 == y2)
		{
			return (0.0);
		}
		const bool unclampedPatch = x1 >= patchRadius
			&& y1 >= patchRadius
			&& x2 >= patchRadius
			&& y2 >= patchRadius
			&& x1 + patchRadius < width
			&& y1 + patchRadius < height
			&& x2 + patchRadius < width
			&& y2 + patchRadius < height;

		if (unclampedPatch)
		{
			for (long long offsetY = -radius; offsetY <= radius; offsetY++)
			{
				for (long long offsetX = -radius; offsetX <= radius; offsetX++)
				{
					const std::size_t ax = static_cast<std::size_t>(static_cast<long long>(x1) + offsetX);
					const std::size_t ay = static_cast<std::size_t>(static_cast<long long>(y1) + offsetY);
					const std::size_t bx = static_cast<std::size_t>(static_cast<long long>(x2) + offsetX);
					const std::size_t by = static_cast<std::size_t>(static_cast<long long>(y2) + offsetY);
					const std::size_t indexA = ay * width + ax;
					const std::size_t indexB = by * width + bx;
					const double localVariance = variance[indexA] + variance[indexB] + minVariance;

					distance += colorDistanceSquared(guide[indexA], guide[indexB]) / localVariance;
					samples++;
				}
			}
		}
		else
		{
			for (long long offsetY = -radius; offsetY <= radius; offsetY++)
			{
				for (long long offsetX = -radius; offsetX <= radius; offsetX++)
				{
					const std::size_t ax = clampCoordinate(static_cast<long long>(x1) + offsetX, width);
					const std::size_t ay = clampCoordinate(static_cast<long long>(y1) + offsetY, height);
					const std::size_t bx = clampCoordinate(static_cast<long long>(x2) + offsetX, width);
					const std::size_t by = clampCoordinate(static_cast<long long>(y2) + offsetY, height);
					const std::size_t indexA = ay * width + ax;
					const std::size_t indexB = by * width + bx;
					const double localVariance = variance[indexA] + variance[indexB] + minVariance;

					distance += colorDistanceSquared(guide[indexA], guide[indexB]) / localVariance;
					samples++;
				}
			}
		}

		return (distance / static_cast<double>(samples));
	}

	double	colorWeightFromDistance(double distance, double bandwidth, double minVariance)
	{
		const double bandwidthSquared = std::max(minVariance, bandwidth * bandwidth);

		return (std::exp(-distance / bandwidthSquared));
	}


	double	colorWeight(
		const std::vector<Color>& guide,
		const std::vector<double>& variance,
		std::size_t width,
		std::size_t height,
		std::size_t x1,
		std::size_t y1,
		std::size_t x2,
		std::size_t y2,
		unsigned int patchRadius,
		double bandwidth,
		double minVariance
	)
	{
		const double distance = patchDistance(
			guide,
			variance,
			width,
			height,
			x1,
			y1,
			x2,
			y2,
			patchRadius,
			minVariance
		);

		return (colorWeightFromDistance(distance, bandwidth, minVariance));
	}

	std::vector<Color>	averageColorBuffers(const std::vector<Color>& colorA, const std::vector<Color>& colorB)
	{
		std::vector<Color> result(colorA.size());

		for (std::size_t i = 0; i < result.size(); i++)
		{
			result[i] = (colorA[i] + colorB[i]) * 0.5;
		}
		return (result);
	}

	std::vector<double>	doubledVariance(const std::vector<double>& variance)
	{
		std::vector<double> result(variance.size());

		for (std::size_t i = 0; i < variance.size(); i++)
		{
			result[i] = variance[i] * 2.0;
		}
		return (result);
	}

	std::vector<double>	averageFeatureVarianceMap(const std::vector<Denoise::FeatureVector>& featureVariance)
	{
		std::vector<double> result(featureVariance.size());

		for (std::size_t i = 0; i < featureVariance.size(); i++)
		{
			result[i] = averageFeatureVariance(featureVariance[i]);
		}
		return (result);
	}

	Denoise::FeatureVector	featureBlend(
		const Denoise::FeatureVector& a,
		const Denoise::FeatureVector& b,
		double weightA
	)
	{
		Denoise::FeatureVector result;
		const double weightB = 1.0 - weightA;

		for (std::size_t i = 0; i < Denoise::NFOR_FEATURE_COUNT; i++)
		{
			result[i] = a[i] * weightA + b[i] * weightB;
		}
		return (result);
	}

	std::vector<Denoise::FeatureVector>	prefilterFeatures(
		const std::vector<Denoise::FeatureVector>& image,
		const std::vector<Denoise::FeatureVector>& guide,
		const std::vector<double>& featureVarianceAverage,
		std::size_t width,
		std::size_t height,
		unsigned int radius,
		double minVariance,
		std::size_t threadCount
	)
	{
		std::vector<Denoise::FeatureVector> result(image.size());
		const long long filterRadius = static_cast<long long>(radius);

		parallelRows(height, threadCount, [&](std::size_t startY, std::size_t stopY)
		{
			for (std::size_t y = startY; y < stopY; y++)
			{
				for (std::size_t x = 0; x < width; x++)
				{
					Denoise::FeatureVector sum;
					double totalWeight = 0.0;
					const std::size_t center = y * width + x;
					const double centerVariance = featureVarianceAverage[center];

					for (long long offsetY = -filterRadius; offsetY <= filterRadius; offsetY++)
					{
						for (long long offsetX = -filterRadius; offsetX <= filterRadius; offsetX++)
						{
							const std::size_t sampleX = clampCoordinate(static_cast<long long>(x) + offsetX, width);
							const std::size_t sampleY = clampCoordinate(static_cast<long long>(y) + offsetY, height);
							const std::size_t sample = sampleY * width + sampleX;
							const double localVariance = centerVariance
								+ featureVarianceAverage[sample]
								+ minVariance;
							const double distance = featureDistanceSquared(guide[center], guide[sample]) / localVariance;
							const double spatialDistance = static_cast<double>((offsetX * offsetX) + (offsetY * offsetY));
							const double spatialSigma = std::max(1.0, static_cast<double>(radius));
							const double weight = std::exp(-distance / 0.25) * std::exp(-spatialDistance / (2.0 * spatialSigma * spatialSigma));

							for (std::size_t feature = 0; feature < Denoise::NFOR_FEATURE_COUNT; feature++)
							{
								sum[feature] += image[sample][feature] * weight;
							}
							totalWeight += weight;
						}
					}

					if (totalWeight <= std::numeric_limits<double>::epsilon())
					{
						result[center] = image[center];
					}
					else
					{
						for (std::size_t feature = 0; feature < Denoise::NFOR_FEATURE_COUNT; feature++)
						{
							result[center][feature] = sum[feature] / totalWeight;
						}
					}
				}
			}
		});
		return (result);
	}

	bool	solveLinearSystemRGB(RegressionMatrix& matrix, RegressionRGB& rhs)
	{
		for (std::size_t pivot = 0; pivot < REGRESSION_SIZE; pivot++)
		{
			std::size_t bestRow = pivot;
			double bestValue = std::fabs(matrix[pivot][pivot]);

			for (std::size_t row = pivot + 1; row < REGRESSION_SIZE; row++)
			{
				const double value = std::fabs(matrix[row][pivot]);
				if (value > bestValue)
				{
					bestValue = value;
					bestRow = row;
				}
			}
			if (bestValue < 1e-10)
			{
				return (false);
			}
			if (bestRow != pivot)
			{
				std::swap(matrix[pivot], matrix[bestRow]);
				std::swap(rhs[pivot], rhs[bestRow]);
			}

			const double pivotValue = matrix[pivot][pivot];
			for (std::size_t column = pivot; column < REGRESSION_SIZE; column++)
			{
				matrix[pivot][column] /= pivotValue;
			}
			for (std::size_t component = 0; component < 3; component++)
			{
				rhs[pivot][component] /= pivotValue;
			}

			for (std::size_t row = 0; row < REGRESSION_SIZE; row++)
			{
				if (row == pivot)
				{
					continue;
				}
				const double factor = matrix[row][pivot];
				for (std::size_t column = pivot; column < REGRESSION_SIZE; column++)
				{
					matrix[row][column] -= factor * matrix[pivot][column];
				}
				for (std::size_t component = 0; component < 3; component++)
				{
					rhs[row][component] -= factor * rhs[pivot][component];
				}
			}
		}
		return (true);
	}

	Color	weightedAverageFallback(
		const std::vector<Color>& image,
		const std::vector<double>& guideVariance,
		std::size_t width,
		std::size_t height,
		std::size_t x,
		std::size_t y,
		unsigned int radius,
		double minVariance
	)
	{
		Color result(0.0, 0.0, 0.0);
		double totalWeight = 0.0;
		const long long filterRadius = static_cast<long long>(radius);

		for (long long offsetY = -filterRadius; offsetY <= filterRadius; offsetY++)
		{
			for (long long offsetX = -filterRadius; offsetX <= filterRadius; offsetX++)
			{
				const std::size_t sampleX = clampCoordinate(static_cast<long long>(x) + offsetX, width);
				const std::size_t sampleY = clampCoordinate(static_cast<long long>(y) + offsetY, height);
				const std::size_t sample = sampleY * width + sampleX;
				const double spatialDistance = static_cast<double>((offsetX * offsetX) + (offsetY * offsetY));
				const double varianceWeight = 1.0 / (guideVariance[sample] + minVariance);
				const double weight = varianceWeight * std::exp(-spatialDistance / 8.0);

				result += image[sample] * weight;
				totalWeight += weight;
			}
		}
		if (totalWeight <= std::numeric_limits<double>::epsilon())
		{
			return (image[y * width + x]);
		}
		return (result / totalWeight);
	}

	void	initializeRegressionMatrix(RegressionMatrix& matrix)
	{
		const double ridge = 1e-4;

		for (std::size_t diagonal = 0; diagonal < REGRESSION_SIZE; diagonal++)
		{
			matrix[diagonal][diagonal] = ridge;
		}
	}

	RegressionBasis	regressionBasis(
		const std::vector<Denoise::FeatureVector>& features,
		std::size_t center,
		std::size_t sample
	)
	{
		RegressionBasis basis = {};

		basis[0] = 1.0;
		for (std::size_t feature = 0; feature < Denoise::NFOR_FEATURE_COUNT; feature++)
		{
			basis[feature + 1] = features[sample][feature] - features[center][feature];
		}
		return (basis);
	}

	void	addRegressionSample(
		RegressionMatrix& matrix,
		RegressionRGB& rhs,
		const RegressionBasis& basis,
		double weight,
		const Color& color
	)
	{
		for (std::size_t row = 0; row < REGRESSION_SIZE; row++)
		{
			const double weightedBasis = weight * basis[row];

			for (std::size_t column = row; column < REGRESSION_SIZE; column++)
			{
				matrix[row][column] += weightedBasis * basis[column];
			}
			rhs[row][0] += weightedBasis * color.getRed();
			rhs[row][1] += weightedBasis * color.getGreen();
			rhs[row][2] += weightedBasis * color.getBlue();
		}
	}

	void	addRegressionSamplePair(
		RegressionMatrix& firstMatrix,
		RegressionRGB& firstRhs,
		RegressionMatrix& secondMatrix,
		RegressionRGB& secondRhs,
		const RegressionBasis& basis,
		double firstWeight,
		double secondWeight,
		const Color& color
	)
	{
		for (std::size_t row = 0; row < REGRESSION_SIZE; row++)
		{
			const double firstWeightedBasis = firstWeight * basis[row];
			const double secondWeightedBasis = secondWeight * basis[row];

			for (std::size_t column = row; column < REGRESSION_SIZE; column++)
			{
				firstMatrix[row][column] += firstWeightedBasis * basis[column];
				secondMatrix[row][column] += secondWeightedBasis * basis[column];
			}
			firstRhs[row][0] += firstWeightedBasis * color.getRed();
			firstRhs[row][1] += firstWeightedBasis * color.getGreen();
			firstRhs[row][2] += firstWeightedBasis * color.getBlue();
			secondRhs[row][0] += secondWeightedBasis * color.getRed();
			secondRhs[row][1] += secondWeightedBasis * color.getGreen();
			secondRhs[row][2] += secondWeightedBasis * color.getBlue();
		}
	}

	void	completeRegressionMatrix(RegressionMatrix& matrix)
	{
		for (std::size_t row = 1; row < REGRESSION_SIZE; row++)
		{
			for (std::size_t column = 0; column < row; column++)
			{
				matrix[row][column] = matrix[column][row];
			}
		}
	}

	Color	solveRegressionPixel(
		RegressionMatrix& matrix,
		RegressionRGB& rhs,
		const std::vector<Color>& image,
		const std::vector<double>& imageVariance,
		std::size_t width,
		std::size_t height,
		std::size_t x,
		std::size_t y,
		const Denoise::NFORSettings& settings
	)
	{
		completeRegressionMatrix(matrix);
		if (!solveLinearSystemRGB(matrix, rhs))
		{
			return (weightedAverageFallback(
				image,
				imageVariance,
				width,
				height,
				x,
				y,
				settings.regressionRadius,
				settings.minVariance
			));
		}
		return (Color(rhs[0][0], rhs[0][1], rhs[0][2]));
	}

	std::vector<Color>	regressionPass(
		const std::vector<Color>& image,
		const std::vector<Denoise::FeatureVector>& features,
		const std::vector<double>& imageVariance,
		std::size_t width,
		std::size_t height,
		const Denoise::NFORSettings& settings,
		double bandwidth
	)
	{
		std::vector<Color> result(image.size());
		const long long radius = static_cast<long long>(settings.regressionRadius);

		parallelRows(height, settings.threadCount, [&](std::size_t startY, std::size_t stopY)
		{
			for (std::size_t y = startY; y < stopY; y++)
			{
				for (std::size_t x = 0; x < width; x++)
				{
					const std::size_t center = y * width + x;
					RegressionMatrix matrix = {};
					RegressionRGB rhs = {};

					initializeRegressionMatrix(matrix);
					for (long long offsetY = -radius; offsetY <= radius; offsetY++)
					{
						for (long long offsetX = -radius; offsetX <= radius; offsetX++)
						{
							const std::size_t sampleX = clampCoordinate(static_cast<long long>(x) + offsetX, width);
							const std::size_t sampleY = clampCoordinate(static_cast<long long>(y) + offsetY, height);
							const std::size_t sample = sampleY * width + sampleX;
							const double distance = patchDistance(
								image,
								imageVariance,
								width,
								height,
								x,
								y,
								sampleX,
								sampleY,
								settings.patchRadius,
								settings.minVariance
							);
							const double weight = colorWeightFromDistance(distance, bandwidth, settings.minVariance);
							const RegressionBasis basis = regressionBasis(features, center, sample);

							addRegressionSample(matrix, rhs, basis, weight, image[sample]);
						}
					}
					result[center] = solveRegressionPixel(
						matrix,
						rhs,
						image,
						imageVariance,
						width,
						height,
						x,
						y,
						settings
					);
				}
			}
		});
		return (result);
	}

	RegressionPair	regressionPairPass(
		const std::vector<Color>& image,
		const std::vector<Denoise::FeatureVector>& features,
		const std::vector<double>& imageVariance,
		std::size_t width,
		std::size_t height,
		const Denoise::NFORSettings& settings,
		double firstBandwidth,
		double secondBandwidth
	)
	{
		RegressionPair result;
		const long long radius = static_cast<long long>(settings.regressionRadius);

		result.first.resize(image.size());
		result.second.resize(image.size());
		parallelRows(height, settings.threadCount, [&](std::size_t startY, std::size_t stopY)
		{
			for (std::size_t y = startY; y < stopY; y++)
			{
				for (std::size_t x = 0; x < width; x++)
				{
					const std::size_t center = y * width + x;
					RegressionMatrix firstMatrix = {};
					RegressionRGB firstRhs = {};
					RegressionMatrix secondMatrix = {};
					RegressionRGB secondRhs = {};

					initializeRegressionMatrix(firstMatrix);
					initializeRegressionMatrix(secondMatrix);
					for (long long offsetY = -radius; offsetY <= radius; offsetY++)
					{
						for (long long offsetX = -radius; offsetX <= radius; offsetX++)
						{
							const std::size_t sampleX = clampCoordinate(static_cast<long long>(x) + offsetX, width);
							const std::size_t sampleY = clampCoordinate(static_cast<long long>(y) + offsetY, height);
							const std::size_t sample = sampleY * width + sampleX;
							const double distance = patchDistance(
								image,
								imageVariance,
								width,
								height,
								x,
								y,
								sampleX,
								sampleY,
								settings.patchRadius,
								settings.minVariance
							);
							const double firstWeight = colorWeightFromDistance(distance, firstBandwidth, settings.minVariance);
							const double secondWeight = colorWeightFromDistance(distance, secondBandwidth, settings.minVariance);
							const RegressionBasis basis = regressionBasis(features, center, sample);

							addRegressionSamplePair(
								firstMatrix,
								firstRhs,
								secondMatrix,
								secondRhs,
								basis,
								firstWeight,
								secondWeight,
								image[sample]
							);
						}
					}
					result.first[center] = solveRegressionPixel(
						firstMatrix,
						firstRhs,
						image,
						imageVariance,
						width,
						height,
						x,
						y,
						settings
					);
					result.second[center] = solveRegressionPixel(
						secondMatrix,
						secondRhs,
						image,
						imageVariance,
						width,
						height,
						x,
						y,
						settings
					);
				}
			}
		});
		return (result);
	}

	ScalarMapPair	filterScalarMapPair(
		const std::vector<double>& firstImage,
		const std::vector<double>& secondImage,
		const std::vector<Color>& guide,
		const std::vector<double>& guideVariance,
		std::size_t width,
		std::size_t height,
		unsigned int radius,
		double minVariance,
		std::size_t threadCount
	)
	{
		ScalarMapPair result;
		const long long filterRadius = static_cast<long long>(radius);

		result.first.resize(firstImage.size());
		result.second.resize(secondImage.size());
		parallelRows(height, threadCount, [&](std::size_t startY, std::size_t stopY)
		{
			for (std::size_t y = startY; y < stopY; y++)
			{
				for (std::size_t x = 0; x < width; x++)
				{
					double firstValue = 0.0;
					double secondValue = 0.0;
					double totalWeight = 0.0;
					const std::size_t center = y * width + x;

					for (long long offsetY = -filterRadius; offsetY <= filterRadius; offsetY++)
					{
						for (long long offsetX = -filterRadius; offsetX <= filterRadius; offsetX++)
						{
							const std::size_t sampleX = clampCoordinate(static_cast<long long>(x) + offsetX, width);
							const std::size_t sampleY = clampCoordinate(static_cast<long long>(y) + offsetY, height);
							const std::size_t sample = sampleY * width + sampleX;
							const double weight = colorWeight(
								guide,
								guideVariance,
								width,
								height,
								x,
								y,
								sampleX,
								sampleY,
								1,
								1.0,
								minVariance
							);

							firstValue += firstImage[sample] * weight;
							secondValue += secondImage[sample] * weight;
							totalWeight += weight;
						}
					}
					if (totalWeight > std::numeric_limits<double>::epsilon())
					{
						result.first[center] = firstValue / totalWeight;
						result.second[center] = secondValue / totalWeight;
					}
					else
					{
						result.first[center] = firstImage[center];
						result.second[center] = secondImage[center];
					}
				}
			}
		});
		return (result);
	}

	std::vector<double>	estimateMSE(
		const std::vector<Color>& colorA,
		const std::vector<Color>& colorB,
		const std::vector<Color>& filteredA,
		const std::vector<Color>& filteredB,
		const std::vector<double>& colorVariance
	)
	{
		std::vector<double> result(colorA.size());

		for (std::size_t i = 0; i < result.size(); i++)
		{
			const double mseA = colorDistanceSquared(colorB[i], filteredA[i]) - 2.0 * colorVariance[i];
			const double mseB = colorDistanceSquared(colorA[i], filteredB[i]) - 2.0 * colorVariance[i];
			const double residualVariance = colorDistanceSquared(filteredA[i], filteredB[i]) * 0.25;

			result[i] = std::max(0.0, ((mseA + mseB) * 0.5) - residualVariance);
		}
		return (result);
	}

	std::vector<double>	combinedResultVariance(
		const std::vector<Color>& a,
		const std::vector<Color>& b
	)
	{
		std::vector<double> variance(a.size());

		for (std::size_t i = 0; i < variance.size(); i++)
		{
			variance[i] = colorDistanceSquared(a[i], b[i]) * 0.25;
		}
		return (variance);
	}

	std::vector<double>	bandwidthSelectionMap(
		const std::vector<double>& ownMSE,
		const std::vector<double>& otherMSE
	)
	{
		std::vector<double> selection(ownMSE.size());

		for (std::size_t i = 0; i < selection.size(); i++)
		{
			selection[i] = ownMSE[i] <= otherMSE[i] ? 1.0 : 0.0;
		}
		return (selection);
	}

	bool	isVolumeFeature(const Denoise::FeatureVector& feature)
	{
		if (feature[2] < 0.5)
			return (false);
		const double isotropic = static_cast<double>(ISOTROPIC)
			/ static_cast<double>(PRINCIPLED);
		const double henyeyGreenstein = static_cast<double>(HENYEY_GREENSTEIN)
			/ static_cast<double>(PRINCIPLED);
		return (
			std::fabs(feature[10] - isotropic) < 0.04
			|| std::fabs(feature[10] - henyeyGreenstein) < 0.04
		);
	}

	std::vector<Color>	reconstructVolumeResidual(
		const std::vector<Color>& stochasticColor,
		const std::vector<Color>& surfaceResult,
		const std::vector<Denoise::FeatureVector>& features,
		const std::vector<Color>& deterministicColor,
		const std::vector<double>& volumeOpacity,
		const std::vector<double>& colorVariance,
		const std::vector<unsigned int>& sampleCount,
		std::size_t width,
		std::size_t height,
		std::size_t threadCount
	)
	{
		if (
			deterministicColor.size() != stochasticColor.size()
			|| volumeOpacity.size() != stochasticColor.size()
			|| colorVariance.size() != stochasticColor.size()
			|| sampleCount.size() != stochasticColor.size()
		)
			return (surfaceResult);
		std::vector<unsigned char> volumeMask(stochasticColor.size(), 0);
		bool hasVolumes = false;
		for (std::size_t i = 0; i < volumeMask.size(); i++)
		{
			volumeMask[i] = (
				isVolumeFeature(features[i])
				|| volumeOpacity[i] > 1e-5
			) ? 1 : 0;
			hasVolumes = hasVolumes || volumeMask[i] != 0;
		}
		if (!hasVolumes)
			return (surfaceResult);

		// Multiple scattering is a low-frequency diffusion signal. Polynomial
		// surface regression can hallucinate coherent curves from its enormous
		// low-spp variance, so reconstruct it with an edge-aware a-trous filter.
		// The separately integrated direct-light layer is the radiance guide and
		// is added only after this pass, preserving sharp silver linings and lobes.
		constexpr std::array<double, 5> kernel = {1.0, 4.0, 6.0, 4.0, 1.0};
		std::vector<Color> current = stochasticColor;
		std::vector<Color> next = current;
		for (int iteration = 0; iteration < 3; iteration++)
		{
			const long long stride = 1LL << iteration;
			parallelRows(height, threadCount, [&](std::size_t startY, std::size_t stopY)
			{
				for (std::size_t y = startY; y < stopY; y++)
				{
					for (std::size_t x = 0; x < width; x++)
					{
						const std::size_t center = y * width + x;
						if (!volumeMask[center])
						{
							next[center] = surfaceResult[center];
							continue;
						}
						Color sum(0.0, 0.0, 0.0);
						double totalWeight = 0.0;
						const double centerDepth = features[center][3];
						const double centerDensity = features[center][7];
						const double centerOpacity = volumeOpacity[center];
						const double centerGuide = colorLuminance(deterministicColor[center]);
						for (long long ky = -2; ky <= 2; ky++)
						{
							for (long long kx = -2; kx <= 2; kx++)
							{
								const std::size_t sx = clampCoordinate(
									static_cast<long long>(x) + kx * stride,
									width
								);
								const std::size_t sy = clampCoordinate(
									static_cast<long long>(y) + ky * stride,
									height
								);
								const std::size_t sample = sy * width + sx;
								if (!volumeMask[sample])
									continue;
								const double depthDifference = std::fabs(
									centerDepth - features[sample][3]
								);
								const double sampleDensity = features[sample][7];
								const double densityDifference = std::fabs(
									centerDensity - sampleDensity
								) / (0.08 + centerDensity + sampleDensity);
								const double opacityDifference = std::fabs(
									centerOpacity - volumeOpacity[sample]
								) / (0.04 + centerOpacity + volumeOpacity[sample]);
								const double sampleGuide = colorLuminance(deterministicColor[sample]);
								const double guideDifference = std::fabs(centerGuide - sampleGuide)
									/ (0.05 + centerGuide + sampleGuide);
								const double edgeWeight = std::exp(
									-(depthDifference * depthDifference) / (2.0 * 0.018 * 0.018)
									-(densityDifference * densityDifference) / (2.0 * 0.24 * 0.24)
									-(opacityDifference * opacityDifference) / (2.0 * 0.20 * 0.20)
									-(guideDifference * guideDifference) / (2.0 * 0.32 * 0.32)
								);
								const double weight = kernel[static_cast<std::size_t>(kx + 2)]
									* kernel[static_cast<std::size_t>(ky + 2)] * edgeWeight;
								sum += current[sample] * weight;
								totalWeight += weight;
							}
						}
						const Color filtered = totalWeight > std::numeric_limits<double>::epsilon()
							? sum / totalWeight
							: current[center];
						const double variance = std::isfinite(colorVariance[center])
							? std::max(0.0, colorVariance[center])
							: 0.0;
						const double relativeNoise = std::sqrt(variance)
							/ (0.02 + colorLuminance(stochasticColor[center]));
						const double minimumNoise = iteration == 0 ? 0.01
							: (iteration == 1 ? 0.035 : 0.07);
						const double maximumNoise = iteration == 0 ? 0.08
							: (iteration == 1 ? 0.16 : 0.28);
						double filterStrength = 1.0;
						if (sampleCount[center] >= 64u)
						{
							filterStrength = std::clamp(
								(relativeNoise - minimumNoise)
									/ (maximumNoise - minimumNoise),
								0.0,
								1.0
							);
							filterStrength = filterStrength * filterStrength
								* (3.0 - 2.0 * filterStrength);
						}
						next[center] = current[center] * (1.0 - filterStrength)
							+ filtered * filterStrength;
					}
				}
			});
			current.swap(next);
		}
		for (std::size_t i = 0; i < current.size(); i++)
		{
			if (!volumeMask[i])
				current[i] = surfaceResult[i];
		}
		return (current);
	}
}

void	Denoise::NFORBuffers::initialize(std::size_t imageWidth, std::size_t imageHeight)
{
	this->width = imageWidth;
	this->height = imageHeight;
	const std::size_t pixelCount = imageWidth * imageHeight;

	this->colorA.assign(pixelCount, Color());
	this->colorB.assign(pixelCount, Color());
	this->deterministicColor.assign(pixelCount, Color());
	this->volumeOpacity.assign(pixelCount, 0.0);
	this->colorVariance.assign(pixelCount, 0.0);
	this->sampleCount.assign(pixelCount, 0u);
	this->featuresA.assign(pixelCount, FeatureVector());
	this->featuresB.assign(pixelCount, FeatureVector());
	this->featureVariance.assign(pixelCount, FeatureVector());
}

bool	Denoise::NFORBuffers::empty(void) const
{
	return (this->width == 0 || this->height == 0 || this->colorA.empty());
}

std::size_t	Denoise::NFORBuffers::index(std::size_t x, std::size_t y) const
{
	return (y * this->width + x);
}

std::unique_ptr<Image>	Denoise::applyNFOR(const NFORBuffers& buffers, const NFORSettings& settings)
{
	reportProgress(settings, 0);
	if (buffers.empty())
	{
		reportProgress(settings, 100);
		return (std::make_unique<Image>(0, 0));
	}
	if (settings.regressionRadius == 0 || settings.patchRadius == 0)
	{
		throw std::invalid_argument("NFOR radii must be positive.");
	}

	const std::vector<Color> colorA = suppressDenoiseFireflies(buffers.colorA, buffers.width, buffers.height);
	const std::vector<Color> colorB = suppressDenoiseFireflies(buffers.colorB, buffers.width, buffers.height);
	const std::vector<Color> color = suppressDenoiseFireflies(
		averageColorBuffers(colorA, colorB),
		buffers.width,
		buffers.height
	);
	const std::vector<double> regressionVariance = doubledVariance(buffers.colorVariance);
	const std::vector<double> featureVarianceAverage = averageFeatureVarianceMap(buffers.featureVariance);
	reportProgress(settings, 5);
	const std::vector<FeatureVector> filteredFeaturesA = prefilterFeatures(
		buffers.featuresA,
		buffers.featuresB,
		featureVarianceAverage,
		buffers.width,
		buffers.height,
		settings.featurePrefilterRadius,
		settings.minVariance,
		settings.threadCount
	);
	reportProgress(settings, 15);
	const std::vector<FeatureVector> filteredFeaturesB = prefilterFeatures(
		buffers.featuresB,
		buffers.featuresA,
		featureVarianceAverage,
		buffers.width,
		buffers.height,
		settings.featurePrefilterRadius,
		settings.minVariance,
		settings.threadCount
	);
	reportProgress(settings, 25);

	const RegressionPair filteredA = regressionPairPass(
		colorA,
		filteredFeaturesB,
		regressionVariance,
		buffers.width,
		buffers.height,
		settings,
		0.5,
		1.0
	);
	reportProgress(settings, 45);
	const RegressionPair filteredB = regressionPairPass(
		colorB,
		filteredFeaturesA,
		regressionVariance,
		buffers.width,
		buffers.height,
		settings,
		0.5,
		1.0
	);
	reportProgress(settings, 65);
	const ScalarMapPair mse = filterScalarMapPair(
		estimateMSE(colorA, colorB, filteredA.first, filteredB.first, regressionVariance),
		estimateMSE(colorA, colorB, filteredA.second, filteredB.second, regressionVariance),
		color,
		buffers.colorVariance,
		buffers.width,
		buffers.height,
		3,
		settings.minVariance,
		settings.threadCount
	);
	reportProgress(settings, 75);
	const ScalarMapPair selection = filterScalarMapPair(
		bandwidthSelectionMap(mse.first, mse.second),
		bandwidthSelectionMap(mse.second, mse.first),
		color,
		buffers.colorVariance,
		buffers.width,
		buffers.height,
		3,
		settings.minVariance,
		settings.threadCount
	);
	reportProgress(settings, 85);

	std::vector<Color> combined(buffers.width * buffers.height);
	std::vector<FeatureVector> finalFeatures(buffers.width * buffers.height);
	for (std::size_t i = 0; i < combined.size(); i++)
	{
		const double selectionTotal = selection.first[i] + selection.second[i];
		const double weight0 = selectionTotal > settings.minVariance
			? selection.first[i] / selectionTotal
			: (mse.first[i] <= mse.second[i] ? 1.0 : 0.0);
		const Color resultA = filteredA.first[i] * weight0 + filteredA.second[i] * (1.0 - weight0);
		const Color resultB = filteredB.first[i] * weight0 + filteredB.second[i] * (1.0 - weight0);

		combined[i] = (resultA + resultB) * 0.5;
		finalFeatures[i] = featureBlend(filteredFeaturesA[i], filteredFeaturesB[i], 0.5);
	}
	reportProgress(settings, 90);

	const std::vector<double> finalVariance = combinedResultVariance(filteredA.second, filteredB.second);
	const std::vector<Color> surfaceColor = regressionPass(
		combined,
		finalFeatures,
		finalVariance,
		buffers.width,
		buffers.height,
		settings,
		1.0
	);
	const std::vector<Color> finalColor = reconstructVolumeResidual(
		color,
		surfaceColor,
		finalFeatures,
		buffers.deterministicColor,
		buffers.volumeOpacity,
		buffers.colorVariance,
		buffers.sampleCount,
		buffers.width,
		buffers.height,
		settings.threadCount
	);
	auto image = std::make_unique<Image>(buffers.width, buffers.height);

	reportProgress(settings, 98);
	image->initialize();
	Color* imagePixels = image->pixels();
	for (std::size_t y = 0; y < buffers.height; y++)
	{
		for (std::size_t x = 0; x < buffers.width; x++)
		{
			imagePixels[y * buffers.width + x] = finalColor[y * buffers.width + x];
		}
	}
	reportProgress(settings, 100);
	return (image);
}
