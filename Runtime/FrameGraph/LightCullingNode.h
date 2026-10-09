#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "RHI/RenderSubmission.h"

namespace Sailor::Framegraph
{
	class LightCullingNode : public TFrameGraphNode<LightCullingNode>
	{
	public:

		static const uint32_t LightsPerTile = 128;
		static const uint32_t TileSize = 16;

		SAILOR_API static StringHash GetName() { return "LightCulling"_h; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		class SubmissionResources final : public RHI::RHIFrameGraphSubmissionResource
		{
		public:
			void ResetForSubmission() override {}

			RHI::RHIShaderBindingSetPtr m_bindings{};
			RHI::RHIShaderBindingPtr m_publishedIndices{};
			size_t m_tileCapacity = 0;
		};

		struct PushConstants
		{
			alignas(8) mat4 m_invViewProjection;
			alignas(8) glm::ivec2 m_viewportSize;
			alignas(8) glm::ivec2 m_numTiles;
			alignas(8) int32_t m_lightsNum;
		};

		ShaderSetPtr m_pComputeShader{};
	};

	template class TFrameGraphNode<LightCullingNode>;
};
