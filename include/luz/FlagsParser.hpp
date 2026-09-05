#pragma once

#include "Scene/Scene.hpp"
#include <vector>
#include <string>

class	FlagsParser
{
	public:
		FlagsParser(int argc, char** argv);
		void parse(Scene& scene);

	private:
		typedef std::vector<std::string> _stringVec;
		typedef _stringVec::iterator _iterator;

		_stringVec _args;

		_iterator	_findFlag(_stringVec flagVariations);
		_iterator	_findFlag(std::string flag);
		_iterator	_findPositionalFile(void);

		void	_parseHelp(void);
		void	_rejectRemovedFlags(void);
		void	_parseSeed(void);
		void	_parseFile(Scene& scene);
		void	_parseBenchmark(Scene& scene);
		void	_parseSamples(Scene& scene);
		void	_parseAdaptiveSampling(Scene& scene);
		void	_parseAdaptiveMinSamples(Scene& scene);
		void	_parseAdaptiveBackgroundMinSamples(Scene& scene);
		void	_parseAdaptiveVolumeMinSamples(Scene& scene);
		void	_parseAdaptiveThreshold(Scene& scene);
		void	_parseAdaptiveCheckInterval(Scene& scene);
		void	_parseMaxLightBounces(Scene& scene);
		void	_parseResolution(Scene& scene);
		void	_parseDetach(void);
		void	_parseThreads(Scene& scene);
		void	_parseViewTransform(Scene& scene);
		void	_parseBloom(Scene& scene);
		void	_parseExposure(Scene& scene);
		void	_parseContrast(Scene& scene);
		void	_parseCaustics(Scene& scene);
		void	_parseCausticPhotons(Scene& scene);
		void	_parseCausticPasses(Scene& scene);
		void	_parseCausticRadius(Scene& scene);
		void	_parseCausticAlpha(Scene& scene);
		void	_parseDenoise(Scene& scene);
		void	_parseOutput(Scene& scene);
		void	_parseDenoiseOutput(Scene& scene);
		void	_parseRenderTimes(Scene& scene);
};
