#include "OBJReader.hpp"
#include "Hittables/Hittable.hpp"
#include "Hittables/BVHNode.hpp"
#include "Hittables/Triangle.hpp"
#include "Hittables/Mesh.hpp"
#include "Vector3.hpp"
#include "Utilities.hpp"
#include "ANSIColors.hpp"
#include "Clock.hpp"
#include "Materials/Lambertian.hpp"
#include "Random.hpp"
#include <memory>
#include <vector>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <string>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <cmath>
#include <ios>
#include <limits>
#include <utility>

namespace
{
	const double	DEGENERATE_TRIANGLE_EPSILON_SQUARED = 1e-24;
	const double	DEGREES_TO_RADIANS = 3.14159265358979323846 / 180.0;
	const double	TRANSFORM_EPSILON = 1e-12;

	bool	nearlyEqual(double a, double b)
	{
		return (std::fabs(a - b) <= TRANSFORM_EPSILON);
	}

	bool	isZeroVector(const Vector3& vector)
	{
		return (
			nearlyEqual(vector.getX(), 0.0)
			&& nearlyEqual(vector.getY(), 0.0)
			&& nearlyEqual(vector.getZ(), 0.0)
		);
	}

	bool	isUnitScale(const Vector3& scale)
	{
		return (
			nearlyEqual(scale.getX(), 1.0)
			&& nearlyEqual(scale.getY(), 1.0)
			&& nearlyEqual(scale.getZ(), 1.0)
		);
	}

	Vector3	normalizedOrZero(const Vector3& normal)
	{
		const double lengthSquared = Utilities::vectorLengthSquared(normal);

		if (lengthSquared <= DEGENERATE_TRIANGLE_EPSILON_SQUARED)
		{
			return (Vector3());
		}
		if (nearlyEqual(lengthSquared, 1.0))
		{
			return (normal);
		}
		return (Utilities::normalize(normal));
	}

	struct ObjTransform
	{
		Vector3	positionOffset;
		Vector3	scale;
		double	cosX = 1.0;
		double	sinX = 0.0;
		double	cosY = 1.0;
		double	sinY = 0.0;
		double	cosZ = 1.0;
		double	sinZ = 0.0;
		bool	identity = true;
		bool	normalIdentity = true;

		ObjTransform(Vector3 positionOffset, Vector3 rotationDegrees, Vector3 scale)
			: positionOffset(positionOffset), scale(scale)
		{
			const bool rotationIdentity = isZeroVector(rotationDegrees);
			const bool scaleIdentity = isUnitScale(scale);

			this->identity = isZeroVector(positionOffset) && rotationIdentity && scaleIdentity;
			this->normalIdentity = rotationIdentity && scaleIdentity;
			if (rotationIdentity)
			{
				return;
			}

			const double xRadians = rotationDegrees.getX() * DEGREES_TO_RADIANS;
			const double yRadians = rotationDegrees.getY() * DEGREES_TO_RADIANS;
			const double zRadians = rotationDegrees.getZ() * DEGREES_TO_RADIANS;

			this->cosX = std::cos(xRadians);
			this->sinX = std::sin(xRadians);
			this->cosY = std::cos(yRadians);
			this->sinY = std::sin(yRadians);
			this->cosZ = std::cos(zRadians);
			this->sinZ = std::sin(zRadians);
		}

		Vector3	rotate(Vector3 vector) const
		{
			double x = vector.getX();
			double y = vector.getY();
			double z = vector.getZ();

			double nextY = (y * this->cosX) - (z * this->sinX);
			double nextZ = (y * this->sinX) + (z * this->cosX);
			y = nextY;
			z = nextZ;

			double nextX = (x * this->cosY) + (z * this->sinY);
			nextZ = (-x * this->sinY) + (z * this->cosY);
			x = nextX;
			z = nextZ;

			nextX = (x * this->cosZ) - (y * this->sinZ);
			nextY = (x * this->sinZ) + (y * this->cosZ);
			x = nextX;
			y = nextY;

			return (Vector3(x, y, z));
		}

