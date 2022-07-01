#pragma once
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"

namespace Sailor
{
	class RHIFrameGraph : public RHI::RHIResource
	{
	public:

		SAILOR_API RHIFrameGraph() = default;

		SAILOR_API TVector<FrameGraphNodePtr>& GetGraph() { return m_graph; }

		SAILOR_API void SetSampler(const std::string& name, TexturePtr sampler);
		SAILOR_API void SetRenderTarget(const std::string& name, RHI::RHITexturePtr sampler);

		SAILOR_API TexturePtr GetSampler(const std::string& name);
		SAILOR_API RHI::RHITexturePtr GetRenderTarget(const std::string& name);

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

		SAILOR_API void Process(RHI::RHISceneViewPtr rhiSceneView);

		SAILOR_API void Clear();

	protected:

		TMap<std::string, TexturePtr> m_samplers;
		TMap<std::string, RHI::RHITexturePtr> m_renderTargets;
		TMap<std::string, glm::vec4> m_values;
		TVector<FrameGraphNodePtr> m_graph;
	};

	using RHIFrameGraphPtr = TRefPtr<RHIFrameGraph>;
};
