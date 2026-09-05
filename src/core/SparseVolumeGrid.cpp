#include "SparseVolumeGrid.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace
{
	constexpr std::array<char, 8> MAGIC = {'L', 'U', 'Z', 'V', 'O', 'L', '1', '\0'};
	constexpr std::uint32_t VERSION = 1;
	constexpr std::uint32_t HEADER_BYTES = 96;
	constexpr std::uint32_t ENCODING_BRICK_UNORM16 = 1;
	constexpr std::uint64_t BRICK_RECORD_BYTES = 16
		+ SparseVolumeGrid::BRICK_SAMPLE_COUNT * sizeof(std::uint16_t);
	constexpr std::uint64_t COORDINATE_LIMIT = 1u << 21u;
	constexpr std::uint32_t EMPTY_BRICK = std::numeric_limits<std::uint32_t>::max();

	void requireRead(std::istream& input, char* destination, std::streamsize bytes, const std::string& fileName)
	{
		input.read(destination, bytes);
		if (!input)
			throw std::runtime_error("Truncated Luz volume file: " + fileName);
	}

	std::uint16_t readU16(std::istream& input, const std::string& fileName)
	{
		std::array<unsigned char, 2> bytes{};
		requireRead(input, reinterpret_cast<char*>(bytes.data()), 2, fileName);
		return (static_cast<std::uint16_t>(bytes[0])
			| static_cast<std::uint16_t>(bytes[1]) << 8u);
	}

	std::uint32_t readU32(std::istream& input, const std::string& fileName)
	{
		std::array<unsigned char, 4> bytes{};
		requireRead(input, reinterpret_cast<char*>(bytes.data()), 4, fileName);
		return (static_cast<std::uint32_t>(bytes[0])
			| static_cast<std::uint32_t>(bytes[1]) << 8u
			| static_cast<std::uint32_t>(bytes[2]) << 16u
			| static_cast<std::uint32_t>(bytes[3]) << 24u);
	}

	std::uint64_t readU64Exact(std::istream& input, const std::string& fileName)
	{
		std::array<unsigned char, 8> bytes{};
		requireRead(input, reinterpret_cast<char*>(bytes.data()), 8, fileName);
		std::uint64_t value = 0;
		for (unsigned int byte = 0; byte < 8; byte++)
			value |= static_cast<std::uint64_t>(bytes[byte]) << (byte * 8u);
		return (value);
	}

	std::int32_t readI32(std::istream& input, const std::string& fileName)
	{
		return (std::bit_cast<std::int32_t>(readU32(input, fileName)));
	}

	float readF32(std::istream& input, const std::string& fileName)
	{
		return (std::bit_cast<float>(readU32(input, fileName)));
	}

	void writeU16(std::ostream& output, std::uint16_t value)
	{
		const std::array<unsigned char, 2> bytes = {
			static_cast<unsigned char>(value & 0xffu),
			static_cast<unsigned char>((value >> 8u) & 0xffu)
		};
		output.write(reinterpret_cast<const char*>(bytes.data()), 2);
	}

	void writeU32(std::ostream& output, std::uint32_t value)
	{
		const std::array<unsigned char, 4> bytes = {
			static_cast<unsigned char>(value & 0xffu),
			static_cast<unsigned char>((value >> 8u) & 0xffu),
			static_cast<unsigned char>((value >> 16u) & 0xffu),
			static_cast<unsigned char>((value >> 24u) & 0xffu)
		};
		output.write(reinterpret_cast<const char*>(bytes.data()), 4);
	}

	void writeU64(std::ostream& output, std::uint64_t value)
	{
		for (unsigned int byte = 0; byte < 8; byte++)
			output.put(static_cast<char>((value >> (byte * 8u)) & 0xffu));
	}

	void writeI32(std::ostream& output, std::int32_t value)
	{
		writeU32(output, std::bit_cast<std::uint32_t>(value));
	}

	void writeF32(std::ostream& output, float value)
	{
		writeU32(output, std::bit_cast<std::uint32_t>(value));
	}

	void requireFiniteNonnegative(float value, const std::string& label)
	{
		if (!std::isfinite(value) || value < 0.0f)
			throw std::invalid_argument(label + " must be finite and nonnegative.");
	}

	void requireValidMetadata(
		const std::array<std::uint32_t, 3>& dimensions,
		const std::array<float, 3>& voxelSize
	)
	{
		for (int axis = 0; axis < 3; axis++)
		{
			if (dimensions[axis] == 0)
				throw std::invalid_argument("Luz volume dimensions must be positive.");
			if (!std::isfinite(voxelSize[axis]) || voxelSize[axis] <= 0.0f)
				throw std::invalid_argument("Luz volume voxel size must be finite and positive.");
		}
	}

	std::uint32_t brickDimension(std::uint32_t dimension)
	{
		return ((dimension + SparseVolumeGrid::BRICK_EDGE - 1u)
			/ SparseVolumeGrid::BRICK_EDGE);
	}
}