		Vector3	transformVertex(const Vector3& vertex) const
		{
			if (this->identity)
			{
				return (vertex);
			}
			return (this->rotate(vertex * this->scale) + this->positionOffset);
		}

		Vector3	transformNormal(Vector3 normal) const
		{
			if (this->normalIdentity)
			{
				return (normalizedOrZero(normal));
			}
			normal.setX(this->scale.getX() == 0.0 ? normal.getX() : normal.getX() / this->scale.getX());
			normal.setY(this->scale.getY() == 0.0 ? normal.getY() : normal.getY() / this->scale.getY());
			normal.setZ(this->scale.getZ() == 0.0 ? normal.getZ() : normal.getZ() / this->scale.getZ());
			return (normalizedOrZero(this->rotate(normal)));
		}
	};

	ObjMeshData	parseObjFile(std::ifstream& stream);
	ObjMeshData	loadObjMeshDataFromFile(const std::string& fileName);
	std::vector<Triangle>	buildObjTriangles(
		const ObjMeshData& data,
		Vector3 positionOffset,
		Vector3 rotationDegrees,
		Vector3 scale,
		std::shared_ptr<Material> material,
		std::size_t* skippedDegenerateTriangles
	);

	Vector3	parseVectorValues(const char* value, const std::string& line)
	{
		double x;
		double y;
		double z;

		if (std::sscanf(value, "%lf %lf %lf", &x, &y, &z) != 3)
		{
			throw std::runtime_error("Invalid OBJ vector line: " + line);
		}
		return (Vector3(x, y, z));
	}

	Vector3	parseTextureValues(const char* value, const std::string& line)
	{
		double u;
		double v;

		if (std::sscanf(value, "%lf %lf", &u, &v) != 2)
		{
			throw std::runtime_error("Invalid OBJ texture coordinate line: " + line);
		}
		return (Vector3(u, v, 0.0));
	}

	void	reserveObjStorage(
		std::ifstream& stream,
		std::vector<Vector3>& vertices,
		std::vector<Vector3>& textureCoordinates,
		std::vector<Vector3>& normals,
		std::size_t& triangleReserveHint
	)
	{
		const std::streampos start = stream.tellg();
		if (start == std::streampos(-1))
		{
			return;
		}

		stream.seekg(0, std::ios::end);
		const std::streampos end = stream.tellg();
		stream.seekg(start);
		if (end == std::streampos(-1) || end <= start)
		{
			return;
		}

		const std::size_t byteCount = static_cast<std::size_t>(end - start);
		const std::size_t estimatedLines = std::max<std::size_t>(16, byteCount / 48);

		vertices.reserve(estimatedLines / 2);
		textureCoordinates.reserve(estimatedLines / 3);
		normals.reserve(estimatedLines / 3);
		triangleReserveHint = estimatedLines / 2;
	}

	int	resolveObjIndex(int index, std::size_t count, const std::string& token)
	{
		int resolvedIndex;

		if (index > 0)
		{
			resolvedIndex = index - 1;
		}
		else if (index < 0)
		{
			resolvedIndex = static_cast<int>(count) + index;
		}
		else
		{
			throw std::runtime_error("OBJ indices are 1-based: " + token);
		}
		if (resolvedIndex < 0 || static_cast<std::size_t>(resolvedIndex) >= count)
		{
			throw std::runtime_error("OBJ index out of range: " + token);
		}
		return (resolvedIndex);
	}

	bool	isTokenEnd(char character)
	{
		return (
			character == '\0'
			|| std::isspace(static_cast<unsigned char>(character))
		);
	}

	void	skipSpaces(const char*& cursor)
	{
		while (std::isspace(static_cast<unsigned char>(*cursor)))
		{
			cursor++;
		}
	}

	int	parseObjIndexToken(const char*& cursor, const std::string& line)
	{
		char* end = nullptr;
		const long value = std::strtol(cursor, &end, 10);

		if (end == cursor)
		{
			throw std::runtime_error("Invalid OBJ face vertex index: " + line);
		}
		if (
			value < static_cast<long>(std::numeric_limits<int>::min())
			|| value > static_cast<long>(std::numeric_limits<int>::max())
		)
		{
			throw std::runtime_error("OBJ face vertex index is out of range: " + line);
		}
		cursor = end;
		return (static_cast<int>(value));
	}

