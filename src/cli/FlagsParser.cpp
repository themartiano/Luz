#include "FlagsParser.hpp"
#include "Random.hpp"
#include "SceneFile/SceneFile.hpp"
#include "Scene/SceneHelpers.hpp"
#include "Defaults.hpp"
#include "Utilities.hpp"
#include "Clock.hpp"
#include <algorithm>
#include <unistd.h>
#include <stdexcept>
#include <cstdlib>

namespace
{
	typedef std::vector<std::string>	StringVec;

	bool	parseOptionalBoolean(const std::string& value, bool& output)
	{
		if (value == "true" || value == "1")
		{
			output = true;
			return (true);
		}
		if (value == "false" || value == "0")
		{
			output = false;
			return (true);
		}

		return (false);
	}

	bool	isOneOf(const std::string& value, const StringVec& options)
	{
		return (std::find(options.begin(), options.end(), value) != options.end());
	}

	bool	isFlag(const std::string& value)
	{
		return (!value.empty() && value.rfind("-", 0) == 0);
	}

	void	printHelp(void)
	{
		std::cout
			<< "Usage: ./luz [scene.luz] [options]\n\n"
			<< "Arguments:\n"
			<< "  PATH                        Load a .luz scene file\n\n"
			<< "Options:\n"
			<< "  -f, --file PATH             Load a .luz scene file\n"
			<< "  -r, --resolution WxH        Override render resolution\n"
			<< "  -s, --samples N             Override samples per pixel\n"
			<< "  --adaptive [true|false]     Toggle adaptive sampling (default: true)\n"
			<< "  --no-adaptive               Disable adaptive sampling\n"
			<< "  --adaptive-min-samples N    Minimum samples before adaptive stopping\n"
			<< "  --adaptive-threshold F      Relative adaptive noise threshold\n"
			<< "  --adaptive-check-interval N Adaptive convergence check interval\n"
			<< "  -mlb, --maxLightBounces N   Override maximum light bounces\n"
			<< "      --max-light-bounces N   Alias for --maxLightBounces\n"
			<< "  -t, --threads N             Render with N worker threads\n"
			<< "  --seed N                    Seed random sampling\n"
			<< "  --view-transform standard|agx|aces|raw\n"
			<< "                               Select display transform; raw is debugging/HDR data, not viewing\n"
			<< "  --bloom true|false          Toggle bloom\n"
			<< "  --exposure EV              Exposure compensation in stops\n"
			<< "  --contrast F               Display contrast multiplier\n"
			<< "  --caustics [true|false]     Toggle caustic photon mapping\n"
			<< "  --no-caustics               Disable caustic photon mapping\n"
			<< "  --caustic-photons N         Photons emitted for caustic mapping\n"
			<< "  --caustic-passes N          Progressive caustic radius passes\n"
			<< "  --caustic-radius METERS     Initial caustic gather radius\n"
			<< "  --caustic-alpha F           Progressive caustic radius factor\n"
			<< "  --denoise [true|false]      Toggle denoised companion render (default: true)\n"
			<< "  --no-denoise                Disable denoising\n"
			<< "  -o, --output PATH.EXT       Override render output path\n"
			<< "  --denoise-output PATH.EXT   Override denoised output path\n"
			<< "  --render-times              Write renderTime.bmp\n"
			<< "  --benchmark                 Run the built-in benchmark scene\n"
			<< "  --benchmark-case NAME       Benchmark case: default, many-objects, mesh-bvh, diffuse, postprocess, atmosphere, lights, emissive-geometry, primitives-materials, volumes, obj-mesh\n";
	}
}

FlagsParser::FlagsParser(int argc, char** argv)
{
	this->_args = _stringVec(argv + 1, argv + argc);
}