std::uint64_t SparseVolumeGrid::brickKey(std::int32_t x, std::int32_t y, std::int32_t z)
{
	if (x < 0 || y < 0 || z < 0
		|| static_cast<std::uint64_t>(x) >= COORDINATE_LIMIT
		|| static_cast<std::uint64_t>(y) >= COORDINATE_LIMIT
		|| static_cast<std::uint64_t>(z) >= COORDINATE_LIMIT)
		throw std::out_of_range("Luz volume brick coordinate is outside the supported range.");
	return (static_cast<std::uint64_t>(x)
		| static_cast<std::uint64_t>(y) << 21u
		| static_cast<std::uint64_t>(z) << 42u);
}

SparseVolumeGrid SparseVolumeGrid::load(const std::string& fileName)
{
	std::ifstream input(fileName, std::ios::binary);
	if (!input)
		throw std::runtime_error("Could not open Luz volume file: " + fileName);
	std::array<char, 8> magic{};
	requireRead(input, magic.data(), static_cast<std::streamsize>(magic.size()), fileName);
	if (magic != MAGIC)
		throw std::runtime_error("Invalid Luz volume signature: " + fileName);
	const std::uint32_t version = readU32(input, fileName);
	const std::uint32_t headerBytes = readU32(input, fileName);
	const std::uint32_t brickEdge = readU32(input, fileName);
	const std::uint32_t encoding = readU32(input, fileName);
	if (version != VERSION || headerBytes != HEADER_BYTES
		|| brickEdge != BRICK_EDGE || encoding != ENCODING_BRICK_UNORM16)
		throw std::runtime_error("Unsupported Luz volume format variant: " + fileName);

	SparseVolumeGrid grid;
	for (int axis = 0; axis < 3; axis++) grid._dimensions[axis] = readU32(input, fileName);
	for (int axis = 0; axis < 3; axis++) grid._indexMinimum[axis] = readI32(input, fileName);
	for (int axis = 0; axis < 3; axis++) grid._voxelSize[axis] = readF32(input, fileName);
	grid._maximumDensity = readF32(input, fileName);
	const std::uint64_t brickCount = readU64Exact(input, fileName);
	requireValidMetadata(grid._dimensions, grid._voxelSize);
	requireFiniteNonnegative(grid._maximumDensity, "Luz volume maximum density");
	if (brickCount > std::numeric_limits<std::uint32_t>::max())
		throw std::runtime_error("Luz volume contains too many bricks: " + fileName);

	std::array<char, HEADER_BYTES - 72> reserved{};
	requireRead(input, reserved.data(), static_cast<std::streamsize>(reserved.size()), fileName);
	const std::uint64_t fileBytes = std::filesystem::file_size(fileName);
	if (brickCount > (std::numeric_limits<std::uint64_t>::max() - HEADER_BYTES) / BRICK_RECORD_BYTES
		|| fileBytes != HEADER_BYTES + brickCount * BRICK_RECORD_BYTES)
		throw std::runtime_error("Luz volume file size does not match its header: " + fileName);

	grid._bricks.reserve(static_cast<std::size_t>(brickCount));
	std::uint64_t lookupCount = 1;
	for (int axis = 0; axis < 3; axis++)
	{
		grid._brickDimensions[axis] = brickDimension(grid._dimensions[axis]);
		if (lookupCount > std::numeric_limits<std::size_t>::max() / grid._brickDimensions[axis])
			throw std::runtime_error("Luz volume brick lookup is too large: " + fileName);
		lookupCount *= grid._brickDimensions[axis];
	}
	grid._brickLookup.assign(static_cast<std::size_t>(lookupCount), EMPTY_BRICK);
	for (std::uint64_t index = 0; index < brickCount; index++)
	{
		const std::int32_t x = readI32(input, fileName);
		const std::int32_t y = readI32(input, fileName);
		const std::int32_t z = readI32(input, fileName);
		if (x < 0 || y < 0 || z < 0
			|| static_cast<std::uint32_t>(x) >= brickDimension(grid._dimensions[0])
			|| static_cast<std::uint32_t>(y) >= brickDimension(grid._dimensions[1])
			|| static_cast<std::uint32_t>(z) >= brickDimension(grid._dimensions[2]))
			throw std::runtime_error("Luz volume contains an out-of-bounds brick: " + fileName);
		Brick brick;
		brick.maximum = readF32(input, fileName);
		requireFiniteNonnegative(brick.maximum, "Luz volume brick maximum");
		for (std::uint16_t& density : brick.density)
			density = readU16(input, fileName);
		const std::uint16_t quantizedMinimum = *std::min_element(
			brick.density.begin(),
			brick.density.end()
		);
		brick.minimum = brick.maximum
			* (static_cast<float>(quantizedMinimum) / 65535.0f);
		const std::size_t lookupIndex = grid.brickLookupIndex(x, y, z);
		if (grid._brickLookup[lookupIndex] != EMPTY_BRICK)
			throw std::runtime_error("Luz volume contains duplicate bricks: " + fileName);
		grid._brickLookup[lookupIndex] = static_cast<std::uint32_t>(grid._bricks.size());
		grid._bricks.push_back(std::move(brick));
	}
	grid.buildInterpolationBounds();
	return (grid);
}

