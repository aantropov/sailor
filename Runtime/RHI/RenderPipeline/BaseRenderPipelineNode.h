#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/RenderPipeline/RenderPipeline.h"

namespace Sailor::RHI
{
	// Don't derive from the interface
	// You should derive from TRHIRenderPipelineNode<YourNodeType, "YourNodeName">
	class IBaseRenderPipelineNode : public RHIResource
	{
	public:

		IBaseRenderPipelineNode() = default;
		virtual ~IBaseRenderPipelineNode() = default;

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
	};


	using RHIRenderPipelineNodePtr = TRefPtr<IBaseRenderPipelineNode>;
};
