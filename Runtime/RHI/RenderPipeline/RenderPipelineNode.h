#pragma once
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/RenderPipeline/RenderPipeline.h"

namespace Sailor::RHI
{
	class RHIRenderPipelineNode : RHIResource
	{
	public:

		RHIRenderPipelineNode(const std::string& name) : m_name(name) {}

		void SetVectorParam(std::string name, const glm::vec4& value);
		void SetResourceParam(std::string name, RHIResourcePtr value);

		RHIResourcePtr GetResourceParam(std::string name) const;
		glm::vec4 GetVectorParam(std::string name) const;

		virtual void Initialize(RHIRenderPipelinePtr renderPipeline) = 0;
		virtual void Process() = 0;
		virtual void Clear() = 0;

	protected:

		TMap<std::string, glm::vec4> m_vectorParams;
		TMap<std::string, RHIResourcePtr> m_resourceParams;

		RHIRenderPipelinePtr m_renderPipeline{};

		std::string m_name;
	};

	using RHIRenderPipelineNodePtr = TRefPtr<RHIRenderPipelineNode>;
};
