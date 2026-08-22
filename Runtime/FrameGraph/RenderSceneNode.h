#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/RenderSubmission.h"
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

			bool operator==(const PerInstanceData& rhs) const
			{
				return model == rhs.model &&
					sphereBounds == rhs.sphereBounds &&
					materialInstance == rhs.materialInstance &&
					skeletonOffset == rhs.skeletonOffset &&
					bIsCulled == rhs.bIsCulled &&
					padding == rhs.padding &&
					bakedVolumeScale == rhs.bakedVolumeScale;
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

		class SubmissionResources final : public RHI::RHIFrameGraphSubmissionResource
		{
		public:
			void ResetForSubmission() override
			{
				m_orderedDrawItems.Clear(false);
				m_renderPassColorAttachments.Clear(false);
				m_renderPassColorSurfaces.Clear(false);
				m_cullingDispatchBindings.Clear(false);
				m_arenaRangeInstances.Clear(false);
				m_arenaRangeStableKeys.Clear(false);
				m_arenaRangeMaterialVersionRuns.Clear(false);
			}

			void InvalidateSubmission() override
			{
				ResetForSubmission();
				m_packet.InvalidateUploadedState();
			}

			RHI::TPackedDrawPacket<PerInstanceData> m_packet;
			TVector<OrderedDrawItem> m_orderedDrawItems;
			TVector<RHI::RHIBufferPtr> m_indirectBuffers;
			RHI::RHIShaderBindingSetPtr m_perInstanceData{};
			size_t m_sizePerInstanceData = 0u;
			size_t m_sizeInstanceIndices = 0u;
			TVector<RHI::RHIShaderBindingSetPtr> m_cullingIndirectBufferBinding;
			RHI::RHIShaderBindingSetPtr m_computeMeshCullingBindings{};
			RHI::RHITexturePtr m_cullingDepthHighZ{};
			RHI::RHIShaderBindingSetPtr m_nodeLightsBindings{};
			RHI::RHIShaderBindingSetPtr m_nodeLightsSource{};
			RHI::RHITexturePtr m_transmissionTexture{};
			uint64_t m_nodeLightsSourceRevision = 0ull;
			TVector<RHI::RHITexturePtr> m_renderPassColorAttachments{};
			TVector<RHI::RHISurfacePtr> m_renderPassColorSurfaces{};
			TVector<RHI::RHIShaderBindingSetPtr> m_cullingDispatchBindings{};
			TVector<PerInstanceData> m_arenaRangeInstances{};
			TVector<uint64_t> m_arenaRangeStableKeys{};
			TVector<RHI::PackedDrawArenaMaterialRun> m_arenaRangeMaterialVersionRuns{};
			std::array<TextureDependencyCollector,
				RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				m_requestedPacketTextures{};
		};

		SAILOR_SHARED_API static const char* m_name;

		SpinLock m_syncSharedResources;

		// Culling
		ShaderSetPtr m_pComputeMeshCullingShader{};

		// Shared cache across platforms; macOS relies on it most because of descriptor pressure.
		TextureBindingCache m_textureBindingCache;
		RHI::TPackedDrawPacketPayloadCache<PerInstanceData> m_packetPayloadCache;
		RHI::TPackedDrawPagedArenaCache<PerInstanceData> m_pagedArenaCache;
	};

	template class TFrameGraphNode<RenderSceneNode>;
};
