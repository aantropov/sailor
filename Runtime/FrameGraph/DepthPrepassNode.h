#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/Batch.hpp"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor
{
	// TODO: We have an issue with enabled MSAA, since MSAA depth target is stored in temporary MSAA target that is handled by VulkanGraphicsDriver
	class DepthPrepassNode : public TFrameGraphNode<DepthPrepassNode>
	{
	public:

		struct PerInstanceData
		{
			glm::mat4 model;
			vec4 sphereBounds;
			uint32_t materialInstance = 0;
			uint32_t bIsCulled = 0;
			uint64_t padding;

			bool operator==(const PerInstanceData& rhs) const { return this->materialInstance == rhs.materialInstance && this->model == rhs.model; }

			size_t GetHash() const
			{
				hash<glm::mat4> p;
				return p(model);
			}
		};

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual Tasks::TaskPtr<void, void> Prepare(RHI::RHIFrameGraphPtr frameGraph, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;
		SAILOR_API RHI::ESortingOrder GetSortingOrder() const;

	protected:

		uint32_t m_numMeshes = 0;
		SpinLock m_syncSharedResources;
		RHI::TDrawCalls<PerInstanceData> m_drawCalls;
		TSet<RHI::RHIBatch> m_batches;

		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_depthOnlyMaterials;
		RHI::RHIShaderBindingSetPtr m_perInstanceData;
		size_t m_sizePerInstanceData = 0;

		RHI::RHIMaterialPtr GetOrAddDepthMaterial(RHI::RHIVertexDescriptionPtr vertex);
		TVector<RHI::RHIBufferPtr> m_indirectBuffers;

		// Culling
		TVector<RHI::RHIShaderBindingSetPtr> m_cullingIndirectBufferBinding;
		ShaderSetPtr m_pComputeMeshCullingShader{};
		RHI::RHIShaderBindingSetPtr m_computeMeshCullingBindings{};

		static const char* m_name;
	};

	namespace Framegraph
	{
		template class TFrameGraphNode<DepthPrepassNode>;
	}
};
