#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"

namespace Sailor
{
	using RHIFrameGraphPtr = TRefPtr<class RHIFrameGraph>;

	// Don't derive from the base class, that is CRTP (Curiously recurrent template pattern)
	// You should derive from TFrameGraphNode<YourNodeType, "YourNodeName">
	class BaseFrameGraphNode : public RHI::RHIResource
	{
	public:

		SAILOR_API virtual ~BaseFrameGraphNode() = default;

		SAILOR_API void SetVectorParam(std::string name, const glm::vec4& value);
		SAILOR_API void SetResourceParam(std::string name, RHI::RHIResourcePtr value);

		SAILOR_API RHI::RHIResourcePtr GetResourceParam(std::string name) const;
		SAILOR_API glm::vec4 GetVectorParam(std::string name) const;

		SAILOR_API virtual void Initialize(RHIFrameGraphPtr frameGraph) = 0;
		SAILOR_API virtual void Process() = 0;
		SAILOR_API virtual void Clear() = 0;

	protected:

		TMap<std::string, glm::vec4> m_vectorParams;
		TMap<std::string, RHI::RHIResourcePtr> m_resourceParams;

		RHIFrameGraphPtr m_frameGraph{};
	};


	using FrameGraphNodePtr = TRefPtr<BaseFrameGraphNode>;
};
