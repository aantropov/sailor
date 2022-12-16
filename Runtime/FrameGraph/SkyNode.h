#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class SkyNode : public TFrameGraphNode<SkyNode>
	{
		const uint32_t SkyResolution = 128u;
		const uint32_t SunResolution = 16u;

	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph,
			RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView) override;

		SAILOR_API virtual void Clear() override;

		SAILOR_API RHI::RHIShaderBindingSetPtr GetShaderBindings() { return m_pShaderBindings; }

	protected:

		ShaderSetPtr m_pSunShader{};
		ShaderSetPtr m_pSkyShader{};
		ShaderSetPtr m_pComposeShader{};

		RHI::RHIMaterialPtr m_pSkyMaterial{};
		RHI::RHIMaterialPtr m_pSunMaterial{};
		RHI::RHIMaterialPtr m_pComposeMaterial{};

		RHI::RHIShaderBindingSetPtr m_pShaderBindings{};

		RHI::RHITexturePtr m_pSkyTexture;
		RHI::RHITexturePtr m_pSunTexture;

		static const char* m_name;
	};

	template class TFrameGraphNode<SkyNode>;
};
