#include "RHIFrameGraph.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Tasks/Tasks.h"

using namespace Sailor;

void RHIFrameGraph::Clear()
{
	m_samplers.Clear();
	m_graph.Clear();
	m_values.Clear();
}

void RHIFrameGraph::SetSampler(const std::string& name, TexturePtr sampler)
{
	m_samplers[name] = sampler;
}

void RHIFrameGraph::SetRenderTarget(const std::string& name, RHI::RHITexturePtr sampler)
{
	m_renderTargets[name] = sampler;
}

void RHIFrameGraph::FillFrameData(RHI::RHISceneViewSnapshot& snapshot, float deltaTime, float worldTime) const
{
	SAILOR_PROFILE_FUNCTION();

	RHI::UboFrameData frameData;

	snapshot.m_frameBindings = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(snapshot.m_frameBindings, "frameData", sizeof(RHI::UboFrameData), 0, RHI::EShaderBindingType::UniformBuffer);

	frameData.m_projection = snapshot.m_camera->GetProjectionMatrix();
	frameData.m_currentTime = worldTime;
	frameData.m_deltaTime = deltaTime;
	frameData.m_view = snapshot.m_camera->GetViewMatrix();

	// Update GPU side
	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto driverCommands = renderer->GetDriverCommands();
	RHI::RHICommandListPtr cmdList = App::GetSubmodule<RHI::Renderer>()->GetDriver()->CreateCommandList(false, true);
	driverCommands->BeginCommandList(cmdList, true);
	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(cmdList, snapshot.m_frameBindings->GetOrCreateShaderBinding("frameData"), &frameData, sizeof(frameData));
	driverCommands->EndCommandList(cmdList);

	renderer->GetDriver()->SubmitCommandList_Immediate(cmdList);
}

void RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView, TVector<RHI::RHICommandListPtr>& outCommandLists)
{
	SAILOR_PROFILE_FUNCTION();

	auto snapshots = rhiSceneView->GetSnapshots();

	for (auto& snapshot : snapshots)
	{
		FillFrameData(snapshot, rhiSceneView->m_deltaTime, rhiSceneView->m_currentTime);

		SAILOR_PROFILE_BLOCK("FrameGraph");
		auto renderer = App::GetSubmodule<RHI::Renderer>();
		auto driverCommands = renderer->GetDriverCommands();
		auto cmdList = renderer->GetDriver()->CreateCommandList(true, false);
		driverCommands->BeginCommandList(cmdList, true);

		for (auto& node : m_graph)
		{
			node->Process(cmdList, snapshot);
		}

		driverCommands->EndCommandList(cmdList);
		SAILOR_PROFILE_END_BLOCK();

		outCommandLists.Emplace(std::move(cmdList));
	}
}

TexturePtr RHIFrameGraph::GetSampler(const std::string& name)
{
	if (!m_renderTargets.ContainsKey(name))
	{
		return TexturePtr();
	}

	return m_samplers[name];
}

RHI::RHITexturePtr RHIFrameGraph::GetRenderTarget(const std::string& name)
{
	if (!m_renderTargets.ContainsKey(name))
	{
		return nullptr;
	}

	return m_renderTargets[name];
}
