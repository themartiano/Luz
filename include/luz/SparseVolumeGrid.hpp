#pragma once

#include "Vector3.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class SparseGridVolume;

// Dependency-free sparse scalar grid used by authored Luz volumes.
//
// .luzvol v1 stores independent 8^3 bricks. Each brick has its own floating
// point maximum and 16-bit UNORM samples, so low-density edge wisps keep their
// precision without paying four bytes per voxel. The disk format is explicitly
// little-endian and is parsed field-by-field.
class SparseVolumeGrid
{
	public:
		static constexpr std::uint32_t BRICK_EDGE = 8;
		static constexpr std::uint32_t BRICK_SAMPLE_COUNT =
			BRICK_EDGE * BRICK_EDGE * BRICK_EDGE;

		struct SourceBrick
		{
			std::int32_t x = 0;
			std::int32_t y = 0;
			std::int32_t z = 0;
			std::array<float, BRICK_SAMPLE_COUNT> density{};
		};

		static SparseVolumeGrid load(const std::string& fileName);
		static void write(
			const std::string& fileName,
			const std::array<std::uint32_t, 3>& dimensions,
			const std::array<std::int32_t, 3>& indexMinimum,
			const std::array<float, 3>& voxelSize,
			const std::vector<SourceBrick>& bricks
		);

		float sample(const Vector3& normalizedPosition) const;
		float brickMinimum(std::int32_t x, std::int32_t y, std::int32_t z) const;
		float brickMaximum(std::int32_t x, std::int32_t y, std::int32_t z) const;
		void interpolationBrickBounds(
			std::int32_t x,
			std::int32_t y,
			std::int32_t z,
			float& minimum,
			float& maximum
		) const;
		float interpolationBrickMinimum(std::int32_t x, std::int32_t y, std::int32_t z) const;
		float interpolationBrickMaximum(std::int32_t x, std::int32_t y, std::int32_t z) const;
		const std::array<std::uint32_t, 3>& dimensions(void) const;
		const std::array<std::int32_t, 3>& indexMinimum(void) const;
		const std::array<float, 3>& voxelSize(void) const;
		float maximumDensity(void) const;
		std::size_t brickCount(void) const;

	private:
		friend class SparseGridVolume;

		struct Brick
		{
			float minimum = 0.0f;
			float maximum = 0.0f;
			std::array<std::uint16_t, BRICK_SAMPLE_COUNT> density{};
		};
		struct InterpolationBounds
		{
			float minimum = 0.0f;
			float maximum = 0.0f;
		};

		static std::uint64_t brickKey(std::int32_t x, std::int32_t y, std::int32_t z);
		std::size_t brickLookupIndex(std::int32_t x, std::int32_t y, std::int32_t z) const;
		// SparseGridVolume calls this only for points already constrained to its
		// integration interval. Public callers use sample(), which validates all
		// coordinates before entering the same interpolation implementation.
		float sampleUnchecked(const Vector3& normalizedPosition) const;
		float voxel(std::int32_t x, std::int32_t y, std::int32_t z) const;
		InterpolationBounds calculateInterpolationBounds(
			std::int32_t x,
			std::int32_t y,
			std::int32_t z
		) const;
		void buildInterpolationBounds(void);

		std::array<std::uint32_t, 3> _dimensions{0, 0, 0};
		std::array<std::int32_t, 3> _indexMinimum{0, 0, 0};
		std::array<float, 3> _voxelSize{1.0f, 1.0f, 1.0f};
		float _maximumDensity = 0.0f;
		std::vector<Brick> _bricks;
		std::array<std::uint32_t, 3> _brickDimensions{0, 0, 0};
		std::vector<std::uint32_t> _brickLookup;
		std::vector<InterpolationBounds> _interpolationBounds;
};