void SparseVolumeGrid::write(
	const std::string& fileName,
	const std::array<std::uint32_t, 3>& dimensions,
	const std::array<std::int32_t, 3>& indexMinimum,
	const std::array<float, 3>& voxelSize,
	const std::vector<SourceBrick>& bricks
)
{
	requireValidMetadata(dimensions, voxelSize);
	std::vector<const SourceBrick*> nonempty;
	std::vector<float> maxima;
	nonempty.reserve(bricks.size());
	maxima.reserve(bricks.size());
	std::unordered_set<std::uint64_t> keys;
	keys.reserve(bricks.size());
	float globalMaximum = 0.0f;
	for (const SourceBrick& brick : bricks)
	{
		if (brick.x < 0 || brick.y < 0 || brick.z < 0
			|| static_cast<std::uint32_t>(brick.x) >= brickDimension(dimensions[0])
			|| static_cast<std::uint32_t>(brick.y) >= brickDimension(dimensions[1])
			|| static_cast<std::uint32_t>(brick.z) >= brickDimension(dimensions[2]))
			throw std::invalid_argument("Luz volume source brick is outside the grid.");
		const std::uint64_t key = brickKey(brick.x, brick.y, brick.z);
		if (!keys.insert(key).second)
			throw std::invalid_argument("Luz volume source contains duplicate bricks.");
		float maximum = 0.0f;
		for (float density : brick.density)
		{
			requireFiniteNonnegative(density, "Luz volume density");
			maximum = std::max(maximum, density);
		}
		if (maximum <= 0.0f)
			continue;
		nonempty.push_back(&brick);
		maxima.push_back(maximum);
		globalMaximum = std::max(globalMaximum, maximum);
	}

	std::ofstream output(fileName, std::ios::binary | std::ios::trunc);
	if (!output)
		throw std::runtime_error("Could not create Luz volume file: " + fileName);
	output.write(MAGIC.data(), static_cast<std::streamsize>(MAGIC.size()));
	writeU32(output, VERSION);
	writeU32(output, HEADER_BYTES);
	writeU32(output, BRICK_EDGE);
	writeU32(output, ENCODING_BRICK_UNORM16);
	for (std::uint32_t dimension : dimensions) writeU32(output, dimension);
	for (std::int32_t minimum : indexMinimum) writeI32(output, minimum);
	for (float size : voxelSize) writeF32(output, size);
	writeF32(output, globalMaximum);
	writeU64(output, static_cast<std::uint64_t>(nonempty.size()));
	for (std::uint32_t byte = 72; byte < HEADER_BYTES; byte++) output.put('\0');
	for (std::size_t index = 0; index < nonempty.size(); index++)
	{
		const SourceBrick& brick = *nonempty[index];
		const float maximum = maxima[index];
		writeI32(output, brick.x);
		writeI32(output, brick.y);
		writeI32(output, brick.z);
		writeF32(output, maximum);
		for (float density : brick.density)
		{
			const double normalized = std::clamp(
				static_cast<double>(density) / static_cast<double>(maximum),
				0.0,
				1.0
			);
			writeU16(output, static_cast<std::uint16_t>(std::lround(normalized * 65535.0)));
		}
	}
	if (!output)
		throw std::runtime_error("Failed while writing Luz volume file: " + fileName);
}

