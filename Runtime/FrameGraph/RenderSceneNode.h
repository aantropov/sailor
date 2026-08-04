#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "FrameGraph/RenderSceneTextureCache.h"
#include "RHI/Batch.hpp"

namespace Sailor::Framegraph
{
	class RenderSceneNode : public TFrameGraphNode<RenderSceneNode>
	{
	public:

		class PerInstanceData
		{
		public:

			glm::mat4 model;
			vec4 sphereBounds;
			uint32_t materialInstance = 0;
			uint32_t skeletonOffset = 0;
			uint32_t bIsCulled = 0;
			uint32_t padding = 0;
			vec4 bakedVolumeScale = vec4(1.0f);

			bool operator==(const PerInstanceData& rhs) const { return this->materialInstance == rhs.materialInstance && this->model == rhs.model; }

			size_t GetHash() const
			{
				hash<glm::mat4> p;
				return p(model);
			}
		};

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual Sailor::Tasks::TaskPtr<void, void> Prepare(RHI::RHIFrameGraphPtr frameGraph, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;
		SAILOR_API RHI::ESortingOrder GetSortingOrder() const;

	protected:
		struct OrderedDrawItem
		{
			RHI::RHIBatch m_batch;
			RHI::RHIMeshPtr m_mesh;
			PerInstanceData m_instanceData;
			float m_cameraDepth = 0.0f;
			size_t m_staticMeshEcs = 0;
			size_t m_meshIndex = 0;
		};

		void ProcessBackToFront(RHI::RHIFrameGraphPtr frameGraph,
			RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView,
			const RHI::RHIShaderBindingPtr& storageBinding,
			const std::string& queueTag);

		SAILOR_SHARED_API static const char* m_name;

		uint32_t m_numMeshes = 0;
		SpinLock m_syncSharedResources;
		RHI::TDrawCalls<PerInstanceData> m_drawCalls;
		TSet<RHI::RHIBatch> m_batches;
		TVector<OrderedDrawItem> m_orderedDrawItems;
		TVector<RHI::RHIBufferPtr> m_indirectBuffers;

		RHI::RHIShaderBindingSetPtr m_perInstanceData;
		size_t m_sizePerInstanceData = 0;

		// Culling
		TVector<RHI::RHIShaderBindingSetPtr> m_cullingIndirectBufferBinding;
		ShaderSetPtr m_pComputeMeshCullingShader{};
		RHI::RHIShaderBindingSetPtr m_computeMeshCullingBindings{};

		// Shared cache across platforms; macOS relies on it most because of descriptor pressure.
		TextureBindingCache m_textureBindingCache;
	};

	template class TFrameGraphNode<RenderSceneNode>;
};
