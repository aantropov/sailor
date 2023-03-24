#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "FrameGraph/SkyNode.h"

namespace Sailor::Framegraph
{
	class EnvironmentNode : public TFrameGraphNode<EnvironmentNode>
	{
	public:

		static constexpr uint32_t EnvMapSize = 512;
		static constexpr uint32_t EnvMapLevels = 10;

		static constexpr uint32_t IrradianceMapSize = 32;
		static constexpr uint32_t BrdfLutSize = 256;

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		SAILOR_API void SetDirty() { m_bIsDirty = true; };

	protected:

		ShaderSetPtr m_pComputeIrradianceShader{};
		ShaderSetPtr m_pComputeSpecularShader{};
		ShaderSetPtr m_pComputeBrdfShader{};

		RHI::RHIShaderBindingSetPtr m_computeIrradianceBindings{};
		RHI::RHIShaderBindingSetPtr m_computeSpecularBindings{};
		RHI::RHIShaderBindingSetPtr m_computeBrdfBindings{};

		TConcurrentMap<SkyNode::SkyParams, RHI::RHICubemapPtr> m_envCubemaps{};
		TConcurrentMap<SkyNode::SkyParams, RHI::RHICubemapPtr> m_irradianceCubemaps{};
		RHI::RHITexturePtr m_brdfSampler{};

		TexturePtr m_envMapTexture;

		bool m_bIsDirty = false;
		static const char* m_name;
	};

	template class TFrameGraphNode<EnvironmentNode>;
};
