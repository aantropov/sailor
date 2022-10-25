#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class RenderSceneNode : public TFrameGraphNode<RenderSceneNode>
	{
	public:

		class PerInstanceData
		{
		public:
			alignas(16) glm::mat4 model;
			alignas(16) uint32_t materialInstance = 0;

			bool operator==(const PerInstanceData& rhs) const { return this->materialInstance == rhs.materialInstance && this->model == rhs.model; }

			size_t GetHash() const
			{
				hash<glm::mat4> p;
				return p(model);
			}
		};

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;
		SAILOR_API RHI::ESortingOrder GetSortingOrder() const;

	protected:

		static const char* m_name;
		TVector<RHI::RHIBufferPtr> m_indirectBuffers;

		RHI::RHIShaderBindingSetPtr m_perInstanceData;
		size_t m_sizePerInstanceData = 0;
	};

	template class TFrameGraphNode<RenderSceneNode>;
};
