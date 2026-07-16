#pragma once

#include "Sampler.hpp"
#include "Vector3.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

// Frozen spatial-directional radiance guide for participating media. Training
// uses commutative fixed-point accumulation so the frozen field is independent
// of thread scheduling; render-time sampling is read-only and lock-free.
class VolumeGuidingField
{
	public:
		struct Sample
		{
			Vector3 direction;
			double pdf = 0.0;
			bool valid = false;
		};

		VolumeGuidingField(
			const Vector3& minimum,
			const Vector3& maximum,
			std::uint32_t spatialResolution,
			std::uint32_t directionalLobes = 16,
			double lobeAnisotropy = 0.8
		);

		void record(
			const Vector3& position,
			const Vector3& incidentDirection,
			double radianceWeight
		);
		void freeze(double priorWeight = 0.05);
		Sample sample(
			const Vector3& position,
			double lobeSample,
			const Sampler::Sample2D& directionSample
		) const;
		double pdf(const Vector3& position, const Vector3& direction) const;

		bool isFrozen(void) const;
		bool contains(const Vector3& position) const;
		std::size_t cellCount(void) const;
		std::size_t memoryBytes(void) const;

	private:
		std::size_t spatialIndex(const Vector3& position) const;
		std::size_t nearestLobe(const Vector3& direction) const;
		double lobePDF(const Vector3& center, const Vector3& direction) const;
		Vector3 sampleLobe(
			const Vector3& center,
			const Sampler::Sample2D& sample
		) const;

		Vector3 _minimum;
		Vector3 _maximum;
		Vector3 _inverseExtent;
		std::uint32_t _spatialResolution = 0;
		std::uint32_t _directionalLobes = 0;
		double _lobeAnisotropy = 0.0;
		std::size_t _cellCount = 0;
		std::unique_ptr<std::atomic<std::uint64_t>[]> _trainingWeights;
		std::vector<Vector3> _lobeCenters;
		std::vector<double> _probabilities;
		std::atomic<bool> _frozen = false;
};
