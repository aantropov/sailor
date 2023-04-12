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

		// CSM
		static constexpr uint32_t MaxCSM = 2;
		static constexpr uint32_t NumCascades = 3;
		static constexpr float ShadowCascadeLevels[NumCascades] = { 1.0f / 15.0f, 1.0f / 5.0f, 1.0f / 2.0f };

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

		SAILOR_API static TVector<glm::mat4> CalculateLightSpaceMatrices(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float cameraNearPlane, float cameraFarPlane);
		SAILOR_API static glm::mat4 CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar);

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

		TVector<RHI::RHIRenderTargetPtr> m_csmShadowMaps;
		RHI::RHIRenderTargetPtr m_defaultShadowMap;

		static const char* m_name;
	};

	template class TFrameGraphNode<ShadowPrepassNode>;
};
