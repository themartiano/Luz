#pragma once

#include "Color.hpp"
#include "Image.hpp"
#include <array>
#include <cstddef>
#include <memory>
#include <vector>

namespace Denoise
{
	constexpr std::size_t NFOR_FEATURE_COUNT = 11;
	using ProgressCallback = void (*)(unsigned int percentage, void* userData);

	struct FeatureVector
	{
		std::array<double, NFOR_FEATURE_COUNT> values = {};

		double&	operator[](std::size_t index) { return (values[index]); }
		double	operator[](std::size_t index) const { return (values[index]); }
	};

	struct NFORBuffers
	{
		std::size_t	width = 0;
		std::size_t	height = 0;
		std::vector<Color>	colorA;
		std::vector<Color>	colorB;
		// Low-variance analytic terms are composited after regression so their
		// edges and high-frequency structure are never blurred by the denoiser.
		std::vector<Color>	deterministicColor;
		// Deterministic camera-ray opacity from the same primary cloud march. It
		// keeps sub-threshold wisps in the volume reconstruction and separates
		// thin vapor from opaque billows without another integration pass.
		std::vector<double>	volumeOpacity;
		std::vector<double>	colorVariance;
		std::vector<unsigned int>	sampleCount;
		std::vector<FeatureVector>	featuresA;
		std::vector<FeatureVector>	featuresB;
		std::vector<FeatureVector>	featureVariance;

		void	initialize(std::size_t imageWidth, std::size_t imageHeight);
		bool	empty(void) const;
		std::size_t	index(std::size_t x, std::size_t y) const;
	};

	struct NFORSettings
	{
		unsigned int	featurePrefilterRadius = 1;
		unsigned int	regressionRadius = 2;
		unsigned int	patchRadius = 1;
		double			minVariance = 1e-5;
		std::size_t		threadCount = 1;
		ProgressCallback	progressCallback = nullptr;
		void*				progressUserData = nullptr;
	};

	std::unique_ptr<Image>	applyNFOR(const NFORBuffers& buffers, const NFORSettings& settings = NFORSettings());
}
