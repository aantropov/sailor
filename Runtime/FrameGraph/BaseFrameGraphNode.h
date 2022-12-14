#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"

namespace Sailor::Framegraph
{
	// Don't derive from the base class, that is CRTP (Curiously recurrent template pattern)
	// You should derive from TFrameGraphNode<YourNodeType, "YourNodeName">
	class BaseFrameGraphNode : public RHI::RHIResource
	{
	public:

		SAILOR_API virtual ~BaseFrameGraphNode() = default;

		SAILOR_API void SetString(const std::string& name, const std::string& value);
		SAILOR_API void SetVec4(const std::string& name, const glm::vec4& value);
		SAILOR_API void SetRHIResource(const std::string& name, RHI::RHIResourcePtr value);

		SAILOR_API RHI::RHITexturePtr GetResolvedAttachment(const std::string& name) const;
		SAILOR_API RHI::RHIResourcePtr GetRHIResource(const std::string& name) const;
		SAILOR_API const glm::vec4& GetVec4(const std::string& name) const;
		SAILOR_API const std::string& GetString(const std::string& name) const;

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) = 0;
		SAILOR_API virtual void Clear() = 0;

		SAILOR_API const std::string& GetTag() const { return m_tag; }
		SAILOR_API void SetTag(const std::string& tag) { m_tag = tag; }

	protected:

		TMap<std::string, std::string> m_stringParams;
		TMap<std::string, glm::vec4> m_vectorParams;
		TMap<std::string, RHI::RHIResourcePtr> m_resourceParams;

		std::string m_tag{};
	};

	using FrameGraphNodePtr = TRefPtr<BaseFrameGraphNode>;
};
