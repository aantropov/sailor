#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class LightCullingNode : public TFrameGraphNode<LightCullingNode>
	{
	public:

		const uint32_t LightsPerTile = 4;
		const uint32_t TileSize = 16;

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		struct PushConstants
		{
			alignas(8) mat4 m_invViewProjection;
			alignas(8) glm::ivec2 m_viewportSize;
			alignas(8) glm::ivec2 m_numTiles;
			alignas(8) int32_t m_lightsNum;
		};

		static const char* m_name;

		ShaderSetPtr m_pComputeShader{};
		RHI::RHIShaderBindingSetPtr m_culledLights;
	};

	template class TFrameGraphNode<LightCullingNode>;
};
