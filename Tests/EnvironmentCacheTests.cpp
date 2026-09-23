#include "FrameGraph/EnvironmentNode.h"
#include "RHI/Cubemap.h"
#include "RHI/Shader.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace Sailor;
using namespace Sailor::Framegraph;
using namespace Sailor::RHI;

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	struct ResourceCounts
	{
		uint32_t m_created = 0u;
		uint32_t m_destroyed = 0u;

		uint32_t Live() const { return m_created - m_destroyed; }
	};

	class CountedCubemap final : public RHICubemap
	{
	public:
		CountedCubemap(ResourceCounts& counts, uint32_t state, uint32_t channel) :
			RHICubemap(ETextureFiltration::Linear, ETextureClamping::Clamp, false),
			m_state(state), m_channel(channel), m_counts(counts)
		{
			++m_counts.m_created;
		}

		~CountedCubemap() override
		{
			++m_counts.m_destroyed;
		}

		uint32_t m_state;
		uint32_t m_channel;

	private:
		ResourceCounts& m_counts;
	};

	class EnvironmentNodeProbe final : public EnvironmentNode
	{
	public:
		using EnvironmentNode::EnvironmentMaps;
		using EnvironmentNode::CacheEnvironment;
		using EnvironmentNode::TryRestoreEnvironment;

		uint32_t NumCachedEnvironments() const { return m_numCachedEnvironments; }

		void SeedLocalReflection(RHITexturePtr upload)
		{
			LocalReflectionImage image;
			image.m_extent = glm::uvec2(4u, 2u);
			image.m_pixels.Resize(8u);
			for (glm::vec4& pixel : image.m_pixels)
			{
				pixel = glm::vec4(1.0f);
			}
			image.m_samplesPerPixel = 16u;
			image.m_parameters.m_positionBlend = glm::vec4(0, 0, 0, 1);
			image.m_parameters.m_minEnabled = glm::vec4(-1, -1, -1, 1);
			image.m_parameters.m_max = glm::vec4(1);
			m_localParameters = image.m_parameters;
			m_localReflection = TSharedPtr<const LocalReflectionImage>::Make(std::move(image));
			m_localUploadTexture = std::move(upload);
			m_localReflectionSamples.store(16u);
			m_localReflectionReady.store(true);
			m_bLocalReflectionDirty = false;
		}

		bool IsLocalResetPending() const
		{
			return m_bLocalReflectionDirty && !m_localReflection && !m_localUploadTexture;
		}

		void SeedEnvironmentSky(const SkyParameters& parameters)
		{
			m_environmentSkyParams = parameters;
			m_environmentUsesSky = true;
		}
	};

	EnvironmentNodeProbe::EnvironmentMaps MakeMaps(ResourceCounts& counts, uint32_t state)
	{
		return {
			TRefPtr<CountedCubemap>::Make(counts, state, 0u),
			TRefPtr<CountedCubemap>::Make(counts, state, 1u),
			TRefPtr<CountedCubemap>::Make(counts, state, 2u)
		};
	}

	SkyEnvironmentKey MakeKey(uint32_t state)
	{
		SkyParameters sky;
		sky.m_sunIlluminance = glm::vec4(100.0f + static_cast<float>(state));
		sky.m_groundRadiance = glm::vec4(0.01f * static_cast<float>(state));
		return sky.GetEnvironmentKey();
	}

	constexpr const char* MapNames[] = { "g_envCubemap", "g_irradianceCubemap", "g_sheenEnvCubemap" };

	void RequireFrameGraphMaps(RHIFrameGraphPtr frameGraph, uint32_t state)
	{
		for (uint32_t channel = 0u; channel < 3u; ++channel)
		{
			const auto cube = frameGraph->GetSampler(MapNames[channel]).DynamicCast<CountedCubemap>();
			Require(cube && cube->m_state == state && cube->m_channel == channel,
				"a cache hit must publish one complete GGX/irradiance/sheen bundle");
		}
	}

	void TestCacheRemainsBoundedAcrossLightingStates()
	{
		ResourceCounts filteredCounts;
		ResourceCounts rawCounts;
		{
			EnvironmentNodeProbe node;
			auto frameGraph = RHIFrameGraphPtr::Make();
			auto raw = TRefPtr<CountedCubemap>::Make(rawCounts, 0u, 0u);
			for (uint32_t state = 0u; state < 500u; ++state)
			{
				const SkyEnvironmentKey key = MakeKey(state);
				Require(!node.TryRestoreEnvironment(frameGraph, key, raw),
					"a new lighting state must miss without inserting a partial bundle");
				Require(node.NumCachedEnvironments() == (std::min)(state, 4u),
					"cache misses must not consume cache capacity");
				node.CacheEnvironment(key, MakeMaps(filteredCounts, state));
				const uint32_t expectedCount = (std::min)(state + 1u, 4u);
				Require(node.NumCachedEnvironments() == expectedCount &&
					filteredCounts.Live() == 3u * expectedCount,
					"500 distinct lighting keys must retain at most four filtered bundles");
				Require(node.TryRestoreEnvironment(frameGraph, key, raw),
					"the newly filtered environment must be reusable");
				RequireFrameGraphMaps(frameGraph, state);
			}
			Require(filteredCounts.m_created == 1500u && filteredCounts.m_destroyed == 1488u,
				"evicted bundles must release their three unreferenced resources");
		}
		Require(filteredCounts.Live() == 0u && rawCounts.Live() == 0u,
			"node and frame-graph destruction must release the remaining resources");
	}

	void TestCacheHitsRefreshLruAndRawSource()
	{
		ResourceCounts filteredCounts;
		ResourceCounts rawCounts;
		EnvironmentNodeProbe node;
		auto frameGraph = RHIFrameGraphPtr::Make();
		auto firstRaw = TRefPtr<CountedCubemap>::Make(rawCounts, 0u, 0u);
		auto currentRaw = TRefPtr<CountedCubemap>::Make(rawCounts, 1u, 0u);
		for (uint32_t state = 1u; state <= 4u; ++state)
		{
			node.CacheEnvironment(MakeKey(state), MakeMaps(filteredCounts, state));
		}
		Require(node.TryRestoreEnvironment(frameGraph, MakeKey(1u), firstRaw),
			"the oldest cached key must be reusable");
		const RHITexturePtr firstSpecular = frameGraph->GetSampler("g_envCubemap");
		Require(node.TryRestoreEnvironment(frameGraph, MakeKey(1u), currentRaw) &&
			frameGraph->GetSampler("g_envCubemap") == firstSpecular && filteredCounts.m_created == 12u,
			"repeated cache hits must reuse the exact resources without filtering again");
		Require(frameGraph->GetSampler("g_rawEnvCubemap").GetRawPtr() == currentRaw.GetRawPtr(),
			"a cache hit must publish the current ready raw sky, not a cached raw source");

		node.CacheEnvironment(MakeKey(5u), MakeMaps(filteredCounts, 5u));
		Require(!node.TryRestoreEnvironment(frameGraph, MakeKey(2u), firstRaw),
			"inserting a fifth key must evict the least recently used key, not the oldest hit");
		RequireFrameGraphMaps(frameGraph, 1u);
		Require(frameGraph->GetSampler("g_rawEnvCubemap").GetRawPtr() == currentRaw.GetRawPtr(),
			"a cache miss must not disturb the currently published raw source");
		for (uint32_t state : { 1u, 3u, 4u, 5u })
		{
			Require(node.TryRestoreEnvironment(frameGraph, MakeKey(state), currentRaw),
				"all four non-evicted bundles must remain available");
			RequireFrameGraphMaps(frameGraph, state);
		}
	}

	void TestConsumersRetainEvictedMaps()
	{
		ResourceCounts counts;
		RHIShaderBinding retainedBinding;
		RHICubemapPtr previousFogEnvironment;
		{
			EnvironmentNodeProbe node;
			auto frameGraph = RHIFrameGraphPtr::Make();
			node.CacheEnvironment(MakeKey(1u), MakeMaps(counts, 1u));
			Require(node.TryRestoreEnvironment(frameGraph, MakeKey(1u), {}), "the first bundle must be available");
			for (uint32_t channel = 0u; channel < 3u; ++channel)
			{
				retainedBinding.SetTextureBinding(channel, frameGraph->GetSampler(MapNames[channel]));
			}
			previousFogEnvironment = frameGraph->GetSampler("g_irradianceCubemap").DynamicCast<RHICubemap>();
			for (uint32_t state = 2u; state <= 5u; ++state)
			{
				node.CacheEnvironment(MakeKey(state), MakeMaps(counts, state));
			}
			Require(!node.TryRestoreEnvironment(frameGraph, MakeKey(1u), {}) && counts.Live() == 15u,
				"eviction must drop cache ownership without destroying maps held by consumers");
			frameGraph->Clear();
			Require(counts.Live() == 15u, "shader bindings must retain the evicted bundle after frame-graph reset");
		}
		Require(counts.Live() == 3u, "consumer references must outlive the Environment node");
		for (uint32_t channel = 0u; channel < 3u; ++channel)
		{
			const auto cube = retainedBinding.GetTextureBinding(channel).DynamicCast<CountedCubemap>();
			Require(cube && cube->m_state == 1u && cube->m_channel == channel,
				"retained bindings must still refer to the original complete bundle");
		}
		retainedBinding.SetTextureBindings({});
		Require(counts.Live() == 1u, "the previous fog irradiance must remain alive independently of other maps");
		previousFogEnvironment.Clear();
		Require(counts.Live() == 0u, "the final consumer release must destroy the last evicted map");
	}

	void TestLocalReflectionResetKeepsEnvironmentCache()
	{
		ResourceCounts filteredCounts;
		ResourceCounts uploadCounts;
		EnvironmentNodeProbe node;
		auto frameGraph = RHIFrameGraphPtr::Make();
		const SkyParameters sky;
		node.CacheEnvironment(sky.GetEnvironmentKey(), MakeMaps(filteredCounts, 1u));
		node.SeedEnvironmentSky(sky);
		node.SeedLocalReflection(TRefPtr<CountedCubemap>::Make(uploadCounts, 0u, 0u));
		Require(node.IsLocalReflectionReady() && node.GetLocalReflectionSamples() == 16u,
			"the reset fixture must contain a published local reflection");
		node.Clear();
		Require(!node.IsLocalReflectionReady() && node.GetLocalReflectionSamples() == 0u &&
			node.GetLocalReflectionParameters().m_minEnabled.w == 0.0f && node.IsLocalResetPending() &&
			uploadCounts.Live() == 0u,
			"Clear must reset local reflection status, parameters and upload ownership");
		SkyParameters retainedSky;
		Require(node.NumCachedEnvironments() == 1u && filteredCounts.Live() == 3u &&
			node.GetEnvironmentSkyParams(retainedSky) && retainedSky == sky &&
			node.TryRestoreEnvironment(frameGraph, sky.GetEnvironmentKey(), {}),
			"local reflection reset must preserve fallback IBL resources and their sky parameters");
		RequireFrameGraphMaps(frameGraph, 1u);
	}
}

int main()
{
	try
	{
		TestCacheRemainsBoundedAcrossLightingStates();
		TestCacheHitsRefreshLruAndRawSource();
		TestConsumersRetainEvictedMaps();
		TestLocalReflectionResetKeepsEnvironmentCache();
		std::cout << "Environment cache tests passed." << std::endl;
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Environment cache tests failed: " << error.what() << std::endl;
		return 1;
	}
}
