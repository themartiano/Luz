#include "SceneFile/SceneFile.hpp"
#include "SceneFileInternal.hpp"
#include "ANSIColors.hpp"
#include "Clock.hpp"
#include "Utilities.hpp"
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace
{
	const std::size_t	MAX_CONCURRENT_MESH_LOADS = 4;

	bool	splitAssignment(const std::string& line, std::string& key, std::string& value)
	{
		const std::size_t separator = line.find('=');

		if (separator == std::string::npos || separator == 0 || separator == line.length() - 1)
		{
			return (false);
		}
		key = SceneFile::internal::_lowerCopy(SceneFile::internal::_trim(line.substr(0, separator)));
		value = SceneFile::internal::_trim(line.substr(separator + 1));

		return (true);
	}

	void	addResolvedMeshFile(
		std::unordered_set<std::string>& meshFiles,
		const std::filesystem::path& baseDirectory,
		const std::string& meshFile
	)
	{
		if (!meshFile.empty())
		{
			meshFiles.insert(SceneFile::internal::_resolveAssetPath(baseDirectory, meshFile));
		}
	}

	void	collectNamedMeshes(
		std::ifstream& stream,
		const std::filesystem::path& baseDirectory,
		std::unordered_map<std::string, std::string>& meshes
	)
	{
		std::string line;

		do
		{
			getline(stream, line);
			const std::string trimmedLine = SceneFile::internal::_trim(line);

			if (trimmedLine.empty())
			{
				break;
			}
			if (trimmedLine.at(0) == '#')
			{
				continue;
			}

			std::string meshName;
			if (!SceneFile::internal::_parseNamedBlockHeader(trimmedLine, "mesh", meshName))
			{
				continue;
			}

			std::string meshFile;
			do
			{
				getline(stream, line);
				const std::string blockLine = SceneFile::internal::_trim(line);

				if (blockLine.empty() || blockLine.at(0) == '#')
				{
					continue;
				}
				if (blockLine == "}")
				{
					if (!meshFile.empty())
					{
						meshes[meshName] = SceneFile::internal::_resolveAssetPath(baseDirectory, meshFile);
					}
					break;
				}

				std::string key;
				std::string value;
				if (splitAssignment(blockLine, key, value) && (key == "file" || key == "path"))
				{
					meshFile = value;
				}
			} while (!stream.eof());
		} while (!stream.eof());
	}

	void	collectObjectBlockMeshFile(
		std::ifstream& stream,
		const std::filesystem::path& baseDirectory,
		const std::unordered_map<std::string, std::string>& meshes,
		std::unordered_set<std::string>& meshFiles
	)
	{
		std::string line;
		std::string meshName;
		std::string objectFile;

		do
		{
			getline(stream, line);
			const std::string blockLine = SceneFile::internal::_trim(line);

			if (blockLine.empty() || blockLine.at(0) == '#')
			{
				continue;
			}
			if (blockLine == "}")
			{
				if (!objectFile.empty())
				{
					addResolvedMeshFile(meshFiles, baseDirectory, objectFile);
					return;
				}
				const auto meshIt = meshes.find(meshName);
				if (meshIt != meshes.end())
				{
					meshFiles.insert(meshIt->second);
				}
				return;
			}

			std::string key;
			std::string value;
			if (!splitAssignment(blockLine, key, value))
			{
				continue;
			}
			if (key == "mesh")
			{
				meshName = value;
			}
			else if (key == "file" || key == "path")
			{
				objectFile = value;
			}
		} while (!stream.eof());
	}

	void	collectCompactObjMeshFile(
		const std::string& line,
		const std::filesystem::path& baseDirectory,
		std::unordered_set<std::string>& meshFiles
	)
	{
		std::string value = SceneFile::internal::_trim(line.substr(std::string("obj=").size()));
		const std::size_t separator = value.find(',');

		if (separator != std::string::npos)
		{
			value = SceneFile::internal::_trim(value.substr(0, separator));
		}
		addResolvedMeshFile(meshFiles, baseDirectory, value);
	}

	std::size_t	countSceneMeshLoads(const std::string& fileName)
	{
		std::ifstream stream(fileName);
		std::string line;
		std::unordered_map<std::string, std::string> meshes;
		std::unordered_set<std::string> meshFiles;
		const std::filesystem::path baseDirectory = std::filesystem::path(fileName).parent_path();
		bool inSceneSection = false;

		while (getline(stream, line))
		{
			const std::string trimmedLine = SceneFile::internal::_trim(line);
			const std::string lowerLine = SceneFile::internal::_lowerCopy(trimmedLine);

			if (lowerLine.empty() || lowerLine.at(0) == '#')
			{
				continue;
			}
			if (lowerLine.rfind("[meshes]", 0) != std::string::npos)
			{
				collectNamedMeshes(stream, baseDirectory, meshes);
				inSceneSection = false;
				continue;
			}
			if (lowerLine.rfind("[scene]", 0) != std::string::npos)
			{
				inSceneSection = true;
				continue;
			}
			if (!inSceneSection)
			{
				continue;
			}

			std::string objectName;
			if (SceneFile::internal::_parseNamedBlockHeader(trimmedLine, "object", objectName))
			{
				collectObjectBlockMeshFile(stream, baseDirectory, meshes, meshFiles);
				continue;
			}
			if (lowerLine.rfind("obj=", 0) == 0)
			{
				collectCompactObjMeshFile(trimmedLine, baseDirectory, meshFiles);
			}
		}

		return (meshFiles.size());
	}

	std::size_t	countSceneMeshUses(const std::string& fileName)
	{
		std::ifstream stream(fileName);
		std::string line;
		std::size_t meshUses = 0;

		while (getline(stream, line))
		{
			const std::string lowerLine = SceneFile::internal::_lowerCopy(SceneFile::internal::_trim(line));

			if (lowerLine.empty() || lowerLine.at(0) == '#')
			{
				continue;
			}
			if (lowerLine.rfind("object ", 0) == 0 || lowerLine.rfind("obj=", 0) == 0)
			{
				meshUses++;
			}
		}

		return (meshUses);
	}

	std::size_t	meshLoadConcurrency(std::size_t meshLoads)
	{
		if (meshLoads <= 1)
		{
			return (1);
		}

		const unsigned int hardwareConcurrency = std::thread::hardware_concurrency();
		const std::size_t hardwareLimit = hardwareConcurrency == 0 ? 2 : hardwareConcurrency;
		const std::size_t cappedHardwareLimit = std::min(hardwareLimit, MAX_CONCURRENT_MESH_LOADS);

		return (std::max<std::size_t>(1, std::min(meshLoads, cappedHardwareLimit)));
	}
}

