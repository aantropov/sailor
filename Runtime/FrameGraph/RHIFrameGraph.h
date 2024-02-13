#pragma once
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "Tasks/Tasks.h"

using namespace Sailor::Framegraph;

namespace Sailor::RHI
{
	class RHIFrameGraph : public RHI::RHIResource
	{
	public:

		// TODO: Ideally we would like to track performance
		// and make the decisions on the CPU/GPU frame time
		const uint32_t MaxRecordedCommands = 350;
		const uint32_t MaxGpuCost = 650;

		SAILOR_API RHIFrameGraph();
		SAILOR_API ~RHIFrameGraph();

		SAILOR_API FrameGraphNodePtr GetGraphNode(const std::string& tag);
		SAILOR_API TVector<FrameGraphNodePtr>& GetGraph() { return m_graph; }

		SAILOR_API void SetSampler(const std::string& name, RHI::RHITexturePtr sampler);
		SAILOR_API void SetRenderTarget(const std::string& name, RHI::RHIRenderTargetPtr sampler);
		SAILOR_API void SetSurface(const std::string& name, RHI::RHISurfacePtr surface);

		SAILOR_API RHI::RHITexturePtr GetSampler(const std::string& name);
		SAILOR_API RHI::RHIRenderTargetPtr GetRenderTarget(const std::string& name);
		SAILOR_API RHI::RHISurfacePtr GetSurface(const std::string& name);

		SAILOR_API RHI::RHIMeshPtr GetFullscreenNdcQuad() { return m_postEffectPlane; }

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

		SAILOR_API TVector<Sailor::Tasks::TaskPtr<void,void>> Prepare(RHI::RHISceneViewPtr rhiSceneView);

		SAILOR_API void Process(RHI::RHISceneViewPtr rhiSceneView, 
			TVector<RHI::RHICommandListPtr>& outTransferCommandLists, 
			TVector<RHI::RHICommandListPtr>& outSecondaryCommandLists,
			RHISemaphorePtr& outWaitSemaphore);

		SAILOR_API void Clear();

	protected:

		void FillFrameData(RHI::RHICommandListPtr transferCmdList, RHI::RHISceneViewSnapshot& snapshot, float deltaTime, float worldTime) const;

		TMap<std::string, RHI::RHITexturePtr> m_samplers;
		TMap<std::string, RHI::RHIRenderTargetPtr> m_renderTargets;
		TMap<std::string, RHI::RHISurfacePtr> m_surfaces;
		TMap<std::string, glm::vec4> m_values;
		TVector<Framegraph::FrameGraphNodePtr> m_graph;

		RHI::RHIMeshPtr m_postEffectPlane;
	};

	using RHIFrameGraphPtr = TRefPtr<RHIFrameGraph>;
};