float SparseVolumeGrid::voxel(std::int32_t x, std::int32_t y, std::int32_t z) const
{
	if (x < 0 || y < 0 || z < 0
		|| static_cast<std::uint32_t>(x) >= this->_dimensions[0]
		|| static_cast<std::uint32_t>(y) >= this->_dimensions[1]
		|| static_cast<std::uint32_t>(z) >= this->_dimensions[2])
		return (0.0f);
	const std::int32_t brickX = x / static_cast<std::int32_t>(BRICK_EDGE);
	const std::int32_t brickY = y / static_cast<std::int32_t>(BRICK_EDGE);
	const std::int32_t brickZ = z / static_cast<std::int32_t>(BRICK_EDGE);
	const std::uint32_t brickIndex = this->_brickLookup[this->brickLookupIndex(brickX, brickY, brickZ)];
	if (brickIndex == EMPTY_BRICK)
		return (0.0f);
	const std::uint32_t localX = static_cast<std::uint32_t>(x) % BRICK_EDGE;
	const std::uint32_t localY = static_cast<std::uint32_t>(y) % BRICK_EDGE;
	const std::uint32_t localZ = static_cast<std::uint32_t>(z) % BRICK_EDGE;
	const Brick& brick = this->_bricks[brickIndex];
	const std::size_t sampleIndex = static_cast<std::size_t>(
		(localZ * BRICK_EDGE + localY) * BRICK_EDGE + localX
	);
	return (brick.maximum * (static_cast<float>(brick.density[sampleIndex]) / 65535.0f));
}

float SparseVolumeGrid::sample(const Vector3& normalizedPosition) const
{
	for (int axis = 0; axis < 3; axis++)
	{
		if (!std::isfinite(normalizedPosition[axis])
			|| normalizedPosition[axis] < 0.0 || normalizedPosition[axis] > 1.0)
			return (0.0f);
	}
	return (this->sampleUnchecked(normalizedPosition));
}