void	FlagsParser::parse(Scene& scene)
{
	// Function call order is important since it'll determine the flag importance
	this->_parseHelp();
	this->_rejectRemovedFlags();
	this->_parseSeed();
	if (this->_findFlag("--benchmark") != this->_args.end())
	{
		scene.setBenchmarkMode(true);
	}
	this->_parseFile(scene);
	this->_parseBenchmark(scene);
	this->_parseSamples(scene);
	this->_parseAdaptiveSampling(scene);
	this->_parseAdaptiveMinSamples(scene);
	this->_parseAdaptiveThreshold(scene);
	this->_parseAdaptiveCheckInterval(scene);
	this->_parseMaxLightBounces(scene);
	this->_parseResolution(scene);
	this->_parseDetach();
	this->_parseThreads(scene);
	this->_parseViewTransform(scene);
	this->_parseBloom(scene);
	this->_parseExposure(scene);
	this->_parseContrast(scene);
	this->_parseCaustics(scene);
	this->_parseCausticPhotons(scene);
	this->_parseCausticPasses(scene);
	this->_parseCausticRadius(scene);
	this->_parseCausticAlpha(scene);
	this->_parseDenoise(scene);
	this->_parseOutput(scene);
	this->_parseDenoiseOutput(scene);
	this->_parseRenderTimes(scene);
}

void	FlagsParser::_parseHelp(void)
{
	auto it = this->_findFlag(_stringVec{"--help", "-h"});
	if (this->_args.empty() || it != this->_args.end())
	{
		printHelp();
		exit(EXIT_SUCCESS);
	}
}

void	FlagsParser::_rejectRemovedFlags(void)
{
	for (const std::string& arg : this->_args)
	{
		if (arg == "--compression" || arg.rfind("--compression=", 0) == 0)
		{
			throw std::runtime_error("--compression has been removed.");
		}
		if (arg == "--output-file" || arg.rfind("--output-file=", 0) == 0)
		{
			throw std::runtime_error("--output-file has been removed. Use --output PATH.EXT.");
		}
		if (arg == "--gamma" || arg.rfind("--gamma=", 0) == 0)
		{
			throw std::runtime_error("--gamma has been removed. Use --view-transform standard, agx, aces, or raw.");
		}
		if (
			arg == "--tonemapping"
			|| arg == "-tm"
			|| arg.rfind("--tonemapping=", 0) == 0
		)
		{
			throw std::runtime_error("--tonemapping has been removed. Use --view-transform standard, agx, aces, or raw.");
		}
	}
}

FlagsParser::_iterator	FlagsParser::_findFlag(_stringVec flagVariations)
{
	for (auto flag : flagVariations)
	{
		auto it = std::find(this->_args.begin(), this->_args.end(), flag);
		if (it != this->_args.end())
		{
			return (it);
		}
	}

	return (this->_args.end());
}

FlagsParser::_iterator	FlagsParser::_findFlag(std::string flag)
{
	return (_findFlag(_stringVec{flag}));
}

FlagsParser::_iterator	FlagsParser::_findPositionalFile(void)
{
	const StringVec valueFlags = {
		"--seed",
		"--file", "-f",
		"--benchmark-case",
		"--samples", "-s",
		"--adaptive-min-samples",
		"--adaptive-threshold",
		"--adaptive-check-interval",
		"--maxLightBounces", "--max-light-bounces", "-mlb",
		"--resolution", "-r",
		"--threads", "-t",
		"--view-transform",
		"--bloom",
		"--exposure",
		"--contrast",
		"--caustic-photons",
		"--caustic-passes",
		"--caustic-radius",
		"--caustic-alpha",
		"--output", "-o",
		"--denoise-output",
	};
	const StringVec optionalBooleanFlags = {
		"--adaptive",
		"--denoise",
		"--caustics",
	};

	for (auto it = this->_args.begin(); it != this->_args.end(); it++)
	{
		if (isOneOf(*it, valueFlags))
		{
			if (it + 1 != this->_args.end())
			{
				it++;
			}
			continue;
		}
		if (isOneOf(*it, optionalBooleanFlags))
		{
			bool ignored;

			if (it + 1 != this->_args.end() && parseOptionalBoolean(*(it + 1), ignored))
			{
				it++;
			}
			continue;
		}
		if (isFlag(*it))
		{
			continue;
		}
		return (it);
	}

	return (this->_args.end());
}

