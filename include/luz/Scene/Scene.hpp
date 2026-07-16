#pragma once

#include "Camera.hpp"
#include "Hittables/Hittable.hpp"
#include "Atmosphere.hpp"
#include "EnvironmentMap.hpp"
#include "SkyTypes.hpp"
#include "Image.hpp"
#include "Denoise/NFOR.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory>

class	CausticPhotonMap;
class	VolumeGuidingField;

struct	SceneRenderStats
{
	std::size_t	renderedSamples = 0;
	double		averageSamplesPerPixel = 0.0;
	double		modelLoadMS = 0.0;
	double		sceneBuildMS = 0.0;
	double		renderMS = 0.0;
	double		denoiseMS = 0.0;
	double		postProcessMS = 0.0;
	double		volumeGuideMS = 0.0;
	double		totalMS = 0.0;
	bool		displayDiagnosticsValid = false;
	double		displayLuminanceP01 = 0.0;
	double		displayLuminanceP50 = 0.0;
	double		displayLuminanceP99 = 0.0;
	double		displayNearBlackPixelFraction = 0.0;
	double		displayNearWhitePixelFraction = 0.0;
	double		displayClippedPixelFraction = 0.0;
	bool		modelLoadProgressShown = false;
};

class	Scene
{
	public:
		Scene(void);
		~Scene(void);
		void	addCamera(Camera camera);
		void	addHittable(std::shared_ptr<Hittable> hittable);
		int		getSampleCount(void) const;
		void	setSampleCount(const int sampleCount);
		bool	getAdaptiveSampling(void) const;
		void	setAdaptiveSampling(bool adaptiveSampling);
		int		getAdaptiveMinSamples(void) const;
		void	setAdaptiveMinSamples(int adaptiveMinSamples);
		int		getAdaptiveBackgroundMinSamples(void) const;
		void	setAdaptiveBackgroundMinSamples(int adaptiveBackgroundMinSamples);
		int		getAdaptiveVolumeMinSamples(void) const;
		void	setAdaptiveVolumeMinSamples(int adaptiveVolumeMinSamples);
		int		getAdaptiveCheckInterval(void) const;
		void	setAdaptiveCheckInterval(int adaptiveCheckInterval);
		double	getAdaptiveThreshold(void) const;
		void	setAdaptiveThreshold(double adaptiveThreshold);
		int		getMaxLightBounces(void) const;
		void	setMaxLightBounces(const int maxLightBounces);
		ViewTransform	getViewTransform(void) const;
		void	setViewTransform(ViewTransform viewTransform);
		double	getExposure(void) const;
		void	setExposure(double exposure);
		void	setPhotographicExposure(double fNumber, double shutterSeconds, double iso);
		int		getVolumeGuidingTrainingSamples(void) const;
		void	setVolumeGuidingTrainingSamples(int trainingSamples);
		std::uint32_t	getVolumeGuidingResolution(void) const;
		void	setVolumeGuidingResolution(std::uint32_t resolution);
		std::uint32_t	getVolumeGuidingLobes(void) const;
		void	setVolumeGuidingLobes(std::uint32_t lobes);
		double	getVolumeGuidingAnisotropy(void) const;
		void	setVolumeGuidingAnisotropy(double anisotropy);
		double	getVolumeGuidingStrength(void) const;
		void	setVolumeGuidingStrength(double strength);
		int		getVolumeGuidingStartBounce(void) const;
		void	setVolumeGuidingStartBounce(int startBounce);
		void	setVolumeGuidingField(std::shared_ptr<VolumeGuidingField> field);
		const std::shared_ptr<VolumeGuidingField>&	getVolumeGuidingField(void) const;
		double	getContrast(void) const;
		void	setContrast(double contrast);
		bool	getCausticsEnabled(void) const;
		void	setCausticsEnabled(bool causticsEnabled);
		int		getCausticPhotonCount(void) const;
		void	setCausticPhotonCount(int causticPhotonCount);
		int		getCausticPassCount(void) const;
		void	setCausticPassCount(int causticPassCount);
		double	getCausticRadiusMeters(void) const;
		void	setCausticRadiusMeters(double causticRadiusMeters);
		double	getCausticAlpha(void) const;
		void	setCausticAlpha(double causticAlpha);
		void	setCausticPhotonMap(std::shared_ptr<CausticPhotonMap> causticPhotonMap);
		const std::shared_ptr<CausticPhotonMap>&	getCausticPhotonMap(void) const;
		double	getSkyline(void) const;
		SkyTypes	getRenderSky(void) const;
		void	setRenderSky(SkyTypes renderSky);
		bool	getDistanceBlueness(void) const;
		void	setDistanceBlueness(bool distanceBlueness);
		void	setAtmosphere(Atmosphere atmosphere);
		const Atmosphere&	getAtmosphere(void) const;
		double	getMetersPerUnit(void) const;
		void	setMetersPerUnit(double metersPerUnit);
		double	sceneUnitsToMeters(double sceneUnits) const;
		double	sceneAreaToSquareMeters(double sceneArea) const;
		void	syncAtmosphereSunDirection(void);
		Color	getBackgroundColor(void) const;
		void	setBackgroundColor(Color backgroundColor);
		void	setEnvironmentMap(std::shared_ptr<EnvironmentMap> environmentMap);
		const std::shared_ptr<EnvironmentMap>&	getEnvironmentMap(void) const;
		bool	hasEnvironmentMap(void) const;
		void	setEnvironmentStrength(double environmentStrength);
		double	getEnvironmentStrength(void) const;
		void	setEnvironmentLighting(bool environmentLighting);
		bool	getEnvironmentLighting(void) const;
		void	calibrateEnvironmentAverageRadiance(double averageRadiance);
		void	calibrateEnvironmentAverageLuminance(double averageLuminance);
		void	calibrateEnvironmentHorizontalIrradiance(double horizontalIrradiance);
		void	calibrateEnvironmentHorizontalIlluminance(double horizontalIlluminance);
		void	setEnvironmentRotation(double environmentRotation);
		double	getEnvironmentRotation(void) const;
		Camera	getActiveCamera(void) const;
		bool	hasCamera(void) const;
			const std::vector<std::shared_ptr<Hittable>>&	getHittables(void) const;
			std::string	getDefaultRenderOutputFileName(void) const;
			void		setDefaultRenderOutputFileName(std::string defaultRenderOutputFileName);
			void		updateLights(void);
		const std::vector<std::shared_ptr<Hittable>>& getLights(void) const;
		const std::vector<double>&	getLightSelectionCumulativeWeights(void) const;
		double		getLightSelectionTotalWeight(void) const;
		void		updateAccelerationStructure(void);
		const std::shared_ptr<Hittable>&	getAccelerationStructure(void) const;
		const std::vector<std::shared_ptr<Hittable>>&	getUnacceleratedHittables(void) const;
		bool		getStorePixelRenderTimes(void) const;
		void		setStorePixelRenderTimes(bool storePixelRenderTime);
		void		setPixelRenderTime(std::size_t x, std::size_t y, double renderTime);
		std::unique_ptr<Image>&	getImage(void);
		std::unique_ptr<Image>	generateRenderTimeImage(void) const;
		void		setIsFromFile(bool isFromFile);
		bool		getIsFromFile(void) const;
		void		setBenchmarkMode(bool benchmarkMode);
		bool		getBenchmarkMode(void) const;
			void		setRenderingThreads(std::size_t renderingThreads);
			std::size_t	getRenderingThreads(void) const;
			void		setBloom(bool bloom);
			bool		getBloom(void) const;
			void		setDenoise(bool denoise);
			bool		getDenoise(void) const;
			void		setDenoisedImage(std::unique_ptr<Image> denoisedImage);
			std::unique_ptr<Image>&	getDenoisedImage(void);
		const std::unique_ptr<Image>&	getDenoisedImage(void) const;
		bool		hasDenoisedImage(void) const;
		void		clearDenoisedImage(void);
		void		setDenoiseOutputFileName(std::string denoiseOutputFileName);
		std::string	getDenoiseOutputFileName(void) const;
		void		initializeDenoiseBuffers(std::size_t width, std::size_t height);
		Denoise::NFORBuffers*	getDenoiseBuffers(void);
		const Denoise::NFORBuffers*	getDenoiseBuffers(void) const;
		void		clearDenoiseBuffers(void);
		void		resetRenderStats(void);
		void		setRenderStats(SceneRenderStats renderStats);
		const SceneRenderStats&	getRenderStats(void) const;


