#include "RHI/GlobalIllumination.h"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string_view>
#include <vector>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	struct Brick
	{
		glm::vec3 m_min;
		glm::vec3 m_max;
		uint32_t m_level = 0;
	};

	void Require(bool value, std::string_view message)
	{
		if (!value)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	GIProbesData MakeVolume(const std::vector<Brick>& bricks)
	{
		GIProbesData data;
		data.m_stateName = "Adaptive neighbors";
		data.m_bakerVersion = std::string(GIProbesCurrentBakerVersion);
		data.m_volumeMin = glm::vec3((std::numeric_limits<float>::max)());
		data.m_volumeMax = glm::vec3((std::numeric_limits<float>::lowest)());
		data.m_bricks.Reserve(bricks.size());
		data.m_probes.Reserve(bricks.size() * 8);
		for (const auto& bounds : bricks)
		{
			GIProbeBrick brick;
			brick.m_min = bounds.m_min;
			brick.m_max = bounds.m_max;
			brick.m_subdivisionLevel = bounds.m_level;
			brick.m_firstProbeIndex = static_cast<uint32_t>(data.m_probes.Num());
			brick.m_probeCount = 8;
			brick.m_probeCounts = glm::uvec3(2);
			data.m_bricks.Add(brick);
			data.m_volumeMin = glm::min(data.m_volumeMin, bounds.m_min);
			data.m_volumeMax = glm::max(data.m_volumeMax, bounds.m_max);
			for (uint32_t corner = 0; corner < 8; ++corner)
			{
				GIProbe probe;
				probe.m_position = glm::vec3(corner & 1 ? bounds.m_max.x : bounds.m_min.x,
					corner & 2 ? bounds.m_max.y : bounds.m_min.y, corner & 4 ? bounds.m_max.z : bounds.m_min.z);
				probe.m_irradiance[0] = glm::vec3(0.5f);
				data.m_probes.Add(probe);
			}
		}
		return data;
	}

	uint32_t ReferenceMask(const std::vector<Brick>& bricks, size_t source)
	{
		const auto& a = bricks[source];
		uint32_t mask = 0;
		for (size_t index = 0; index < bricks.size(); ++index)
		{
			const auto& b = bricks[index];
			if (index == source || a.m_level == b.m_level)
			{
				continue;
			}
			float magnitude = 1.0f;
			for (uint32_t axis = 0; axis < 3; ++axis)
			{
				magnitude = (std::max)({magnitude, std::abs(a.m_min[axis]), std::abs(a.m_max[axis]),
					std::abs(b.m_min[axis]), std::abs(b.m_max[axis])});
			}
			const float tolerance = magnitude * std::numeric_limits<float>::epsilon() * 8.0f;
			for (uint32_t face = 0; face < 6; ++face)
			{
				const uint32_t axis = face / 2;
				const float aFace = face & 1 ? a.m_max[axis] : a.m_min[axis];
				const float bFace = face & 1 ? b.m_min[axis] : b.m_max[axis];
				bool overlap = std::abs(aFace - bFace) <= tolerance;
				for (uint32_t otherAxis = 0; otherAxis < 3; ++otherAxis)
				{
					if (otherAxis != axis)
					{
						overlap &= (std::min)(a.m_max[otherAxis], b.m_max[otherAxis]) -
							(std::max)(a.m_min[otherAxis], b.m_min[otherAxis]) > tolerance;
					}
				}
				if (overlap)
				{
					mask |= 1u << face;
				}
			}
		}
		return mask;
	}

	uint32_t Mask(const RHIGlobalIlluminationGpuLayout& layout, size_t brick)
	{
		return (std::bit_cast<uint32_t>(layout.m_bricks[brick].m_minAndSubdivision.w) &
			GlobalIlluminationBrickAdaptiveFaceMask) >> GlobalIlluminationBrickAdaptiveFaceShift;
	}

	RHIGlobalIlluminationGpuLayout Check(const std::vector<Brick>& bricks)
	{
		const auto data = MakeVolume(bricks);
		RHIGlobalIlluminationGpuLayout layout;
		std::string diagnostic;
		Require(BuildGlobalIlluminationGpuLayout(data, layout, diagnostic), diagnostic);
		Require(layout.m_bricks.Num() == bricks.size() && layout.m_nodes.Num() == bricks.size() * 2 - 1,
			"packing must retain every brick and its BVH leaf");
		for (size_t index = 0; index < bricks.size(); ++index)
		{
			Require(Mask(layout, index) == ReferenceMask(bricks, index),
				"adaptive face mask differs from the all-pairs reference at brick " + std::to_string(index));
		}
		return layout;
	}

	void TestContacts()
	{
		const Brick unit{glm::vec3(0), glm::vec3(1), 0};
		Require(Mask(Check({unit}), 0) == 0, "one brick must not be its own neighbor");
		for (uint32_t face = 0; face < 6; ++face)
		{
			Brick neighbor{glm::vec3(0.1f), glm::vec3(0.4f), 1};
			const uint32_t axis = face / 2;
			neighbor.m_min[axis] = face & 1 ? 1.0f : -0.3f;
			neighbor.m_max[axis] = face & 1 ? 1.3f : 0.0f;
			auto layout = Check({unit, neighbor});
			Require(Mask(layout, 0) == (1u << face) && Mask(layout, 1) == (1u << (face ^ 1u)),
				"partial face contact must mark both sides without covering the face center");
			neighbor.m_level = 0;
			layout = Check({unit, neighbor});
			Require(Mask(layout, 0) == 0 && Mask(layout, 1) == 0, "equal subdivisions need no adaptive seam");
		}
		for (const Brick other : {Brick{glm::vec3(1), glm::vec3(2), 1},
			Brick{glm::vec3(1, 1, 0), glm::vec3(2, 2, 1), 1},
			Brick{glm::vec3(0.2f), glm::vec3(0.8f), 1},
			Brick{glm::vec3(1.01f, 0, 0), glm::vec3(2, 1, 1), 1}})
		{
			const auto layout = Check({unit, other});
			Require(Mask(layout, 0) == 0 && Mask(layout, 1) == 0,
				"corner, edge, interior overlap and separated boxes must not mark faces");
		}
		const auto wideNeighbor = Check({{glm::vec3(0), glm::vec3(1000, 2000, 2000), 0},
			{glm::vec3(1010, 0, 0), glm::vec3(2010, 20000000, 1000), 1}});
		Require(Mask(wideNeighbor, 0) == 2 && Mask(wideNeighbor, 1) == 1,
			"broad-phase tolerance must include the neighbor's coordinate magnitude");
	}

	std::vector<Brick> MakeAdaptiveGrid(uint32_t side)
	{
		std::vector<Brick> bricks;
		for (uint32_t z = 0; z < side; ++z)
		{
			for (uint32_t y = 0; y < side; ++y)
			{
				for (uint32_t x = 0; x < side; ++x)
				{
					const glm::vec3 origin(x, y, z);
					if ((x + y * 2 + z * 3) % 5 != 0)
					{
						bricks.push_back({origin, origin + 1.0f, 0});
						continue;
					}
					for (uint32_t child = 0; child < 8; ++child)
					{
						const glm::vec3 childMin = origin + glm::vec3(child & 1, (child >> 1) & 1, child >> 2) * 0.5f;
						bricks.push_back({childMin, childMin + 0.5f, 1});
					}
				}
			}
		}
		return bricks;
	}

	void TestGeneratedLayouts()
	{
		const auto source = MakeAdaptiveGrid(5);
		const std::array<float, 4> scales{0.0001f, 1.0f, 64.0f, 4096.0f};
		const std::array<glm::vec3, 4> offsets{glm::vec3(0), glm::vec3(-256, 8192, -1024),
			glm::vec3(1000000, -2000000, 3000000), glm::vec3(-100000000, 100000000, -100000000)};
		std::mt19937 random(128);
		for (size_t i = 0; i < scales.size(); ++i)
		{
			auto bricks = source;
			for (auto& brick : bricks)
			{
				brick.m_min = brick.m_min * scales[i] + offsets[i];
				brick.m_max = brick.m_max * scales[i] + offsets[i];
			}
			for (uint32_t permutation = 0; permutation < 3; ++permutation)
			{
				Check(bricks);
				std::shuffle(bricks.begin(), bricks.end(), random);
			}
		}
	}

	void Benchmark()
	{
		for (const uint32_t side : {6u, 10u, 16u})
		{
			const auto data = MakeVolume(MakeAdaptiveGrid(side));
			std::array<double, 3> milliseconds;
			uint64_t checksum = 0;
			for (auto& elapsed : milliseconds)
			{
				RHIGlobalIlluminationGpuLayout layout;
				std::string diagnostic;
				const auto start = std::chrono::steady_clock::now();
				Require(BuildGlobalIlluminationGpuLayout(data, layout, diagnostic), diagnostic);
				elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
				for (size_t index = 0; index < data.m_bricks.Num(); ++index)
				{
					checksum += Mask(layout, index);
				}
			}
			std::sort(milliseconds.begin(), milliseconds.end());
			std::cout << "bricks=" << data.m_bricks.Num() << " median_ms=" << milliseconds[1]
				<< " mask_checksum=" << checksum << '\n';
		}
	}
}

int main(int argc, char** argv)
{
	try
	{
		if (argc == 2 && std::string_view(argv[1]) == "--benchmark")
		{
			Benchmark();
		}
		else
		{
			TestContacts();
			TestGeneratedLayouts();
			std::cout << "Adaptive GI layout tests passed\n";
		}
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Adaptive GI layout tests failed: " << error.what() << '\n';
		return 1;
	}
}