	ObjMeshFaceVertex	parseFaceVertex(
		const char*& cursor,
		std::size_t vertexCount,
		std::size_t textureCount,
		std::size_t normalCount,
		const std::string& line
	)
	{
		ObjMeshFaceVertex faceVertex;

		skipSpaces(cursor);
		if (*cursor == '\0')
		{
			throw std::runtime_error("OBJ face vertex is missing a position index: " + line);
		}
		faceVertex.vertexIndex = resolveObjIndex(parseObjIndexToken(cursor, line), vertexCount, line);
		if (*cursor == '/')
		{
			cursor++;
			if (*cursor != '/' && !isTokenEnd(*cursor))
			{
				faceVertex.textureIndex = resolveObjIndex(parseObjIndexToken(cursor, line), textureCount, line);
			}
			if (*cursor == '/')
			{
				cursor++;
				if (!isTokenEnd(*cursor))
				{
					faceVertex.normalIndex = resolveObjIndex(parseObjIndexToken(cursor, line), normalCount, line);
				}
			}
		}
		if (!isTokenEnd(*cursor))
		{
			throw std::runtime_error("Invalid OBJ face vertex: " + line);
		}

		return (faceVertex);
	}

	Triangle	buildTriangle(
		const ObjMeshData& data,
		const ObjMeshFaceVertex& faceVertex0,
		const ObjMeshFaceVertex& faceVertex1,
		const ObjMeshFaceVertex& faceVertex2,
		const ObjTransform& transform,
		std::shared_ptr<Material> material
	)
	{
		const Vector3 vertex0 = transform.transformVertex(data.vertices.at(faceVertex0.vertexIndex));
		const Vector3 vertex1 = transform.transformVertex(data.vertices.at(faceVertex1.vertexIndex));
		const Vector3 vertex2 = transform.transformVertex(data.vertices.at(faceVertex2.vertexIndex));
		Triangle triangle(vertex0, vertex1, vertex2, material);

		if (faceVertex0.normalIndex >= 0 && faceVertex1.normalIndex >= 0 && faceVertex2.normalIndex >= 0)
		{
			triangle.setVertexNormals(
				transform.transformNormal(data.normals.at(faceVertex0.normalIndex)),
				transform.transformNormal(data.normals.at(faceVertex1.normalIndex)),
				transform.transformNormal(data.normals.at(faceVertex2.normalIndex))
			);
		}
		if (faceVertex0.textureIndex >= 0 && faceVertex1.textureIndex >= 0 && faceVertex2.textureIndex >= 0)
		{
			triangle.setTextureCoordinates(
				data.textureCoordinates.at(faceVertex0.textureIndex),
				data.textureCoordinates.at(faceVertex1.textureIndex),
				data.textureCoordinates.at(faceVertex2.textureIndex)
			);
		}

		return (triangle);
	}

	void	beginMeshLoadProgress(ObjLoadProgress& progress)
	{
		std::lock_guard<std::mutex> lock(progress.mutex);

		if (!progress.started)
		{
			progress.started = true;
			progress.startTime = std::chrono::steady_clock::now();
			if (progress.outputEnabled)
			{
				progress.reporter = std::make_unique<TerminalProgress::PhaseProgress>(
					"Model load",
					true
				);
			}
		}
	}

