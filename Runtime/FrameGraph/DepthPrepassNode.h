#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor
{
	// TODO: We have an issue with enabled MSAA, since MSAA depth target is stored in temporary MSAA target that is handled by VulkanGraphicsDriver
	class DepthPrepassNode : public TFrameGraphNode<DepthPrepassNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;
		SAILOR_API RHI::ESortingOrder GetSortingOrder() const;

	protected:

		TConcurrentMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_depthOnlyMaterials;

		RHI::RHIMaterialPtr GetOrAddDepthMaterial(RHI::RHIVertexDescriptionPtr vertex);
		TVector<RHI::RHIBufferPtr> m_indirectBuffers;

		static const char* m_name;
	};

	template class TFrameGraphNode<DepthPrepassNode>;
};
