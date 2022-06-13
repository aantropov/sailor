#pragma once
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "FrameGraph/BaseFrameGraphNode.h"

namespace Sailor
{
	class RHIFrameGraph : public RHI::RHIResource
	{
	public:

		RHIFrameGraph() = default;

		void Initialize(const nlohmann::json& FrameGraphAsset);
		void Process();

	protected:

		TVector<RHI::RHITexturePtr> m_resources;
		TConcurrentMap<std::string, glm::vec4> m_values;
		TConcurrentMap<std::string, FrameGraphNodePtr> m_graph;
	};

	using RHIFrameGraphPtr = TRefPtr<RHIFrameGraph>;
};