float SparseVolumeGrid::sampleUnchecked(const Vector3& normalizedPosition) const
{
	const double px = normalizedPosition.getX() * static_cast<double>(this->_dimensions[0]) - 0.5;
	const double py = normalizedPosition.getY() * static_cast<double>(this->_dimensions[1]) - 0.5;
	const double pz = normalizedPosition.getZ() * static_cast<double>(this->_dimensions[2]) - 0.5;
	const std::int32_t x = static_cast<std::int32_t>(std::floor(px));
	const std::int32_t y = static_cast<std::int32_t>(std::floor(py));
	const std::int32_t z = static_cast<std::int32_t>(std::floor(pz));
	const double fx = px - static_cast<double>(x);
	const double fy = py - static_cast<double>(y);
	const double fz = pz - static_cast<double>(z);
	auto lerp = [](double a, double b, double t) { return (a + (b - a) * t); };
	double v000;
	double v100;
	double v010;
	double v110;
	double v001;
	double v101;
	double v011;
	double v111;
	const bool interior = x >= 0 && y >= 0 && z >= 0
		&& static_cast<std::uint32_t>(x + 1) < this->_dimensions[0]
		&& static_cast<std::uint32_t>(y + 1) < this->_dimensions[1]
		&& static_cast<std::uint32_t>(z + 1) < this->_dimensions[2];
	const bool sameBrick = interior
		&& x / static_cast<std::int32_t>(BRICK_EDGE)
			== (x + 1) / static_cast<std::int32_t>(BRICK_EDGE)
		&& y / static_cast<std::int32_t>(BRICK_EDGE)
			== (y + 1) / static_cast<std::int32_t>(BRICK_EDGE)
		&& z / static_cast<std::int32_t>(BRICK_EDGE)
			== (z + 1) / static_cast<std::int32_t>(BRICK_EDGE);
	if (sameBrick)
	{
		const std::int32_t brickX = x / static_cast<std::int32_t>(BRICK_EDGE);
		const std::int32_t brickY = y / static_cast<std::int32_t>(BRICK_EDGE);
		const std::int32_t brickZ = z / static_cast<std::int32_t>(BRICK_EDGE);
		const std::uint32_t brickIndex = this->_brickLookup[
			this->brickLookupIndex(brickX, brickY, brickZ)
		];
		if (brickIndex == EMPTY_BRICK)
			return (0.0f);
		const Brick& brick = this->_bricks[brickIndex];
		const std::uint32_t localX = static_cast<std::uint32_t>(x) % BRICK_EDGE;
		const std::uint32_t localY = static_cast<std::uint32_t>(y) % BRICK_EDGE;
		const std::uint32_t localZ = static_cast<std::uint32_t>(z) % BRICK_EDGE;
		auto samplePair = [&brick](std::uint32_t sx, std::uint32_t sy, std::uint32_t sz)
		{
			const std::size_t index = static_cast<std::size_t>(
				(sz * BRICK_EDGE + sy) * BRICK_EDGE + sx
			);
			std::array<std::uint16_t, 2> density;
			// X-adjacent corners are contiguous. memcpy expresses an unaligned,
			// alias-safe four-byte load; optimized builds lower the four rows to
			// four native loads while preserving endian-correct uint16_t values.
			std::memcpy(
				density.data(),
				brick.density.data() + index,
				sizeof(density)
			);
			return (std::array<double, 2>{
				brick.maximum * (static_cast<float>(density[0]) / 65535.0f),
				brick.maximum * (static_cast<float>(density[1]) / 65535.0f)
			});
		};
		const std::array<double, 2> lowLow = samplePair(localX, localY, localZ);
		const std::array<double, 2> lowHigh = samplePair(localX, localY + 1u, localZ);
		const std::array<double, 2> highLow = samplePair(localX, localY, localZ + 1u);
		const std::array<double, 2> highHigh = samplePair(localX, localY + 1u, localZ + 1u);
		v000 = lowLow[0];
		v100 = lowLow[1];
		v010 = lowHigh[0];
		v110 = lowHigh[1];
		v001 = highLow[0];
		v101 = highLow[1];
		v011 = highHigh[0];
		v111 = highHigh[1];
	}
	else
	{
		v000 = this->voxel(x, y, z);
		v100 = this->voxel(x + 1, y, z);
		v010 = this->voxel(x, y + 1, z);
		v110 = this->voxel(x + 1, y + 1, z);
		v001 = this->voxel(x, y, z + 1);
		v101 = this->voxel(x + 1, y, z + 1);
		v011 = this->voxel(x, y + 1, z + 1);
		v111 = this->voxel(x + 1, y + 1, z + 1);
	}
	const double c00 = lerp(v000, v100, fx);
	const double c10 = lerp(v010, v110, fx);
	const double c01 = lerp(v001, v101, fx);
	const double c11 = lerp(v011, v111, fx);
	return (static_cast<float>(lerp(lerp(c00, c10, fy), lerp(c01, c11, fy), fz)));
}

float SparseVolumeGrid::brickMaximum(std::int32_t x, std::int32_t y, std::int32_t z) const
{
	if (x < 0 || y < 0 || z < 0
		|| static_cast<std::uint32_t>(x) >= this->_brickDimensions[0]
		|| static_cast<std::uint32_t>(y) >= this->_brickDimensions[1]
		|| static_cast<std::uint32_t>(z) >= this->_brickDimensions[2])
		return (0.0f);
	const std::uint32_t brickIndex = this->_brickLookup[this->brickLookupIndex(x, y, z)];
	if (brickIndex == EMPTY_BRICK)
		return (0.0f);
	return (this->_bricks[brickIndex].maximum);
}

float SparseVolumeGrid::brickMinimum(std::int32_t x, std::int32_t y, std::int32_t z) const
{
	if (x < 0 || y < 0 || z < 0
		|| static_cast<std::uint32_t>(x) >= this->_brickDimensions[0]
		|| static_cast<std::uint32_t>(y) >= this->_brickDimensions[1]
		|| static_cast<std::uint32_t>(z) >= this->_brickDimensions[2])
		return (0.0f);
	const std::uint32_t brickIndex = this->_brickLookup[this->brickLookupIndex(x, y, z)];
	if (brickIndex == EMPTY_BRICK)
		return (0.0f);
	return (this->_bricks[brickIndex].minimum);
}

