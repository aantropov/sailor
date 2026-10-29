#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "FrameGraph/SkyNode.h"
#include "FrameGraph/LocalReflection.h"
#include "RHI/Fence.h"
#include <array>
#include <atomic>

namespace Sailor::Framegraph
{
	class EnvironmentNode : public TFrameGraphNode<EnvironmentNode>
	{
	public:

		static constexpr uint32_t EnvMapSize = 512;
		static constexpr uint32_t EnvMapLevels = 10;
		static constexpr uint32_t SheenEnvMapSize = 128;
		static constexpr uint32_t SheenEnvMapLevels = 8;

		static constexpr uint32_t IrradianceMapSize = 32;
		static constexpr uint32_t BrdfLutSize = 256;

		SAILOR_API static StringHash GetName() { return "Environment"_h; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		SAILOR_API void MarkDirty() { m_bIsDirty = true; };
		// Render-thread lighting source paired with the prepared diffuse fallback. Authored HDR
		// maps may already contain a sun, so they do not add the analytic Sky sun.
		SAILOR_API bool GetEnvironmentSkyParams(SkyParameters& parameters) const
		{
			if (!m_environmentUsesSky) return false;
			parameters = m_environmentSkyParams;
			return true;
		}

		// Updates are queued to Render. The game thread may read the published status.
		SAILOR_SHARED_API bool SetLocalReflection(LocalReflectionCubemap capture);
		SAILOR_SHARED_API void ResetLocalReflection(uint64_t maximumGeneration = UINT64_MAX);
		bool IsLocalReflectionReady() const { return m_localReflectionReady.load(); }
		uint64_t GetLocalReflectionGeneration() const { return m_localReflectionGeneration.load(); }
		// Render-thread access, alongside the matching frame-graph samplers.
		LocalReflectionParameters GetLocalReflectionParameters() const { return m_localParameters; }

	protected:
		struct EnvironmentMaps
		{
			RHI::RHICubemapPtr m_specular;
			RHI::RHICubemapPtr m_irradiance;
			RHI::RHICubemapPtr m_sheen;
		};

		struct CachedEnvironment
		{
			SkyEnvironmentKey m_key;
			EnvironmentMaps m_maps;
		};

		SAILOR_SHARED_API bool TryRestoreEnvironment(RHI::RHIFrameGraphPtr frameGraph,
			const SkyEnvironmentKey& key, RHI::RHICubemapPtr rawCubemap);
		// Called after a miss, once the complete bundle has been filtered.
		SAILOR_SHARED_API void CacheEnvironment(const SkyEnvironmentKey& key, EnvironmentMaps maps);

		struct LocalReflectionFiltering
		{
			TSharedPtr<const LocalReflectionCubemap> m_source;
			RHI::RHICubemapPtr m_specular;
			RHI::RHICubemapPtr m_sheen;
			RHI::RHIFencePtr m_completion;
		};

		SAILOR_SHARED_API void CompleteLocalReflection(RHI::RHIFrameGraphPtr frameGraph);
		SAILOR_SHARED_API void InvalidateLocalReflection(uint64_t maximumGeneration);
		void ProcessLocalReflection(RHI::RHIFrameGraphPtr frameGraph);
		TSharedPtr<const LocalReflectionCubemap> m_localReflection;
		LocalReflectionFiltering m_localFiltering;
		bool m_bLocalReflectionDirty = false;
		bool m_bLocalReflectionResetPending = false;
		std::atomic<bool> m_localReflectionReady{ false };
		std::atomic<uint64_t> m_localReflectionGeneration{ 0u };
		LocalReflectionParameters m_localParameters{};

		ShaderSetPtr m_pComputeIrradianceShader{};
		ShaderSetPtr m_pComputeSpecularShader{};
		ShaderSetPtr m_pComputeSheenShader{};
		ShaderSetPtr m_pComputeBrdfShader{};

		RHI::RHIShaderBindingSetPtr m_computeIrradianceBindings{};
		RHI::RHIShaderBindingSetPtr m_computeSpecularBindings{};
		RHI::RHIShaderBindingSetPtr m_computeSheenBindings{};
		RHI::RHIShaderBindingSetPtr m_computeBrdfBindings{};

		// Render-owned, oldest first. Consumers retain their own references after eviction.
		std::array<CachedEnvironment, 4u> m_environmentCache{};
		uint32_t m_numCachedEnvironments = 0u;
		RHI::RHITexturePtr m_brdfSampler{};

		TexturePtr m_envMapTexture;
		RHI::RHITexturePtr m_authoredSource;
		RHI::RHICubemapPtr m_staticRawCubemap;
		SkyParameters m_environmentSkyParams{};
		bool m_environmentUsesSky = false;

		// Authored HDR environments must initialize without a Sky node to trigger them.
		bool m_bIsDirty = true;

	};

	template class TFrameGraphNode<EnvironmentNode>;
};