void	FlagsParser::_parseSeed(void)
{
	auto it = this->_findFlag("--seed");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--seed requires a value.");
		}
		setRandomSeed(std::stoi(*(it + 1)));
	}
}

void	FlagsParser::_parseFile(Scene& scene)
{
	auto it = this->_findFlag(_stringVec{"--file", "-f"});
	const bool explicitFileFlag = (it != this->_args.end());

	if (it == this->_args.end())
	{
		it = this->_findPositionalFile();
	}
	if (it != this->_args.end())
	{
		if (explicitFileFlag && it + 1 == this->_args.end())
		{
			throw std::runtime_error("--file requires a path.");
		}
		std::string filename = *it;
		if (explicitFileFlag)
		{
			filename = *(it + 1);
		}
		SceneFile::read(scene, filename);
	}
}

void	FlagsParser::_parseBenchmark(Scene& scene)
{
	auto it = this->_findFlag("--benchmark");

	if (it != this->_args.end())
	{
		std::string benchmarkCase = "default";
		auto caseIt = this->_findFlag("--benchmark-case");

		if (caseIt != this->_args.end())
		{
			if (caseIt + 1 == this->_args.end())
			{
				throw std::runtime_error("--benchmark-case requires a value.");
			}
			benchmarkCase = *(caseIt + 1);
		}

		scene.setBenchmarkMode(true);
		if (scene.getIsFromFile())
		{
			return;
		}
		scene.setIsFromFile(true); // We simulate that it read a scene file.

		Clock sceneBuildClock;
		sceneBuildClock.start();
		SceneHelpers::benchmark(scene, benchmarkCase);

		scene.getImage()->setWidth(200);
		scene.getImage()->setHeight(200);
		scene.getImage()->initialize();
		scene.setSampleCount(50);
		scene.setMaxLightBounces(8);
		scene.setViewTransform(ViewTransform::AgX);
		if (benchmarkCase != "atmosphere")
		{
			scene.setRenderSky(SKY_NONE);
		}
		scene.setDistanceBlueness(false);
		scene.setBackgroundColor(Color(0.0, 0.0, 0.0));
		SceneRenderStats stats = scene.getRenderStats();
		stats.sceneBuildMS += sceneBuildClock.elapsedMS();
		scene.setRenderStats(stats);
	}
}

void	FlagsParser::_parseSamples(Scene& scene)
{
	auto it = this->_findFlag(_stringVec{"--samples", "-s"});
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--samples requires a value.");
		}
		scene.setSampleCount(std::stoi(*(it + 1)));
	}
}

void	FlagsParser::_parseAdaptiveSampling(Scene& scene)
{
	for (auto it = this->_args.begin(); it != this->_args.end(); it++)
	{
		if (*it == "--adaptive")
		{
			if (it + 1 == this->_args.end() || (it + 1)->rfind("-", 0) == 0)
			{
				scene.setAdaptiveSampling(true);
			}
			else
			{
				bool adaptiveSampling;

				if (!parseOptionalBoolean(*(it + 1), adaptiveSampling))
				{
					throw std::runtime_error("Invalid value for --adaptive. Use true, false, 1, or 0.");
				}
				scene.setAdaptiveSampling(adaptiveSampling);
				it++;
			}
		}
		else if (*it == "--no-adaptive")
		{
			scene.setAdaptiveSampling(false);
		}
	}
}

void	FlagsParser::_parseAdaptiveMinSamples(Scene& scene)
{
	auto it = this->_findFlag("--adaptive-min-samples");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--adaptive-min-samples requires a value.");
		}
		scene.setAdaptiveMinSamples(std::stoi(*(it + 1)));
	}
}

void	FlagsParser::_parseAdaptiveThreshold(Scene& scene)
{
	auto it = this->_findFlag("--adaptive-threshold");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--adaptive-threshold requires a value.");
		}
		scene.setAdaptiveThreshold(std::stod(*(it + 1)));
	}
}