	void	finishMeshLoadProgress(ObjLoadProgress& progress, std::size_t skippedDegenerateTriangles)
	{
		std::lock_guard<std::mutex> lock(progress.mutex);

		if (progress.loaded < progress.total)
		{
			progress.loaded++;
		}
		progress.skippedDegenerateTriangles += skippedDegenerateTriangles;
		if (progress.outputEnabled && progress.reporter)
		{
			const unsigned int percent = progress.total == 0
				? 100u
				: static_cast<unsigned int>((progress.loaded * 100u) / progress.total);

			progress.reporter->update(percent);
		}
		if (progress.loaded >= progress.total)
		{
			const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - progress.startTime;

			progress.elapsedMS = elapsed.count() * 1000.0;
			if (progress.outputEnabled && progress.reporter)
			{
				progress.reporter->finish(progress.elapsedMS);
				if (progress.skippedDegenerateTriangles > 0)
				{
					std::cout
						<< CLR_YELLOW << "  Skipped " << CLR_WHITE << progress.skippedDegenerateTriangles
						<< CLR_YELLOW << " degenerate OBJ triangle"
						<< (progress.skippedDegenerateTriangles == 1 ? "" : "s")
						<< "." << CLR_RESET << std::endl;
				}
			}
		}
	}

	ObjMeshData	loadObjMeshDataFromFile(const std::string& fileName)
	{
		std::ifstream stream;
		stream.open(fileName);
		if (!stream)
		{
			throw std::runtime_error("OBJ file could not be opened: " + fileName);
		}

		return (parseObjFile(stream));
	}

	std::vector<Triangle>	buildObjTriangles(
		const ObjMeshData& data,
		Vector3 positionOffset,
		Vector3 rotationDegrees,
		Vector3 scale,
		std::shared_ptr<Material> material,
		std::size_t* skippedDegenerateTriangles
	)
	{
		const ObjTransform transform(positionOffset, rotationDegrees, scale);
		std::vector<Triangle> triangles;
		std::size_t skipped = 0;

		triangles.reserve(data.triangles.size());
		for (const std::array<ObjMeshFaceVertex, 3>& face : data.triangles)
		{
			Triangle triangle = buildTriangle(
				data,
				face[0],
				face[1],
				face[2],
				transform,
				material
			);
			if (triangle.area() <= DEGENERATE_TRIANGLE_EPSILON_SQUARED)
			{
				skipped++;
				continue;
			}
			triangles.push_back(std::move(triangle));
		}
		if (skippedDegenerateTriangles != nullptr)
		{
			*skippedDegenerateTriangles = skipped;
		}

		return (triangles);
	}
}

ObjMeshData	readObjMeshData(std::string fileName)
{
	return (readObjMeshData(fileName, ObjReadOptions()));
}

ObjMeshData	readObjMeshData(std::string fileName, const ObjReadOptions& options)
{
	Clock	clock;
	const bool useProgress = options.progress != nullptr && options.progress->total > 0;

	if (useProgress)
	{
		beginMeshLoadProgress(*options.progress);
	}
	else if (!options.quiet)
	{
		std::cout << CLR_YELLOW << "Parsing " << CLR_BLUE << fileName << CLR_YELLOW << "..." << CLR_RESET << std::endl;
	}

	clock.start();
	ObjMeshData data = loadObjMeshDataFromFile(fileName);

	if (useProgress)
	{
		finishMeshLoadProgress(*options.progress, 0);
	}
	else if (!options.quiet)
	{
		std::cout << CLR_BLUE << fileName << CLR_GREEN_BRIGHT << " parsing done! " << CLR_BLUE_BRIGHT << "(Duration: " << CLR_WHITE << clock.elapsedS() << "s" << CLR_BLUE_BRIGHT << ")\n\n" << CLR_RESET;
	}

	return (data);
}

Mesh	buildObjMesh(
	const ObjMeshData& data,
	Vector3 positionOffset,
	Vector3 rotationDegrees,
	Vector3 scale,
	std::shared_ptr<Material> material
)
{
	return (Mesh(
		Vector3(),
		material,
		buildObjTriangles(data, positionOffset, rotationDegrees, scale, material, nullptr)
	));
}

// Calls the actual 'readObj' function with a zeroed offset position
Mesh	readObj(std::string fileName)
{
	return (readObj(fileName, Vector3(0.0, 0.0, 0.0), std::make_shared<Lambertian>(Color(0.6, 0.6, 0.6))));
}

// Search and read / parse the obj file named 'fileName' (current directory)
Mesh	readObj(std::string fileName, Vector3 positionOffset, std::shared_ptr<Material> material)
{
	return (readObj(fileName, positionOffset, Vector3(0.0, 0.0, 0.0), Vector3(1.0, 1.0, 1.0), material));
}

