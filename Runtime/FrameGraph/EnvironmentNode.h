#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class EnvironmentNode : public TFrameGraphNode<EnvironmentNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		ShaderSetPtr m_pComputeIrradianceShader{};
		ShaderSetPtr m_pComputeSpecularShader{};
		ShaderSetPtr m_pComputeBrdfShader{};

		RHI::RHIShaderBindingSetPtr m_computeIrradianceBindings{};
		RHI::RHIShaderBindingSetPtr m_computeSpecularBindings{};
		RHI::RHIShaderBindingSetPtr m_computeBrdfBindings{};

		RHI::RHITexturePtr m_brdfSampler;
		RHI::RHICubemapPtr m_irradianceCubemap;
		RHI::RHICubemapPtr m_specularCubemap;

		static const char* m_name;
	};

	template class TFrameGraphNode<EnvironmentNode>;
};
