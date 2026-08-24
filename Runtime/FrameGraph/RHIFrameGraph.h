#pragma once
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "Tasks/Tasks.h"

using namespace Sailor::Framegraph;

namespace Sailor::RHI
{
	struct RHIGlobalIlluminationRenderStats;

	class RHIFrameGraph : public RHI::RHIResource
	{
	public:

		// TODO: Ideally we would like to track performance
		// and make the decisions on the CPU/GPU frame time
		const uint32_t MaxRecordedCommands = 350;
		const uint32_t MaxGpuCost = 650;

		SAILOR_API RHIFrameGraph() = default;
		SAILOR_API virtual ~RHIFrameGraph() = default;

		SAILOR_API FrameGraphNodePtr GetGraphNode(const std::string& tag);
		SAILOR_API TVector<FrameGraphNodePtr>& GetGraph() { return m_graph; }

		SAILOR_API void SetSampler(const std::string& name, RHI::RHITexturePtr sampler);
		SAILOR_API void SetRenderTarget(const std::string& name, RHI::RHIRenderTargetPtr sampler);
		SAILOR_API void SetSurface(const std::string& name, RHI::RHISurfacePtr surface);

		SAILOR_API RHI::RHITexturePtr GetSampler(const std::string& name);
		SAILOR_API RHI::RHIRenderTargetPtr GetRenderTarget(const std::string& name);
		SAILOR_API RHI::RHISurfacePtr GetSurface(const std::string& name);
		SAILOR_API glm::ivec2 GetSceneRenderExtent();

		SAILOR_API RHI::RHIMeshPtr GetFullscreenNdcQuad() { return m_postEffectPlane; }
		SAILOR_API RHI::DrawCallStats GetDrawCallStats() const { return m_drawCallStats; }
		SAILOR_API RHIGlobalIlluminationRenderStats
			GetGlobalIlluminationRenderStats() const;

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

		SAILOR_API TVector<Sailor::Tasks::TaskPtr<void, void>> Prepare(RHI::RHISceneViewPtr rhiSceneView);

		SAILOR_API bool Process(RHI::RHISceneViewPtr rhiSceneView,
			TVector<RHI::RHICommandListPtr>& outTransferCommandLists,
			TVector<RHI::RHICommandListPtr>& outSecondaryCommandLists,
			RHISemaphorePtr inSignalSemaphore,
			RHISemaphorePtr& outWaitSemaphore);

		SAILOR_API void Clear();

	protected:

		RHI::UboFrameData FillFrameData(RHI::RHICommandListPtr transferCmdList, RHI::RHISceneViewSnapshot& snapshot, const RHI::UboFrameData& previousFrame, float deltaTime, float worldTime);

		TMap<std::string, RHI::RHITexturePtr> m_samplers;
		TMap<std::string, RHI::RHIRenderTargetPtr> m_renderTargets;
		TMap<std::string, RHI::RHISurfacePtr> m_surfaces;
		TMap<std::string, glm::vec4> m_values;
		TVector<Framegraph::FrameGraphNodePtr> m_graph;

		RHI::RHIMeshPtr m_postEffectPlane;

		// TODO: Store/Handle that per snapshot
		RHI::UboFrameData m_frameData{};
		RHI::UboFrameData m_prevFrameData{};

		GpuStats m_lastFrameGpuStats{};
		RHI::DrawCallStats m_drawCallStats{};
	};

	using RHIFrameGraphPtr = TRefPtr<RHIFrameGraph>;
};
