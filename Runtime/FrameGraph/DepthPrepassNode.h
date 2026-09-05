#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/RenderSubmission.h"
#include "RHI/Batch.hpp"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "FrameGraph/RenderSceneTextureCache.h"
#include "RHI/MotionHistory.h"

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
			uint32_t skeletonOffset = 0;
			uint32_t padding = 0;
			uint32_t reserved = 0;

			bool operator==(const PerInstanceData& rhs) const
			{
				return model == rhs.model &&
					sphereBounds == rhs.sphereBounds &&
					materialInstance == rhs.materialInstance &&
					skeletonOffset == rhs.skeletonOffset &&
					padding == rhs.padding &&
					reserved == rhs.reserved;
			}

		};

		// Custom depth shaders reuse the main-pass material layout. Keep this rare
		// stream separate so ordinary opaque/masked depth records stay compact.
		struct CustomPerInstanceData
		{
			glm::mat4 model;
			vec4 sphereBounds;
			uint32_t materialInstance = 0u;
			uint32_t skeletonOffset = 0u;
			uint32_t bIsCulled = 0u;
			uint32_t padding = 0u;
			vec4 bakedVolumeScale = vec4(1.0f);
			RHI::RHIObjectMotionData motion{};

			bool operator==(const CustomPerInstanceData& rhs) const
			{
				return model == rhs.model &&
					sphereBounds == rhs.sphereBounds &&
					materialInstance == rhs.materialInstance &&
					skeletonOffset == rhs.skeletonOffset &&
					bIsCulled == rhs.bIsCulled &&
					padding == rhs.padding &&
					bakedVolumeScale == rhs.bakedVolumeScale && motion == rhs.motion;
			}

		};

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual Tasks::TaskPtr<void, void> Prepare(RHI::RHIFrameGraphPtr frameGraph, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;
		SAILOR_API RHI::ESortingOrder GetSortingOrder() const;

	protected:
		class SubmissionResources final : public RHI::RHIFrameGraphSubmissionResource
		{
		public:
			void ResetForSubmission() override
			{
				m_cullingDispatchBindings.Clear(false);
				m_arenaRangeInstances.Clear(false);
				m_arenaRangeStableKeys.Clear(false);
				m_arenaRangeMaterialVersionRuns.Clear(false);
				m_customArenaRangeInstances.Clear(false);
				m_customArenaRangeStableKeys.Clear(false);
				m_customArenaRangeMaterialVersionRuns.Clear(false);
			}

			void InvalidateSubmission() override
			{
				ResetForSubmission();
				m_packet.InvalidateUploadedState();
				m_customPacket.InvalidateUploadedState();
			}

			RHI::TPackedDrawPacket<PerInstanceData> m_packet;
			RHI::RHIShaderBindingSetPtr m_perInstanceData{};
			size_t m_sizePerInstanceData = 0u;
			size_t m_sizeInstanceIndices = 0u;
			TVector<RHI::RHIBufferPtr> m_indirectBuffers;
			TVector<RHI::RHIShaderBindingSetPtr> m_cullingIndirectBufferBinding;
			RHI::RHIShaderBindingSetPtr m_computeMeshCullingBindings{};
			RHI::RHITexturePtr m_cullingDepthHighZ{};
			RHI::TPackedDrawPacket<CustomPerInstanceData> m_customPacket;
			RHI::RHIShaderBindingSetPtr m_customPerInstanceData{};
			size_t m_sizeCustomPerInstanceData = 0u;
			size_t m_sizeCustomInstanceIndices = 0u;
			RHI::RHIBufferPtr m_customIndirectBuffer{};
			TVector<RHI::RHIShaderBindingSetPtr> m_cullingDispatchBindings{};
			TVector<PerInstanceData> m_arenaRangeInstances{};
			TVector<uint64_t> m_arenaRangeStableKeys{};
			TVector<RHI::PackedDrawArenaMaterialRun> m_arenaRangeMaterialVersionRuns{};
			TVector<CustomPerInstanceData> m_customArenaRangeInstances{};
			TVector<uint64_t> m_customArenaRangeStableKeys{};
			TVector<RHI::PackedDrawArenaMaterialRun> m_customArenaRangeMaterialVersionRuns{};
			std::array<Framegraph::TextureDependencyCollector,
				RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				m_requestedPacketTextures{};
		};

		struct DepthMaterialKey
		{
			RHI::VertexAttributeBits m_vertexAttributes = 0u;
			RHI::ECullMode m_cullMode = RHI::ECullMode::Back;

			bool operator==(const DepthMaterialKey& rhs) const
			{
				return m_vertexAttributes == rhs.m_vertexAttributes &&
					m_cullMode == rhs.m_cullMode;
			}

			size_t GetHash() const
			{
				size_t result = std::hash<RHI::VertexAttributeBits>{}(m_vertexAttributes);
				HashCombine(result, static_cast<uint32_t>(m_cullMode));
				return result;
			}
		};

		SpinLock m_syncSharedResources;

		TMap<DepthMaterialKey, RHI::RHIMaterialPtr> m_depthOnlyMaterials;
		TMap<DepthMaterialKey, RHI::RHIMaterialPtr> m_skinnedDepthOnlyMaterials;
		TMap<DepthMaterialKey, RHI::RHIMaterialPtr> m_maskedDepthOnlyMaterials;
		TMap<DepthMaterialKey, RHI::RHIMaterialPtr> m_skinnedMaskedDepthOnlyMaterials;
		RHI::RHIMaterialPtr GetOrAddDepthMaterial(
			RHI::RHIVertexDescriptionPtr vertex,
			bool bSkinned,
			bool bMasked,
			RHI::ECullMode cullMode);
		// Culling
		ShaderSetPtr m_pComputeMeshCullingShader{};
		Framegraph::TextureBindingCache m_textureBindingCache;
		RHI::TPackedDrawPacketPayloadCache<PerInstanceData> m_packetPayloadCache;
		RHI::TPackedDrawPacketPayloadCache<CustomPerInstanceData> m_customPacketPayloadCache;
		RHI::TPackedDrawPagedArenaCache<PerInstanceData> m_pagedArenaCache;
		RHI::TPackedDrawPagedArenaCache<CustomPerInstanceData> m_customPagedArenaCache;

		SAILOR_SHARED_API static const char* m_name;
	};

	namespace Framegraph
	{
		template class TFrameGraphNode<DepthPrepassNode>;
	}
};
