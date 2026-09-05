#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "FrameGraph/SkyNode.h"
#include "FrameGraph/LocalReflection.h"
#include <memory>
#include <atomic>
#include <mutex>

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

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		SAILOR_API void MarkDirty() { m_bIsDirty = true; };

		// CPU producers hand off owned pixels. Only Process touches GPU resources.
		SAILOR_SHARED_API bool SetLocalReflection(LocalReflectionImage image);
		SAILOR_SHARED_API void ResetLocalReflection();
		bool IsLocalReflectionReady() const { return m_localReflectionReady.load(); }
		uint32_t GetLocalReflectionSamples() const { return m_localReflectionSamples.load(); }
		// Render-thread access, alongside the matching frame-graph samplers.
		LocalReflectionParameters GetLocalReflectionParameters() const { return m_localParameters; }

	protected:
		void ProcessLocalReflection(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr commandList);
		std::mutex m_localReflectionLock;
		std::shared_ptr<const LocalReflectionImage> m_pendingLocalReflection;
		std::shared_ptr<const LocalReflectionImage> m_uploadLocalReflection;
		uint64_t m_pendingLocalRevision = 0u, m_localRevision = 0u;
		std::atomic<bool> m_localReflectionReady{ false };
		std::atomic<uint32_t> m_localReflectionSamples{ 0u };
		RHI::RHITexturePtr m_localUploadTexture;
		LocalReflectionParameters m_localParameters{};

		ShaderSetPtr m_pComputeIrradianceShader{};
		ShaderSetPtr m_pComputeSpecularShader{};
		ShaderSetPtr m_pComputeSheenShader{};
		ShaderSetPtr m_pComputeBrdfShader{};

		RHI::RHIShaderBindingSetPtr m_computeIrradianceBindings{};
		RHI::RHIShaderBindingSetPtr m_computeSpecularBindings{};
		RHI::RHIShaderBindingSetPtr m_computeSheenBindings{};
		RHI::RHIShaderBindingSetPtr m_computeBrdfBindings{};

		TMap<SkyEnvironmentKey, RHI::RHICubemapPtr> m_envCubemaps{};
		TMap<SkyEnvironmentKey, RHI::RHICubemapPtr> m_irradianceCubemaps{};
		TMap<SkyEnvironmentKey, RHI::RHICubemapPtr> m_sheenEnvCubemaps{};
		RHI::RHITexturePtr m_brdfSampler{};

		TexturePtr m_envMapTexture;

		bool m_bIsDirty = false;
		static const char* m_name;
	};

	template class TFrameGraphNode<EnvironmentNode>;
};
