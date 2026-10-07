#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"

namespace Sailor::Framegraph
{
	// Derive from TFrameGraphNode<YourNodeType> and return a StringHash from GetName().
	class BaseFrameGraphNode : public RHI::RHIResource
	{
	public:

		SAILOR_API virtual ~BaseFrameGraphNode() = default;

		SAILOR_API void SetString(StringHash name, std::string_view value);
		SAILOR_API void SetVec4(StringHash name, const glm::vec4& value);
		SAILOR_API virtual void SetFloat(StringHash name, float value);
		SAILOR_API void SetRHIResource(StringHash name, RHI::RHIResourcePtr value);

		// Used to resolve render resources during frame recording
		SAILOR_API void SetRHIResource_Unresolved(StringHash name, StringHash value);

		SAILOR_API RHI::RHITexturePtr GetResolvedAttachment(StringHash name, const RHI::RHIFrameGraph* frameGraph = nullptr) const;
		SAILOR_API RHI::RHITexturePtr GetTargetAttachment(StringHash name, const RHI::RHIFrameGraph* frameGraph = nullptr) const;
		SAILOR_API RHI::RHITexturePtr GetSampledAttachment(StringHash name, const RHI::RHIFrameGraph* frameGraph = nullptr) const;
		SAILOR_API RHI::RHIResourcePtr GetRHIResource(StringHash name, const RHI::RHIFrameGraph* frameGraph = nullptr) const;
		SAILOR_API const glm::vec4& GetVec4(StringHash name) const;
		SAILOR_API float GetFloat(StringHash name) const;
		SAILOR_API const std::string& GetString(StringHash name) const;
		SAILOR_API bool TryGetString(StringHash name, std::string& value) const;
		// The view borrows node storage; do not retain it across parameter changes.
		SAILOR_API bool TryGetString(StringHash name, std::string_view& value) const;

		// Called in graph order before scheduling the returned recording tasks.
		SAILOR_API virtual Sailor::Tasks::TaskPtr<void, void> Prepare(RHI::RHIFrameGraphPtr frameGraph, RHI::RHISceneViewSnapshot& sceneView) { return Sailor::Tasks::TaskPtr<void, void>(); }
		SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) = 0;
		SAILOR_API virtual void Clear() = 0;
		SAILOR_API virtual RHI::DrawCallStats GetDrawCallStats() const { return m_drawCallStats; }

		SAILOR_API StringHash GetTag() const { return m_tag; }
		SAILOR_API void SetTag(StringHash tag) { m_tag = tag; m_gpuTimingName = {}; }
		SAILOR_API StringHash GetGpuTimingName(size_t nodeIndex);

	protected:
		friend class RHI::RHIFrameGraph;

		void ResetDrawCallStats() { m_drawCallStats = {}; }
		void RecordDrawCallStats(uint32_t numInstances)
		{
			m_drawCallStats.m_numBatches++;
			m_drawCallStats.m_numInstances += numInstances;
		}

		TMap<StringHash, std::string> m_stringParams;
		TMap<StringHash, glm::vec4> m_vectorParams;
		TMap<StringHash, float> m_floatParams;
		TMap<StringHash, RHI::RHIResourcePtr> m_resourceParams;
		TMap<StringHash, StringHash> m_unresolvedResourceParams;
		uint64_t m_parameterRevision = 0;
		uint64_t m_resourceRevision = 0;
		RHI::DrawCallStats m_drawCallStats{};

		StringHash m_tag{};
		StringHash m_gpuTimingName{};
		size_t m_gpuTimingNodeIndex = 0;
	};

	using FrameGraphNodePtr = TRefPtr<BaseFrameGraphNode>;
};