std::size_t SparseVolumeGrid::brickLookupIndex(
	std::int32_t x,
	std::int32_t y,
	std::int32_t z
) const
{
	return (static_cast<std::size_t>(
		(static_cast<std::uint64_t>(z) * this->_brickDimensions[1]
			+ static_cast<std::uint32_t>(y)) * this->_brickDimensions[0]
			+ static_cast<std::uint32_t>(x)
	));
}

SparseVolumeGrid::InterpolationBounds SparseVolumeGrid::calculateInterpolationBounds(
	std::int32_t x,
	std::int32_t y,
	std::int32_t z
) const
{
	InterpolationBounds bounds;
	bounds.minimum = std::numeric_limits<float>::infinity();
	// Voxel centers are offset by half a voxel from the normalized brick
	// boundaries. A trilinear footprint can therefore touch either adjacent
	// brick at a boundary; include the full one-brick halo conservatively.
	for (std::int32_t dz = -1; dz <= 1; dz++)
		for (std::int32_t dy = -1; dy <= 1; dy++)
			for (std::int32_t dx = -1; dx <= 1; dx++)
			{
				bounds.minimum = std::min(
					bounds.minimum,
					this->brickMinimum(x + dx, y + dy, z + dz)
				);
				bounds.maximum = std::max(
					bounds.maximum,
					this->brickMaximum(x + dx, y + dy, z + dz)
				);
			}
	if (!std::isfinite(bounds.minimum))
		bounds.minimum = 0.0f;
	return (bounds);
}

void SparseVolumeGrid::buildInterpolationBounds(void)
{
	this->_interpolationBounds.resize(this->_brickLookup.size());
	for (std::uint32_t z = 0; z < this->_brickDimensions[2]; z++)
	{
		for (std::uint32_t y = 0; y < this->_brickDimensions[1]; y++)
		{
			for (std::uint32_t x = 0; x < this->_brickDimensions[0]; x++)
			{
				this->_interpolationBounds[this->brickLookupIndex(x, y, z)]
					= this->calculateInterpolationBounds(x, y, z);
			}
		}
	}
}

float SparseVolumeGrid::interpolationBrickMaximum(std::int32_t x, std::int32_t y, std::int32_t z) const
{
	float minimum;
	float maximum;
	this->interpolationBrickBounds(x, y, z, minimum, maximum);
	return (maximum);
}

void SparseVolumeGrid::interpolationBrickBounds(
	std::int32_t x,
	std::int32_t y,
	std::int32_t z,
	float& minimum,
	float& maximum
) const
{
	if (x >= 0 && y >= 0 && z >= 0
		&& static_cast<std::uint32_t>(x) < this->_brickDimensions[0]
		&& static_cast<std::uint32_t>(y) < this->_brickDimensions[1]
		&& static_cast<std::uint32_t>(z) < this->_brickDimensions[2]
		&& this->_interpolationBounds.size() == this->_brickLookup.size())
	{
		const InterpolationBounds& bounds = this->_interpolationBounds[
			this->brickLookupIndex(x, y, z)
		];
		minimum = bounds.minimum;
		maximum = bounds.maximum;
		return;
	}
	const InterpolationBounds bounds = this->calculateInterpolationBounds(x, y, z);
	minimum = bounds.minimum;
	maximum = bounds.maximum;
}

float SparseVolumeGrid::interpolationBrickMinimum(std::int32_t x, std::int32_t y, std::int32_t z) const
{
	float minimum;
	float maximum;
	this->interpolationBrickBounds(x, y, z, minimum, maximum);
	return (minimum);
}

const std::array<std::uint32_t, 3>& SparseVolumeGrid::dimensions(void) const
{
	return (this->_dimensions);
}

const std::array<std::int32_t, 3>& SparseVolumeGrid::indexMinimum(void) const
{
	return (this->_indexMinimum);
}

const std::array<float, 3>& SparseVolumeGrid::voxelSize(void) const
{
	return (this->_voxelSize);
}

float SparseVolumeGrid::maximumDensity(void) const
{
	return (this->_maximumDensity);
}

std::size_t SparseVolumeGrid::brickCount(void) const
{
	return (this->_bricks.size());
}
