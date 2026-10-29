#include "FrameGraph/EnvironmentNode.h"
#include "RHI/Cubemap.h"
#include "RHI/Shader.h"

#include <algorithm>
#include <iostream>
#include <limits>
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
		using EnvironmentNode::CompleteLocalReflection;
		using EnvironmentNode::InvalidateLocalReflection;

		uint32_t NumCachedEnvironments() const { return m_numCachedEnvironments; }

		void SeedLocalReflection(RHICubemapPtr raw)
		{
			LocalReflectionCubemap capture;
			capture.m_cubemap = std::move(raw);
			capture.m_generation = 16u;
			capture.m_parameters.m_positionBlend = glm::vec4(0, 0, 0, 1);
			capture.m_parameters.m_minEnabled = glm::vec4(-1, -1, -1, 1);
			capture.m_parameters.m_max = glm::vec4(1);
			m_localParameters = capture.m_parameters;
			m_localReflection = TSharedPtr<const LocalReflectionCubemap>::Make(std::move(capture));
			m_localReflectionGeneration.store(16u);
			m_localReflectionReady.store(true);
			m_bLocalReflectionDirty = false;
		}

		void SeedFiltering(RHICubemapPtr specular, RHICubemapPtr sheen, RHIFencePtr completion)
		{
			m_localFiltering = { m_localReflection, std::move(specular), std::move(sheen), std::move(completion) };
			m_bLocalReflectionDirty = true;
		}

		void SeedPendingLocalReflection(RHICubemapPtr raw, uint64_t generation)
		{
			LocalReflectionCubemap capture{ m_localParameters, std::move(raw), generation };
			m_localReflection = TSharedPtr<const LocalReflectionCubemap>::Make(std::move(capture));
			m_bLocalReflectionDirty = true;
		}

		bool HasPendingLocalGeneration(uint64_t generation) const
		{
			return m_localReflection && m_localReflection->m_generation == generation;
		}

		bool HasFilteringLocalGeneration(uint64_t generation) const
		{
			return m_localFiltering.m_source && m_localFiltering.m_source->m_generation == generation &&
				m_localFiltering.m_completion;
		}

		bool IsLocalResetPending() const
		{
			return m_bLocalReflectionDirty && !m_localReflection && !m_localFiltering.m_completion;
		}

		bool IsLocalRetryPending() const
		{
			return m_bLocalReflectionDirty && m_localReflection && !m_localFiltering.m_completion;
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

	constexpr StringHash MapNames[] = { "g_envCubemap"_h, "g_irradianceCubemap"_h, "g_sheenEnvCubemap"_h };

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
		const RHITexturePtr firstSpecular = frameGraph->GetSampler("g_envCubemap"_h);
		Require(node.TryRestoreEnvironment(frameGraph, MakeKey(1u), currentRaw) &&
			frameGraph->GetSampler("g_envCubemap"_h) == firstSpecular && filteredCounts.m_created == 12u,
			"repeated cache hits must reuse the exact resources without filtering again");
		Require(frameGraph->GetSampler("g_rawEnvCubemap"_h).GetRawPtr() == currentRaw.GetRawPtr(),
			"a cache hit must publish the current ready raw sky, not a cached raw source");

		node.CacheEnvironment(MakeKey(5u), MakeMaps(filteredCounts, 5u));
		Require(!node.TryRestoreEnvironment(frameGraph, MakeKey(2u), firstRaw),
			"inserting a fifth key must evict the least recently used key, not the oldest hit");
		RequireFrameGraphMaps(frameGraph, 1u);
		Require(frameGraph->GetSampler("g_rawEnvCubemap"_h).GetRawPtr() == currentRaw.GetRawPtr(),
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
		RHIShaderBindingPtr retainedBinding = RHIShaderBindingPtr::Make();
		RHICubemapPtr previousFogEnvironment;
		{
			EnvironmentNodeProbe node;
			auto frameGraph = RHIFrameGraphPtr::Make();
			node.CacheEnvironment(MakeKey(1u), MakeMaps(counts, 1u));
			Require(node.TryRestoreEnvironment(frameGraph, MakeKey(1u), {}), "the first bundle must be available");
			for (uint32_t channel = 0u; channel < 3u; ++channel)
			{
				retainedBinding->SetTextureBinding(channel, frameGraph->GetSampler(MapNames[channel]));
			}
			previousFogEnvironment = frameGraph->GetSampler("g_irradianceCubemap"_h).DynamicCast<RHICubemap>();
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
		auto retainedConsumer = retainedBinding;
		retainedBinding.Clear();
		Require(counts.Live() == 3u, "another RHIPtr must retain the binding and its evicted environment maps");
		for (uint32_t channel = 0u; channel < 3u; ++channel)
		{
			const auto cube = retainedConsumer->GetTextureBinding(channel).DynamicCast<CountedCubemap>();
			Require(cube && cube->m_state == 1u && cube->m_channel == channel,
				"retained bindings must still refer to the original complete bundle");
		}
		retainedConsumer.Clear();
		Require(counts.Live() == 1u, "the previous fog irradiance must remain alive independently of other maps");
		previousFogEnvironment.Clear();
		Require(counts.Live() == 0u, "the final consumer release must destroy the last evicted map");
	}

	void TestLocalReflectionCaptureValidation()
	{
		LocalReflectionCaptureRequest request;
		request.m_parameters.m_positionBlend = glm::vec4(0, 0, 0, 1);
		request.m_parameters.m_minEnabled = glm::vec4(-1, -1, -1, 1);
		request.m_parameters.m_max = glm::vec4(1);
		Require(request.IsValid(), "an interior capture with a finite frustum must be accepted");
		for (uint32_t size : { 16u, 128u, 512u })
		{
			request.m_faceSize = size;
			Require(request.IsValid(), "supported face resolutions must be accepted");
		}
		for (uint32_t size : { 0u, 15u, 17u, 1024u })
		{
			request.m_faceSize = size;
			Require(!request.IsValid(), "invalid or unbounded face resolutions must be rejected");
		}
		request.m_faceSize = 128u;
		for (float value : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
		{
			auto invalid = request;
			invalid.m_nearPlane = value;
			Require(!invalid.IsValid(), "capture must reject a non-finite or nonpositive near plane");
			invalid = request;
			invalid.m_farPlane = value;
			Require(!invalid.IsValid(), "capture must reject a non-finite or reversed far plane");
		}
		auto invalid = request;
		invalid.m_farPlane = invalid.m_nearPlane;
		Require(!invalid.IsValid(), "the capture frustum must have positive depth");
		invalid = request;
		invalid.m_parameters.m_positionBlend.w = 0;
		Require(!invalid.IsValid(), "zero box blend distance must be rejected");
		for (uint32_t axis = 0u; axis < 3u; ++axis)
		{
			invalid = request;
			invalid.m_parameters.m_positionBlend[axis] = request.m_parameters.m_max[axis];
			Require(!invalid.IsValid(), "the capture position must lie strictly inside the projection box");
			invalid = request;
			invalid.m_parameters.m_minEnabled[axis] = request.m_parameters.m_max[axis];
			Require(!invalid.IsValid(), "degenerate projection boxes must be rejected");
			invalid = request;
			invalid.m_parameters.m_max[axis] = std::numeric_limits<float>::quiet_NaN();
			Require(!invalid.IsValid(), "non-finite projection boxes must be rejected");
		}
		LocalReflectionCubemap capture;
		capture.m_parameters = request.m_parameters;
		capture.m_generation = 1u;
		Require(!capture.IsValid(), "a generation without a completed GPU cubemap must be rejected");
	}

	void TestUnfinishedOrFailedLocalFilterKeepsPublishedBundle()
	{
		ResourceCounts sourceCounts, filterCounts, publishedCounts;
		EnvironmentNodeProbe node;
		auto frameGraph = RHIFrameGraphPtr::Make();
		node.SeedLocalReflection(TRefPtr<CountedCubemap>::Make(sourceCounts, 1u, 0u));
		const LocalReflectionParameters previousParameters = node.GetLocalReflectionParameters();
		auto previousSpecular = TRefPtr<CountedCubemap>::Make(publishedCounts, 1u, 0u);
		auto previousSheen = TRefPtr<CountedCubemap>::Make(publishedCounts, 1u, 1u);
		frameGraph->SetSampler("g_localEnvCubemap"_h, previousSpecular);
		frameGraph->SetSampler("g_localSheenEnvCubemap"_h, previousSheen);
		auto completion = RHIFencePtr::Make();
		node.SeedFiltering(TRefPtr<CountedCubemap>::Make(filterCounts, 2u, 0u),
			TRefPtr<CountedCubemap>::Make(filterCounts, 2u, 1u), completion);
		node.CompleteLocalReflection(frameGraph);
		Require(filterCounts.Live() == 2u && sourceCounts.Live() == 1u &&
			node.GetLocalReflectionGeneration() == 16u && node.IsLocalReflectionReady(),
			"an unfinished filter must retain its GPU inputs and leave the published status unchanged");
		completion->MarkSubmissionFailed();
		node.CompleteLocalReflection(frameGraph);
		Require(node.IsLocalRetryPending() && filterCounts.Live() == 0u && sourceCounts.Live() == 1u &&
			node.GetLocalReflectionGeneration() == 16u && node.IsLocalReflectionReady(),
			"failed filtering must release rejected outputs and retain the source for a retry");
		Require(frameGraph->GetSampler("g_localEnvCubemap"_h) == previousSpecular &&
			frameGraph->GetSampler("g_localSheenEnvCubemap"_h) == previousSheen &&
			node.GetLocalReflectionParameters().m_positionBlend == previousParameters.m_positionBlend &&
			node.GetLocalReflectionParameters().m_minEnabled == previousParameters.m_minEnabled &&
			node.GetLocalReflectionParameters().m_max == previousParameters.m_max,
			"a pending or failed filter must preserve the previous pair and its matching box parameters");
		node.SeedFiltering(TRefPtr<CountedCubemap>::Make(filterCounts, 3u, 0u),
			TRefPtr<CountedCubemap>::Make(filterCounts, 3u, 1u), RHIFencePtr::Make());
		node.Clear();
		node.CompleteLocalReflection(frameGraph);
		Require(node.IsLocalResetPending() && sourceCounts.Live() == 0u && filterCounts.Live() == 0u &&
			node.GetLocalReflectionGeneration() == 0u && !node.IsLocalReflectionReady(),
			"reset must invalidate pending capture and filter ownership without republishing it");
	}

	void TestLocalReflectionResetKeepsEnvironmentCache()
	{
		ResourceCounts filteredCounts;
		ResourceCounts sourceCounts;
		EnvironmentNodeProbe node;
		auto frameGraph = RHIFrameGraphPtr::Make();
		const SkyParameters sky;
		node.CacheEnvironment(sky.GetEnvironmentKey(), MakeMaps(filteredCounts, 1u));
		node.SeedEnvironmentSky(sky);
		node.SeedLocalReflection(TRefPtr<CountedCubemap>::Make(sourceCounts, 0u, 0u));
		Require(node.IsLocalReflectionReady() && node.GetLocalReflectionGeneration() == 16u,
			"the reset fixture must contain a published local reflection");
		node.Clear();
		Require(!node.IsLocalReflectionReady() && node.GetLocalReflectionGeneration() == 0u &&
			node.GetLocalReflectionParameters().m_minEnabled.w == 0.0f && node.IsLocalResetPending() &&
			sourceCounts.Live() == 0u,
			"Clear must reset local reflection status, parameters and GPU capture ownership");
		SkyParameters retainedSky;
		Require(node.NumCachedEnvironments() == 1u && filteredCounts.Live() == 3u &&
			node.GetEnvironmentSkyParams(retainedSky) && retainedSky == sky &&
			node.TryRestoreEnvironment(frameGraph, sky.GetEnvironmentKey(), {}),
			"local reflection reset must preserve fallback IBL resources and their sky parameters");
		RequireFrameGraphMaps(frameGraph, 1u);
	}

	void TestLateResetKeepsNewerLocalCapture()
	{
		ResourceCounts sourceCounts, filterCounts;
		EnvironmentNodeProbe node;
		node.SeedLocalReflection(TRefPtr<CountedCubemap>::Make(sourceCounts, 16u, 0u));
		node.SeedPendingLocalReflection(TRefPtr<CountedCubemap>::Make(sourceCounts, 32u, 0u), 32u);
		node.InvalidateLocalReflection(16u);
		Require(!node.IsLocalReflectionReady() && node.GetLocalReflectionGeneration() == 0u &&
			node.GetLocalReflectionParameters().m_minEnabled.w == 0.0f &&
			node.HasPendingLocalGeneration(32u) && sourceCounts.Live() == 1u,
			"an older owner's reset must disable its published capture without deleting a newer queued source");
		node.SeedFiltering(TRefPtr<CountedCubemap>::Make(filterCounts, 32u, 0u),
			TRefPtr<CountedCubemap>::Make(filterCounts, 32u, 1u), RHIFencePtr::Make());
		node.InvalidateLocalReflection(16u);
		Require(node.HasPendingLocalGeneration(32u) && node.HasFilteringLocalGeneration(32u) &&
			sourceCounts.Live() == 1u && filterCounts.Live() == 2u,
			"a repeated late reset must preserve a newer generation's in-flight filtering and GPU source");
		node.Clear();
		Require(sourceCounts.Live() == 0u && filterCounts.Live() == 0u && node.IsLocalResetPending(),
			"an explicit unconditional reset must still invalidate the newer pending generation");
	}
}

int main()
{
	try
	{
		TestCacheRemainsBoundedAcrossLightingStates();
		TestCacheHitsRefreshLruAndRawSource();
		TestConsumersRetainEvictedMaps();
		TestLocalReflectionCaptureValidation();
		TestUnfinishedOrFailedLocalFilterKeepsPublishedBundle();
		TestLocalReflectionResetKeepsEnvironmentCache();
		TestLateResetKeepsNewerLocalCapture();
		std::cout << "Environment cache tests passed." << std::endl;
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Environment cache tests failed: " << error.what() << std::endl;
		return 1;
	}
}
