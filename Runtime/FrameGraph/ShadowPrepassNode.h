#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor
{
	class ShadowPrepassNode : public TFrameGraphNode<ShadowPrepassNode>
	{
	public:

		struct PerInstanceData
		{
			glm::mat4 model;

			bool operator==(const PerInstanceData& rhs) const { return this->model == model; }

			size_t GetHash() const
			{
				hash<glm::mat4> p;
				return p(model);
			}
		};

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:

		TConcurrentMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_shadowMaterials;
		RHI::RHIShaderBindingSetPtr m_perInstanceData;
		size_t m_sizePerInstanceData = 0;

		RHI::RHIMaterialPtr GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertex);		
		TVector<RHI::RHIBufferPtr> m_indirectBuffers;

		RHI::RHIRenderTargetPtr m_shadowMap;

		static const char* m_name;
	};

	template class TFrameGraphNode<ShadowPrepassNode>;
};
