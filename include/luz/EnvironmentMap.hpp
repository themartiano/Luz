#pragma once

#include "Color.hpp"
#include "Sampler.hpp"
#include "Vector3.hpp"
#include <cstddef>
#include <string>
#include <vector>

class	EnvironmentMap
{
	public:
		struct	Sample
		{
			Vector3	direction;
			Color	radiance;
			double	pdf = 0.0;
			bool	valid = false;
		};

		EnvironmentMap(void);
		EnvironmentMap(std::size_t width, std::size_t height, std::vector<Color> pixels);
		static EnvironmentMap	load(const std::string& fileName);
		static EnvironmentMap	loadPPM(const std::string& fileName);
		static EnvironmentMap	loadHDR(const std::string& fileName);
		Color	sampleDirection(const Vector3& direction, double rotationDegrees = 0.0) const;
		Sample	sample(double selection, Sampler::Sample2D jitter, double rotationDegrees = 0.0) const;
		double	pdf(const Vector3& direction, double rotationDegrees = 0.0) const;
		Color	averageRadiance(void) const;
		double	averageLuminance(void) const;
		double	horizontalIrradiance(void) const;
		std::size_t	getWidth(void) const;
		std::size_t	getHeight(void) const;
		bool	empty(void) const;

	private:
		struct	UV
		{
			double	u = 0.0;
			double	v = 0.0;
		};

		Color	sampleUV(double u, double v) const;
		UV	directionToUV(const Vector3& direction, double rotationDegrees) const;
		Vector3	uvToDirection(double u, double v, double rotationDegrees) const;
		void	buildDistribution(void);

		std::size_t	_width;
		std::size_t	_height;
		std::vector<Color>	_pixels;
		std::vector<double>	_weights;
		std::vector<double>	_solidAngles;
		std::vector<double>	_cdf;
		Color	_averageRadiance;
		double	_totalWeight;
		double	_horizontalIrradiance;
};
