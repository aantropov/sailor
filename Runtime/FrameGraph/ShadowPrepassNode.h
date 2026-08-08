#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
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
			glm::vec4 baseColorFactor{ 1.0f };
			uint32_t baseColorSampler = 0;
			float alphaCutoff = 0.5f;
			uint32_t maskedPadding = 0;
			uint32_t stridePadding = 0;

			bool operator==(const PerInstanceData& rhs) const
			{
				return model == rhs.model &&
					materialInstance == rhs.materialInstance &&
					baseColorFactor == rhs.baseColorFactor &&
					skeletonOffset == rhs.skeletonOffset &&
					baseColorSampler == rhs.baseColorSampler &&
					alphaCutoff == rhs.alphaCutoff;
			}

			size_t GetHash() const
			{
				hash<glm::mat4> p;
				return p(model);
			}
		};

		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandLists, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

		SAILOR_API static TVector<glm::mat4> CalculateLightProjectionForCascades(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float cameraNearPlane, float cameraFarPlane);
		SAILOR_API static glm::mat4 CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar, float zMult, glm::ivec2 shadowMapResolution = glm::ivec2(0), float zSourceExtension = 0.0f);

	protected:

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
		TMap<size_t, RHI::RHIMaterialPtr> m_customShadowMaterials{};

		RHI::RHIMaterialPtr GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertex, RHI::EShadowType shadowType, bool bSkinned, bool bMasked);
		RHI::RHIMaterialPtr GetOrAddCustomShadowMaterial(
			const MaterialPtr& sourceMaterial,
			RHI::RHIVertexDescriptionPtr vertex,
			RHI::EShadowType shadowType,
			bool bMasked);

		// Record drawcalls
		size_t m_sizePerInstanceData = 0;
		RHI::RHIShaderBindingSetPtr m_perInstanceData{};
		TVector<RHI::RHIBufferPtr> m_indirectBuffers{};

		// Light matrices
		RHI::RHIShaderBindingPtr m_lightMatrices{};
		Framegraph::TextureBindingCache m_textureBindingCache{};

		static const char* m_name;
	};

	namespace Framegraph
	{
		template class TFrameGraphNode<ShadowPrepassNode>;
	}
};
