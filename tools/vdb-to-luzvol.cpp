// Offline asset converter. This tool deliberately lives outside src/ and is
// never compiled or linked into Luz. It is the only place that uses OpenVDB;
// the renderer consumes the dependency-free .luzvol output.
#include "SparseVolumeGrid.hpp"
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	std::int32_t floorBrick(std::int32_t coordinate)
	{
		const std::int32_t edge = static_cast<std::int32_t>(SparseVolumeGrid::BRICK_EDGE);
		return (coordinate >= 0
			? coordinate / edge
			: -((-coordinate + edge - 1) / edge));
	}

	void usage(const char* executable)
	{
		std::cerr << "Usage: " << executable << " INPUT.vdb OUTPUT.luzvol [GRID_NAME]\n";
	}
}

int main(int argc, char** argv)
{
	try
	{
		if (argc < 3 || argc > 4)
		{
			usage(argv[0]);
			return (2);
		}
		const std::string inputName = argv[1];
		const std::string outputName = argv[2];
		const std::string gridName = argc == 4 ? argv[3] : "density";
		openvdb::initialize();
		openvdb::io::File file(inputName);
		file.open(false);
		openvdb::GridBase::Ptr baseGrid = file.readGrid(gridName);
		file.close();
		openvdb::FloatGrid::Ptr grid = openvdb::gridPtrCast<openvdb::FloatGrid>(baseGrid);
		if (!grid)
			throw std::runtime_error("Grid '" + gridName + "' is not a float density grid.");
		if (grid->empty())
			throw std::runtime_error("Density grid has no active voxels.");
		const openvdb::CoordBBox activeBounds = grid->evalActiveVoxelBoundingBox();

		const openvdb::Coord sourceMinimum = activeBounds.min();
		const openvdb::Coord sourceMaximum = activeBounds.max();
		const std::array<std::int32_t, 3> minimum = {
			floorBrick(sourceMinimum.x()) * static_cast<std::int32_t>(SparseVolumeGrid::BRICK_EDGE),
			floorBrick(sourceMinimum.y()) * static_cast<std::int32_t>(SparseVolumeGrid::BRICK_EDGE),
			floorBrick(sourceMinimum.z()) * static_cast<std::int32_t>(SparseVolumeGrid::BRICK_EDGE)
		};
		const std::array<std::int32_t, 3> maximum = {
			(floorBrick(sourceMaximum.x()) + 1) * static_cast<std::int32_t>(SparseVolumeGrid::BRICK_EDGE) - 1,
			(floorBrick(sourceMaximum.y()) + 1) * static_cast<std::int32_t>(SparseVolumeGrid::BRICK_EDGE) - 1,
			(floorBrick(sourceMaximum.z()) + 1) * static_cast<std::int32_t>(SparseVolumeGrid::BRICK_EDGE) - 1
		};
		const std::array<std::uint32_t, 3> dimensions = {
			static_cast<std::uint32_t>(maximum[0] - minimum[0] + 1),
			static_cast<std::uint32_t>(maximum[1] - minimum[1] + 1),
			static_cast<std::uint32_t>(maximum[2] - minimum[2] + 1)
		};
		const openvdb::Vec3d vdbVoxelSize = grid->voxelSize();
		const std::array<float, 3> voxelSize = {
			static_cast<float>(vdbVoxelSize.x()),
			static_cast<float>(vdbVoxelSize.y()),
			static_cast<float>(vdbVoxelSize.z())
		};
		const std::array<std::uint32_t, 3> brickDimensions = {
			dimensions[0] / SparseVolumeGrid::BRICK_EDGE,
			dimensions[1] / SparseVolumeGrid::BRICK_EDGE,
			dimensions[2] / SparseVolumeGrid::BRICK_EDGE
		};

		std::cout << "Grid: " << gridName << '\n'
			<< "Active bounds: (" << sourceMinimum.x() << ',' << sourceMinimum.y() << ',' << sourceMinimum.z()
			<< ") -> (" << sourceMaximum.x() << ',' << sourceMaximum.y() << ',' << sourceMaximum.z() << ")\n"
			<< "Aligned dimensions: " << dimensions[0] << 'x' << dimensions[1] << 'x' << dimensions[2] << '\n'
			<< "Voxel size: " << voxelSize[0] << ',' << voxelSize[1] << ',' << voxelSize[2] << '\n';

		const openvdb::FloatTree& tree = grid->tree();
		auto accessor = grid->getConstAccessor();
		std::vector<SparseVolumeGrid::SourceBrick> bricks;
		bricks.reserve(static_cast<std::size_t>(tree.leafCount()));
		for (std::uint32_t bz = 0; bz < brickDimensions[2]; bz++)
		{
			for (std::uint32_t by = 0; by < brickDimensions[1]; by++)
		{
				for (std::uint32_t bx = 0; bx < brickDimensions[0]; bx++)
				{
					const openvdb::Coord origin(
						minimum[0] + static_cast<std::int32_t>(bx * SparseVolumeGrid::BRICK_EDGE),
						minimum[1] + static_cast<std::int32_t>(by * SparseVolumeGrid::BRICK_EDGE),
						minimum[2] + static_cast<std::int32_t>(bz * SparseVolumeGrid::BRICK_EDGE)
					);
					if (tree.probeConstLeaf(origin) == nullptr && !accessor.isValueOn(origin))
						continue;
					SparseVolumeGrid::SourceBrick brick;
					brick.x = static_cast<std::int32_t>(bx);
					brick.y = static_cast<std::int32_t>(by);
					brick.z = static_cast<std::int32_t>(bz);
					float maximumDensity = 0.0f;
					for (std::uint32_t z = 0; z < SparseVolumeGrid::BRICK_EDGE; z++)
					{
						for (std::uint32_t y = 0; y < SparseVolumeGrid::BRICK_EDGE; y++)
						{
							for (std::uint32_t x = 0; x < SparseVolumeGrid::BRICK_EDGE; x++)
							{
								const openvdb::Coord coordinate(
									origin.x() + static_cast<std::int32_t>(x),
									origin.y() + static_cast<std::int32_t>(y),
									origin.z() + static_cast<std::int32_t>(z)
								);
								const float density = std::max(0.0f, accessor.getValue(coordinate));
								const std::size_t index = static_cast<std::size_t>(
									(z * SparseVolumeGrid::BRICK_EDGE + y) * SparseVolumeGrid::BRICK_EDGE + x
								);
								brick.density[index] = density;
								maximumDensity = std::max(maximumDensity, density);
							}
						}
					}
					if (maximumDensity > 0.0f)
						bricks.push_back(std::move(brick));
				}
			}
			std::cout << "\rReading sparse bricks: " << (bz + 1) << '/' << brickDimensions[2]
				<< " (" << bricks.size() << " nonempty)" << std::flush;
		}
		std::cout << "\nWriting " << bricks.size() << " bricks...\n";
		SparseVolumeGrid::write(outputName, dimensions, minimum, voxelSize, bricks);
		std::cout << "Ready: " << outputName << '\n';
		openvdb::uninitialize();
		return (0);
	}
	catch (const std::exception& exception)
	{
		std::cerr << "Conversion failed: " << exception.what() << '\n';
		return (1);
	}
}