	private:
		double					_t_max;
		int						_sampleCount;
		bool					_adaptiveSampling;
		int						_adaptiveMinSamples;
		int						_adaptiveBackgroundMinSamples;
		int						_adaptiveVolumeMinSamples;
		int						_adaptiveCheckInterval;
		double					_adaptiveThreshold;
		int						_maxLightBounces;
		int							_volumeGuidingTrainingSamples;
		std::uint32_t				_volumeGuidingResolution;
		std::uint32_t				_volumeGuidingLobes;
		double						_volumeGuidingAnisotropy;
		double						_volumeGuidingStrength;
		int							_volumeGuidingStartBounce;
		std::shared_ptr<VolumeGuidingField>	_volumeGuidingField;
		ViewTransform			_viewTransform;
			double					_exposure;
			double					_contrast;
			bool					_causticsEnabled;
			int						_causticPhotonCount;
			int						_causticPassCount;
			double					_causticRadiusMeters;
			double					_causticAlpha;
			std::shared_ptr<CausticPhotonMap>	_causticPhotonMap;
			std::string				_defaultRenderOutputFileName;
			std::unique_ptr<Image>	_image;
		double					_skyline;
		SkyTypes				_renderSky;
		bool					_distanceBlueness;
		double					_metersPerUnit;
		Atmosphere				_atmosphere;
		Color					_backgroundColor;
		std::shared_ptr<EnvironmentMap>	_environmentMap;
		double					_environmentStrength;
		bool					_environmentLighting;
		double					_environmentRotation;
		std::vector<Camera>		_cameras;
		size_t					_activeCamera;
		std::vector<std::shared_ptr<Hittable>>	_hittables;
		std::vector<std::shared_ptr<Hittable>>	_lights;
		std::vector<double>		_lightSelectionCumulativeWeights;
		double					_lightSelectionTotalWeight;
		std::shared_ptr<Hittable>	_accelerationStructure;
		std::vector<std::shared_ptr<Hittable>>	_unacceleratedHittables;
		bool					_storePixelRenderTimes;
		std::vector<double>		_pixelRenderTimes;
			bool					_isFromFile;
			bool					_benchmarkMode;
			std::size_t				_renderingThreads;
			bool					_bloom;
			bool					_denoise;
			std::unique_ptr<Image>	_denoisedImage;
		std::string				_denoiseOutputFileName;
		std::unique_ptr<Denoise::NFORBuffers>	_denoiseBuffers;
		SceneRenderStats		_renderStats;
};