Mesh	readObj(std::string fileName, Vector3 positionOffset, Vector3 rotationDegrees, Vector3 scale, std::shared_ptr<Material> material)
{
	return (readObj(fileName, positionOffset, rotationDegrees, scale, material, ObjReadOptions()));
}

Mesh	readObj(
	std::string fileName,
	Vector3 positionOffset,
	Vector3 rotationDegrees,
	Vector3 scale,
	std::shared_ptr<Material> material,
	const ObjReadOptions& options
)
{
	Clock	clock;
	std::size_t skippedDegenerateTriangles = 0;
	const bool useProgress = options.progress != nullptr && options.progress->total > 0;

	if (useProgress)
	{
		beginMeshLoadProgress(*options.progress);
	}
	else if (!options.quiet)
	{
		std::cout << CLR_YELLOW << "Parsing " << CLR_BLUE << fileName << CLR_YELLOW << "..." << CLR_RESET << std::endl;
	}

	clock.start();
	const ObjMeshData data = loadObjMeshDataFromFile(fileName);
	Mesh mesh(
		Vector3(),
		material,
		buildObjTriangles(data, positionOffset, rotationDegrees, scale, material, &skippedDegenerateTriangles)
	);

	if (useProgress)
	{
		finishMeshLoadProgress(*options.progress, skippedDegenerateTriangles);
	}
	else if (!options.quiet)
	{
		if (skippedDegenerateTriangles > 0)
		{
			std::cout
				<< CLR_YELLOW << "Skipped " << CLR_WHITE << skippedDegenerateTriangles
				<< CLR_YELLOW << " degenerate OBJ triangle"
				<< (skippedDegenerateTriangles == 1 ? "" : "s")
				<< "." << CLR_RESET << std::endl;
		}
		std::cout << CLR_BLUE << fileName << CLR_GREEN_BRIGHT << " parsing done! " << CLR_BLUE_BRIGHT << "(Duration: " << CLR_WHITE << clock.elapsedS() << "s" << CLR_BLUE_BRIGHT << ")\n\n" << CLR_RESET;
	}

	return (mesh);
}

namespace
{
ObjMeshData	parseObjFile(std::ifstream& stream)
{
	std::string line;
	ObjMeshData data;
	std::size_t triangleReserveHint = 0;

	reserveObjStorage(stream, data.vertices, data.textureCoordinates, data.normals, triangleReserveHint);
	data.triangles.reserve(triangleReserveHint);

	do
	{
		getline(stream, line);

		if (line.length() <= 0)
		{
			continue;
		}

		if (line.rfind("v ", 0) == 0)
		{
			data.vertices.push_back(parseVectorValues(line.c_str() + 2, line));
		}
		else if (line.rfind("vt ", 0) == 0)
		{
			data.textureCoordinates.push_back(parseTextureValues(line.c_str() + 3, line));
		}
		else if (line.rfind("vn ", 0) == 0)
		{
			data.normals.push_back(parseVectorValues(line.c_str() + 3, line));
		}
		else if (line.rfind("f ", 0) == 0)
		{
			std::vector<ObjMeshFaceVertex> faceVertices;
			const char* cursor = line.c_str() + 2;

			faceVertices.reserve(4);
			while (true)
			{
				skipSpaces(cursor);
				if (*cursor == '\0')
				{
					break;
				}
				faceVertices.push_back(parseFaceVertex(
					cursor,
					data.vertices.size(),
					data.textureCoordinates.size(),
					data.normals.size(),
					line
				));
			}
			if (faceVertices.size() < 3)
			{
				throw std::runtime_error("OBJ face has fewer than three vertices: " + line);
			}

			for (std::size_t index = 1; index + 1 < faceVertices.size(); index++)
			{
				data.triangles.push_back({
					faceVertices.at(0),
					faceVertices.at(index),
					faceVertices.at(index + 1)
				});
			}
		}
	} while (!stream.eof());

	return (data);
}
}
