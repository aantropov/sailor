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

		SAILOR_API static TVector<glm::mat4> CalculateLightProjectionForCascades(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float cameraNearPlane, float cameraFarPlane);
		SAILOR_API static glm::mat4 CalculateLightProjectionMatrix(const glm::mat4& lightView, const glm::mat4& cameraWorld, float aspect, float fovY, float zNear, float zFar);

	protected:

		ShaderSetPtr m_pBlurVerticalShader{};
		ShaderSetPtr m_pBlurHorizontalShader{};
		RHI::RHIMaterialPtr m_pBlurVerticalMaterial{};
		RHI::RHIMaterialPtr m_pBlurHorizontalMaterial{};

		// Shadow caster material
		TMap<RHI::VertexAttributeBits, RHI::RHIMaterialPtr> m_shadowMaterials;
		RHI::RHIMaterialPtr GetOrAddShadowMaterial(RHI::RHIVertexDescriptionPtr vertex);

		// Record drawcalls
		size_t m_sizePerInstanceData = 0;
		RHI::RHIShaderBindingSetPtr m_perInstanceData;
		TVector<RHI::RHIBufferPtr> m_indirectBuffers;

		// Light matrices
		RHI::RHIShaderBindingPtr m_lightMatrices;

		static const char* m_name;
	};

	template class TFrameGraphNode<ShadowPrepassNode>;
};
