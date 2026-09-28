#pragma once
#include "Core/Defines.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "Math/Transform.h"
#include "Memory/UniquePtr.hpp"
#include "Raytracing/PathTracer.h"
#include "RHI/RenderSubmission.h"

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
		};

		class SubmissionResources final : public RHI::RHIFrameGraphSubmissionResource
		{
		public:
			void ResetForSubmission() override {}
			void InvalidateSubmission() override { m_imageRevision = 0; }

			RHI::RHITexturePtr m_runtimeTexture{};
			RHI::RHIBufferPtr m_uploadBuffer{};
			RHI::RHIShaderBindingSetPtr m_shaderBindings{};
			uint64_t m_imageRevision = 0;
			CubemapReadbackState m_environment{};
			CubemapReadbackState m_diffuseEnvironment{};
			RHI::RHIFencePtr m_readbackCompletion{};
		};

		struct CameraState
		{
			Raytracing::PathTracer m_pathTracer{};
			TRefPtr<SubmissionResources> m_pendingReadback{};
			RHI::RHICubemapPtr m_environmentSource{};
			RHI::RHICubemapPtr m_diffuseEnvironmentSource{};
			uint64_t m_lastQueuedFrame = 0;
			TVector<glm::vec4> m_accumulatedImage{};
			uint64_t m_accumulatedSamples = 0;
			uint64_t m_imageRevision = 0;
			glm::uvec2 m_extent{ 0u, 0u };
			glm::vec3 m_lastCameraPosition{ 0.0f };
			glm::vec3 m_lastCameraForward{ 0.0f, 0.0f, -1.0f };
			glm::vec3 m_lastCameraUp{ 0.0f, 1.0f, 0.0f };
			float m_lastCameraAspect = 0.0f;
			float m_lastCameraHFov = 0.0f;
			bool m_bHasAccumulationState = false;
		};

		SAILOR_API CameraState& GetCameraState(uint32_t cameraIndex);
		SAILOR_API void AccumulateImage(CameraState& camera, const TVector<glm::vec4>& image, glm::uvec2 extent, uint32_t samples);
		SAILOR_API bool ApplyCompletedReadback(CameraState& camera,
			const RHI::RHICubemapPtr& environment, const RHI::RHICubemapPtr& diffuseEnvironment);
		void QueueEnvironmentReadback(CameraState& camera, TRefPtr<SubmissionResources> resources,
			RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView,
			RHI::RHICubemapPtr environment, RHI::RHICubemapPtr diffuseEnvironment);

		ShaderSetPtr m_pShader{};
		RHI::RHIMaterialPtr m_overlayMaterial{};
		RHI::RHIMaterialPtr m_overlayMaterialMsaa{};
		TMap<uint32_t, TUniquePtr<CameraState>> m_cameras;
		uint32_t m_lastCameraIndex = 0;
		uint64_t m_nextImageRevision = 0;

		static const char* m_name;
	};

	template class TFrameGraphNode<CPUPathTracerNode>;
}
