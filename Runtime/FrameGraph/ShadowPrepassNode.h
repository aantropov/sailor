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

namespace Sailor
{
	class ShadowPrepassNode : public TFrameGraphNode<ShadowPrepassNode>
	{
	public:

		struct PerInstanceData
		{
			glm::mat4 model;
			glm::vec4 sphereBounds{};
			uint32_t materialInstance = 0;
			uint32_t skeletonOffset = 0;
			uint32_t bIsCulled = 0;
			uint32_t padding = 0;
			glm::vec4 bakedVolumeScale{ 1.0f };
			float baseColorAlpha = 1.0f;
			uint32_t baseColorSampler = 0;
			float alphaCutoff = 0.5f;
			uint32_t maskedPadding = 0;

			bool operator==(const PerInstanceData& rhs) const
			{
				return model == rhs.model &&
					sphereBounds == rhs.sphereBounds &&
					materialInstance == rhs.materialInstance &&
					skeletonOffset == rhs.skeletonOffset &&
					bIsCulled == rhs.bIsCulled &&
					padding == rhs.padding &&
					bakedVolumeScale == rhs.bakedVolumeScale &&
					baseColorAlpha == rhs.baseColorAlpha &&
					baseColorSampler == rhs.baseColorSampler &&
					alphaCutoff == rhs.alphaCutoff &&
					maskedPadding == rhs.maskedPadding;
			}

		};

		SAILOR_API static const char* GetName() { return m_name; }
		static constexpr float GetRasterShadowBias(
			RHI::EShadowType shadowType,
			float configuredBias) noexcept
		{
			return shadowType == RHI::EShadowType::PCF ? configuredBias : 0.0f;
		}

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		SAILOR_API static void CalculateLightProjectionForCascades(
			const glm::mat4& lightView,
			const glm::mat4& cameraWorld,
			float aspect,
			float fovY,
			float cameraNearPlane,
			float cameraFarPlane,
			TVector<glm::mat4>& outMatrices);
		SAILOR_API static glm::mat4 CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar, float zMult, glm::ivec2 shadowMapResolution = glm::ivec2(0), float zSourceExtension = 0.0f);

	protected:
		class SubmissionResources final : public RHI::RHIFrameGraphSubmissionResource
		{
		public:
			struct ShadowViewResources
			{
				uint64_t m_viewKey = 0ull;
				RHI::TPackedDrawPacket<PerInstanceData> m_packet{};
				size_t m_sizePerInstanceData = 0u;
				size_t m_sizeInstanceIndices = 0u;
				RHI::RHIShaderBindingSetPtr m_perInstanceData{};
				RHI::RHIBufferPtr m_indirectBuffer{};
				bool m_bUploadedThisSubmission = false;

				void Begin(uint64_t viewKey)
				{
					if (m_viewKey != viewKey)
					{
						*this = {};
						m_viewKey = viewKey;
					}
					m_packet.Reset();
					m_bUploadedThisSubmission = false;
				}
			};

			void ResetForSubmission() override
			{
				m_numActiveShadowViews = 0u;
				m_activeShadowViews.Clear(false);
				m_shadowPayloadRevisions.Clear(false);
				m_buildShadowPayloads.Clear(false);
				m_shadowPayloadComplete.Clear(false);
				m_renderPassColorAttachments.Clear(false);
				m_blurDrawBindingSets.Clear(false);
				m_arenaRangeInstances.Clear(false);
				m_arenaRangeStableKeys.Clear(false);
				m_arenaRangeMaterialVersionRuns.Clear(false);
			}

			void InvalidateSubmission() override
			{
				ResetForSubmission();
				for (const auto& entry : m_shadowViewCache)
				{
					if (entry.Second() && *entry.Second())
					{
						(*entry.Second())->m_packet.InvalidateUploadedState();
						(*entry.Second())->m_bUploadedThisSubmission = false;
					}
				}
			}

			TMap<uint64_t, TSharedPtr<ShadowViewResources>> m_shadowViewCache{};
			TVector<TSharedPtr<ShadowViewResources>> m_activeShadowViews{};
			TVector<std::array<size_t, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>>
				m_shadowPayloadRevisions{};
			TVector<std::array<bool, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>>
				m_buildShadowPayloads{};
			TVector<std::array<bool, RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>>
				m_shadowPayloadComplete{};
			uint32_t m_numActiveShadowViews = 0u;
			std::array<Framegraph::TextureDependencyCollector,
				RHI::TPackedDrawPacket<PerInstanceData>::NumMobilitySegments>
				m_requestedPacketTextures{};
			TVector<RHI::RHITexturePtr> m_renderPassColorAttachments{};
			TVector<RHI::RHIShaderBindingSetPtr> m_blurDrawBindingSets{};
			TVector<PerInstanceData> m_arenaRangeInstances{};
			TVector<uint64_t> m_arenaRangeStableKeys{};
			TVector<RHI::PackedDrawArenaMaterialRun> m_arenaRangeMaterialVersionRuns{};

			RHI::RHIShaderBindingSetPtr m_blurShaderBindings{};
		};

		ShaderSetPtr m_pBlurVerticalShader{};
		ShaderSetPtr m_pBlurHorizontalShader{};
		RHI::RHIMaterialPtr m_pBlurVerticalMaterial{};
		RHI::RHIMaterialPtr m_pBlurHorizontalMaterial{};
		RHI::RHIShaderBindingSetPtr m_pBlurShaderBindings{};

		// Shadow caster material
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_shadowMaterials_Evsm{};
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_shadowMaterials_Pcf{};
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_skinnedShadowMaterials_Evsm{};
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_skinnedShadowMaterials_Pcf{};
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_maskedShadowMaterials_Evsm{};
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_maskedShadowMaterials_Pcf{};
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_skinnedMaskedShadowMaterials_Evsm{};
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_skinnedMaskedShadowMaterials_Pcf{};
		struct CustomShadowMaterialCacheEntry
		{
			RHI::RHIMaterialVersionPtr m_sourceVersion{};
			RHI::RHIMaterialPtr m_material{};
		};
		TMap<size_t, CustomShadowMaterialCacheEntry> m_customShadowMaterials{};

		RHI::RHIMaterialPtr GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertex, RHI::EShadowType shadowType, bool bSkinned, bool bMasked);
		RHI::RHIMaterialPtr GetOrAddCustomShadowMaterial(
			const ShaderSetPtr& sourceShader,
			const RHI::RHIMaterialPtr& sourceMaterial,
			const RHI::RHIMaterialVersionPtr& sourceMaterialVersion,
			RHI::RHIVertexDescriptionPtr vertex,
			RHI::EShadowType shadowType,
			bool bMasked);

		Framegraph::TextureBindingCache m_textureBindingCache{};
		RHI::TPackedDrawPacketPayloadCache<PerInstanceData> m_packetPayloadCache{};
		RHI::TPackedDrawPagedArenaCache<PerInstanceData> m_pagedArenaCache{};

		SAILOR_SHARED_API static const char* m_name;
	};

	namespace Framegraph
	{
		template class TFrameGraphNode<ShadowPrepassNode>;
	}
};
