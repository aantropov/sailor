#pragma once
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "RHI/Texture.h"
#include "FrameGraph/BaseFrameGraphNode.h"

namespace Sailor
{
	class RHIFrameGraph : public RHI::RHIResource
	{
	public:

		RHIFrameGraph() = default;

		TVector<FrameGraphNodePtr>& GetGraph() { return m_graph; }

		void SetSampler(const std::string& name, RHI::RHITexturePtr sampler);
		
		template<typename T>
		void SetValue(const std::string& name, T value)
		{
			m_values[name] = glm::vec4(1) * value;
		}

		template<>
		void SetValue<glm::vec4>(const std::string& name, glm::vec4 value)
		{
			m_values[name] = value;
		}

		void Process();

		void Clear();

	protected:

		TMap<std::string, RHI::RHITexturePtr> m_samplers;
		TMap<std::string, glm::vec4> m_values;
		TVector<FrameGraphNodePtr> m_graph;
	};

	using RHIFrameGraphPtr = TRefPtr<RHIFrameGraph>;
};