void	FlagsParser::_parseAdaptiveCheckInterval(Scene& scene)
{
	auto it = this->_findFlag("--adaptive-check-interval");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--adaptive-check-interval requires a value.");
		}
		scene.setAdaptiveCheckInterval(std::stoi(*(it + 1)));
	}
}

void	FlagsParser::_parseMaxLightBounces(Scene& scene)
{
	auto it = this->_findFlag(_stringVec{"--maxLightBounces", "--max-light-bounces", "-mlb"});
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--maxLightBounces requires a value.");
		}
		scene.setMaxLightBounces(std::stoi(*(it + 1)));
	}
}

void	FlagsParser::_parseResolution(Scene& scene)
{
	auto it = this->_findFlag(_stringVec{"--resolution", "-r"});
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--resolution requires a value.");
		}
		std::string res = *(it + 1);
		const std::size_t separator = res.find("x");
		if (separator == std::string::npos || separator == 0 || separator == res.length() - 1)
		{
			throw std::runtime_error("--resolution must use WIDTHxHEIGHT format.");
		}
		long long width = std::stoll(res.substr(0, separator));
		long long height = std::stoll(res.substr(separator + 1));
		if (width <= 0 || height <= 0)
		{
			throw std::runtime_error("--resolution dimensions must be positive.");
		}
		scene.getImage()->setWidth(static_cast<std::size_t>(width));
		scene.getImage()->setHeight(static_cast<std::size_t>(height));
		scene.getImage()->initialize();
	}
}

void	FlagsParser::_parseDetach(void)
{
	auto it = this->_findFlag(_stringVec{"--detach", "--detached", "-d"});
	if (it != this->_args.end())
	{
		#if defined __linux__ || defined __APPLE__
			int pid = fork();
			if (pid < 0)
			{
				std::cerr << "An error occurred while detaching the process." << std::endl;
				exit(EXIT_FAILURE);
			}
			else if (pid > 0) // Exits the original process
			{
				std::cout << "Process detached with success. PID: " << pid << std::endl;
				exit(EXIT_SUCCESS);
			}
			else // Sets the new process as the group owner and closes stdin, stdou and stderr.
			{
				setsid();
				close(STDIN_FILENO);
				close(STDOUT_FILENO);
				close(STDERR_FILENO);
			}
		#else
			std::cerr << "Detaching is not supported on this platform." << std::endl;
			exit(EXIT_FAILURE);
		#endif
	}
}

void	FlagsParser::_parseThreads(Scene& scene)
{
	auto it = this->_findFlag(_stringVec{"--threads", "-t"});
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--threads requires a value.");
		}
		int threads = std::stoi(*(it + 1));
		if (threads <= 0)
		{
			throw std::runtime_error("--threads must be positive.");
		}
		scene.setRenderingThreads(static_cast<std::size_t>(threads));
	}
}

void	FlagsParser::_parseViewTransform(Scene& scene)
{
	auto it = this->_findFlag("--view-transform");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--view-transform requires a value.");
		}
		std::string viewTransform = *(it + 1);

		Utilities::toLower(viewTransform);
		if (viewTransform == "standard")
		{
			scene.setViewTransform(ViewTransform::Standard);
		}
		else if (viewTransform == "agx")
		{
			scene.setViewTransform(ViewTransform::AgX);
		}
		else if (viewTransform == "aces")
		{
			scene.setViewTransform(ViewTransform::ACES);
		}
		else if (viewTransform == "raw")
		{
			scene.setViewTransform(ViewTransform::Raw);
		}
		else
		{
			throw std::runtime_error("Invalid value for --view-transform. Use standard, agx, aces, or raw. Raw is for debugging/HDR data, not display viewing.");
		}
	}
}

void	FlagsParser::_parseBloom(Scene& scene)
{
	auto it = this->_findFlag("--bloom");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--bloom requires a value.");
		}
		std::string bloom = *(it + 1);
		if (bloom == "true" || bloom == "1")
		{
			scene.setBloom(true);
		}
		else if (bloom == "false" || bloom == "0")
		{
			scene.setBloom(false);
		}
		else
		{
			throw std::runtime_error("Invalid value for --bloom. Use true, false, 1, or 0.");
		}
	}
}

