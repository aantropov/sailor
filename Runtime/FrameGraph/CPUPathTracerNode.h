#pragma once
#include "Core/Defines.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "Math/Transform.h"
#include "Raytracing/PathTracer.h"

namespace Sailor::Framegraph
{
	class CPUPathTracerNode : public TFrameGraphNode<CPUPathTracerNode>
	{
	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph,
			RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView) override;

		SAILOR_API virtual void Clear() override;
		SAILOR_API bool GetLastRenderedImage(TVector<glm::u8vec4>& outImage, glm::uvec2& outExtent) const;

	protected:
		struct CubemapReadbackState
		{
			RHI::RHICubemapPtr m_source{};
			TVector<RHI::RHIBufferPtr> m_faceBuffers{};
			glm::uvec2 m_extent{ 0u, 0u };
			uint32_t m_mipLevel = 0u;
			uint64_t m_lastQueuedFrame = 0ull;
			bool m_bPendingGpuReadback = false;
		};

		ShaderSetPtr m_pShader{};
		RHI::RHIMaterialPtr m_overlayMaterial{};
		RHI::RHIMaterialPtr m_overlayMaterialMsaa{};
		RHI::RHITexturePtr m_runtimeTexture{};
		RHI::RHITexturePtr m_currentFrameTexture{};
		RHI::RHIBufferPtr m_uploadBuffer{};
		RHI::RHIShaderBindingSetPtr m_shaderBindings{};
		Raytracing::PathTracer m_pathTracer{};
		CubemapReadbackState m_environmentReadback{};
		CubemapReadbackState m_diffuseEnvironmentReadback{};
		TVector<glm::vec4> m_accumulatedImage{};
		TVector<glm::u8vec4> m_accumulatedDisplayImage{};
		uint64_t m_accumulatedSamples = 0ull;
		glm::vec3 m_lastCameraPosition{ 0.0f };
		glm::vec3 m_lastCameraForward{ 0.0f, 0.0f, -1.0f };
		glm::vec3 m_lastCameraUp{ 0.0f, 1.0f, 0.0f };
		float m_lastCameraAspect = 0.0f;
		float m_lastCameraHFov = 0.0f;
		bool m_bHasAccumulationState = false;

		glm::uvec2 m_extent{ 0u, 0u };

		static const char* m_name;
	};

	template class TFrameGraphNode<CPUPathTracerNode>;
}