// Searches and reads / parses the Scene file named 'fileName' into 'Scene' (searches in the current directory)
void	SceneFile::read(Scene& scene, std::string fileName)
{
	Clock sceneFileClock;

	sceneFileClock.start();
	scene.setIsFromFile(true);

	std::ifstream stream;
	stream.open(fileName);
	if (!stream)
	{
		throw std::runtime_error("Scene file could not be opened: " + fileName);
	}

	const std::filesystem::path scenePath(fileName);
	ObjLoadProgress meshLoadProgress;
	internal::SceneFileContext context;
	context.baseDirectory = scenePath.parent_path();
	meshLoadProgress.total = countSceneMeshLoads(fileName);
	meshLoadProgress.outputEnabled = !scene.getBenchmarkMode();
	context.meshLoadConcurrency = meshLoadConcurrency(std::max(meshLoadProgress.total, countSceneMeshUses(fileName)));
	if (meshLoadProgress.total > 0)
	{
		context.meshLoadProgress = &meshLoadProgress;
	}

	std::string line;
	do
	{
		getline(stream, line);

		if (line.length() <= 0 || line.at(0) == '#')
		{
			continue;
		}
		std::string lowerLine = line;
		Utilities::toLower(lowerLine);

		if (lowerLine.rfind("[settings]", 0) != std::string::npos)
		{
			internal::_readSettingsSection(scene, stream, context);
		}
		else if (lowerLine.rfind("[spectra]", 0) != std::string::npos)
		{
			internal::_readNamedSpectraSection(stream, context);
		}
		else if (lowerLine.rfind("[materials]", 0) != std::string::npos)
		{
			internal::_readNamedMaterialsSection(stream, context);
		}
		else if (lowerLine.rfind("[meshes]", 0) != std::string::npos)
		{
			internal::_readNamedMeshesSection(stream, context);
		}
		else if (lowerLine.rfind("[scene]", 0) != std::string::npos)
		{
			internal::_readSceneSection(scene, stream, context);
		}
		else
		{
			throw std::runtime_error("Unknown scene-file section: " + line);
		}
	} while (!stream.eof());

	for (const std::shared_future<std::shared_ptr<Hittable>>& pendingMeshLoad : context.pendingMeshLoads)
	{
		// Joining every asynchronous load also propagates loader exceptions. Keep
		// the returned shared pointer alive for the duration of the join and make
		// the intentional discard explicit for toolchains that mark future::get()
		// as [[nodiscard]].
		const std::shared_ptr<Hittable> loadedMesh = pendingMeshLoad.get();
		(void)loadedMesh;
	}
	scene.syncAtmosphereSunDirection();
	const double sceneFileMS = sceneFileClock.elapsedMS();
	SceneRenderStats stats = scene.getRenderStats();
	stats.modelLoadMS += meshLoadProgress.elapsedMS;
	stats.sceneBuildMS += std::max(0.0, sceneFileMS - meshLoadProgress.elapsedMS);
	stats.modelLoadProgressShown = stats.modelLoadProgressShown
		|| (meshLoadProgress.started && meshLoadProgress.outputEnabled);
	scene.setRenderStats(stats);
}
