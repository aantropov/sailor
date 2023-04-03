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

		static constexpr uint32_t MaxShadowsInView = 1024;

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

		// Shadow caster material
		TConcurrentMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_shadowMaterials;
		RHI::RHIMaterialPtr GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertex);

		// Record drawcalls
		size_t m_sizePerInstanceData = 0;
		RHI::RHIShaderBindingSetPtr m_perInstanceData;
		TVector<RHI::RHIBufferPtr> m_indirectBuffers;

		// Light matrices and shadowMaps
		RHI::RHIShaderBindingPtr m_lightMatrices;
		RHI::RHIShaderBindingPtr m_shadowMaps;
		
		RHI::RHIRenderTargetPtr m_shadowMap;
		RHI::RHIRenderTargetPtr m_defaultShadowMap;

		static const char* m_name;
	};

	template class TFrameGraphNode<ShadowPrepassNode>;
};
