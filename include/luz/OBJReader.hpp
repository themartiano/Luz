#pragma once

#include "Hittables/Mesh.hpp"
#include "TerminalProgress.hpp"
#include "Vector3.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct	ObjMeshFaceVertex
{
	int	vertexIndex = -1;
	int	textureIndex = -1;
	int	normalIndex = -1;
};

struct	ObjMeshData
{
	std::vector<Vector3>	vertices;
	std::vector<Vector3>	textureCoordinates;
	std::vector<Vector3>	normals;
	std::vector<std::array<ObjMeshFaceVertex, 3>>	triangles;
};

struct	ObjLoadProgress
{
	std::size_t	total = 0;
	std::size_t	loaded = 0;
	std::size_t	skippedDegenerateTriangles = 0;
	bool		started = false;
	bool		outputEnabled = true;
	double		elapsedMS = 0.0;
	std::chrono::steady_clock::time_point	startTime;
	std::unique_ptr<TerminalProgress::PhaseProgress>	reporter;
	std::mutex	mutex;
};

struct	ObjReadOptions
{
	bool	quiet = false;
	ObjLoadProgress*	progress = nullptr;
};

ObjMeshData	readObjMeshData(std::string fileName);
ObjMeshData	readObjMeshData(std::string fileName, const ObjReadOptions& options);
Mesh	buildObjMesh(
	const ObjMeshData& data,
	Vector3 positionOffset,
	Vector3 rotationDegrees,
	Vector3 scale,
	std::shared_ptr<Material> material
);
Mesh	readObj(std::string fileName);
Mesh	readObj(std::string fileName, Vector3 positionOffset, std::shared_ptr<Material> material);
Mesh	readObj(std::string fileName, Vector3 positionOffset, Vector3 rotationDegrees, Vector3 scale, std::shared_ptr<Material> material);
Mesh	readObj(
	std::string fileName,
	Vector3 positionOffset,
	Vector3 rotationDegrees,
	Vector3 scale,
	std::shared_ptr<Material> material,
	const ObjReadOptions& options
);
