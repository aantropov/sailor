#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"

namespace Sailor
{
	using RHIFrameGraphPtr = TRefPtr<class RHIFrameGraph>;

	// Don't derive from the interface
	// You should derive from TFrameGraphNode<YourNodeType, "YourNodeName">
	class IBaseFrameGraphNode : public RHI::RHIResource
	{
	public:

		virtual ~IBaseFrameGraphNode() = default;

		void SetVectorParam(std::string name, const glm::vec4& value);
		void SetResourceParam(std::string name, RHI::RHIResourcePtr value);

		RHI::RHIResourcePtr GetResourceParam(std::string name) const;
		glm::vec4 GetVectorParam(std::string name) const;

		virtual void Initialize(RHIFrameGraphPtr frameGraph) = 0;
		virtual void Process() = 0;
		virtual void Clear() = 0;

	protected:

		TMap<std::string, glm::vec4> m_vectorParams;
		TMap<std::string, RHI::RHIResourcePtr> m_resourceParams;

		RHIFrameGraphPtr m_FrameGraph{};
	};


	using FrameGraphNodePtr = TRefPtr<IBaseFrameGraphNode>;
};
