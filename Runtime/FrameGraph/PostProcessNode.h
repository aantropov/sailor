#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"
#include "RHI/RenderSubmission.h"

namespace Sailor::Framegraph
{
	class PostProcessNode : public TFrameGraphNode<PostProcessNode>
	{
	public:
		SAILOR_API static StringHash GetName() { return "PostProcess"_h; }

		SAILOR_API void PreloadShader();
		SAILOR_API bool IsShaderReady() const;
		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
		SAILOR_API virtual void Clear() override;

	protected:
		friend class PostProcessNodeTestAccess;

		class SubmissionResources final : public RHI::RHIFrameGraphSubmissionResource
		{
		public:
			void ResetForSubmission() override {}
			void InvalidateSubmission() override { m_uploadedParameterRevision = 0; }

			RHI::RHIShaderBindingSetPtr m_shaderBindings{};
			uint64_t m_shaderGeneration = 0;
			uint64_t m_uploadedParameterRevision = 0;
		};

		ShaderSetPtr m_pShader{};
		RHI::RHIMaterialPtr m_postEffectMaterial{};
		std::string m_shaderPath;
		std::string m_shaderDefines;
		uint64_t m_shaderGeneration = 0;
	};

	template class TFrameGraphNode<PostProcessNode>;
};
