#pragma once
#include "Core/Defines.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "Math/Transform.h"
#include "Raytracing/PathTracer.h"

namespace Sailor::Framegraph
{
	class PathTracerPassNode : public TFrameGraphNode<PathTracerPassNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph,
			RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView) override;

		SAILOR_API virtual void Clear() override;

	protected:
		void ResetAccumulation();

		ShaderSetPtr m_pShader{};
		RHI::RHIMaterialPtr m_accumulateMaterial{};
		RHI::RHIMaterialPtr m_overlayMaterial{};
		RHI::RHIMaterialPtr m_overlayMaterialMsaa{};
		ShaderSetPtr m_pCubeToEquirectShader{};
		RHI::RHITexturePtr m_runtimeTexture{};
		RHI::RHITexturePtr m_currentFrameTexture{};
		RHI::RHITexturePtr m_environmentSpecularEquirectTexture{};
		RHI::RHITexturePtr m_environmentDiffuseEquirectTexture{};
		RHI::RHITexturePtr m_historyTextures[2]{};
		RHI::RHIBufferPtr m_uploadBuffer{};
		RHI::RHIBufferPtr m_environmentSpecularCaptureBuffer{};
		RHI::RHIBufferPtr m_environmentDiffuseCaptureBuffer{};
		RHI::RHIShaderBindingSetPtr m_shaderBindings{};
		RHI::RHIShaderBindingSetPtr m_cubeToEquirectBindings{};
		size_t m_uploadBufferSize = 0;
		glm::uvec2 m_environmentSpecularCaptureExtent{ 0u, 0u };
		glm::uvec2 m_environmentDiffuseCaptureExtent{ 0u, 0u };
		glm::uvec2 m_environmentEquirectExtent{ 0u, 0u };
		bool m_bHasPendingEnvironmentSpecularCapture = false;
		bool m_bHasPendingEnvironmentDiffuseCapture = false;
		uint64_t m_environmentSpecularCaptureSubmittedFrame = 0ull;
		uint64_t m_environmentDiffuseCaptureSubmittedFrame = 0ull;
		Raytracing::PathTracer m_pathTracer{};

		TVector<u8vec4> m_upload{};
		glm::uvec2 m_extent{ 0u, 0u };
		glm::uvec2 m_historyExtent{ 0u, 0u };
		uint32_t m_accumulatedFrames = 0;
		double m_accumulatedSampleWeight = 0.0;
		uint32_t m_historyReadIndex = 0;
		uint32_t m_idleFrames = 0;
		float m_qualityBudget = 0.6f;
		size_t m_sceneHash = 0;

		Math::Transform m_prevCamera{};
		bool m_bHasPrevCamera = false;

		static const char* m_name;
	};

	template class TFrameGraphNode<PathTracerPassNode>;
}