void	FlagsParser::_parseExposure(Scene& scene)
{
	auto it = this->_findFlag("--exposure");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--exposure requires a value.");
		}
		scene.setExposure(std::stod(*(it + 1)));
	}
}

void	FlagsParser::_parseContrast(Scene& scene)
{
	auto it = this->_findFlag("--contrast");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--contrast requires a value.");
		}
		scene.setContrast(std::stod(*(it + 1)));
	}
}

void	FlagsParser::_parseCaustics(Scene& scene)
{
	for (auto it = this->_args.begin(); it != this->_args.end(); it++)
	{
		if (*it == "--caustics")
		{
			if (it + 1 == this->_args.end() || (it + 1)->rfind("-", 0) == 0)
			{
				scene.setCausticsEnabled(true);
			}
			else
			{
				bool caustics;

				if (!parseOptionalBoolean(*(it + 1), caustics))
				{
					throw std::runtime_error("Invalid value for --caustics. Use true, false, 1, or 0.");
				}
				scene.setCausticsEnabled(caustics);
				it++;
			}
		}
		else if (*it == "--no-caustics")
		{
			scene.setCausticsEnabled(false);
		}
	}
}

void	FlagsParser::_parseCausticPhotons(Scene& scene)
{
	auto it = this->_findFlag("--caustic-photons");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--caustic-photons requires a value.");
		}
		scene.setCausticPhotonCount(std::stoi(*(it + 1)));
	}
}

void	FlagsParser::_parseCausticPasses(Scene& scene)
{
	auto it = this->_findFlag("--caustic-passes");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--caustic-passes requires a value.");
		}
		scene.setCausticPassCount(std::stoi(*(it + 1)));
	}
}

void	FlagsParser::_parseCausticRadius(Scene& scene)
{
	auto it = this->_findFlag("--caustic-radius");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--caustic-radius requires a value.");
		}
		scene.setCausticRadiusMeters(std::stod(*(it + 1)));
	}
}

void	FlagsParser::_parseCausticAlpha(Scene& scene)
{
	auto it = this->_findFlag("--caustic-alpha");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--caustic-alpha requires a value.");
		}
		scene.setCausticAlpha(std::stod(*(it + 1)));
	}
}

void	FlagsParser::_parseDenoise(Scene& scene)
{
	for (auto it = this->_args.begin(); it != this->_args.end(); it++)
	{
		if (*it == "--denoise")
		{
			if (it + 1 == this->_args.end() || (it + 1)->rfind("-", 0) == 0)
			{
				scene.setDenoise(true);
			}
			else
			{
				bool denoise;

				if (!parseOptionalBoolean(*(it + 1), denoise))
				{
					throw std::runtime_error("Invalid value for --denoise. Use true, false, 1, or 0.");
				}
				scene.setDenoise(denoise);
				it++;
			}
		}
		else if (*it == "--no-denoise")
		{
			scene.setDenoise(false);
		}
	}
}

void	FlagsParser::_parseOutput(Scene& scene)
{
	auto it = this->_findFlag(_stringVec{"--output", "-o"});
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--output requires a path.");
		}
		scene.setDefaultRenderOutputFileName(*(it + 1));
	}
}

void	FlagsParser::_parseDenoiseOutput(Scene& scene)
{
	auto it = this->_findFlag("--denoise-output");
	if (it != this->_args.end())
	{
		if (it + 1 == this->_args.end())
		{
			throw std::runtime_error("--denoise-output requires a path.");
		}
		scene.setDenoiseOutputFileName(*(it + 1));
	}
}

void	FlagsParser::_parseRenderTimes(Scene& scene)
{
	auto it = this->_findFlag("--render-times");
	if (it != this->_args.end())
	{
		scene.setStorePixelRenderTimes(true);
	}
}
