#include "RHIFrameGraph.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Tasks/Tasks.h"

using namespace Sailor;
using namespace Sailor::RHI;

void RHIFrameGraph::Clear()
{
	m_samplers.Clear();
	m_graph.Clear();
	m_values.Clear();
	m_renderTargets.Clear();
	m_surfaces.Clear();
}

void RHIFrameGraph::SetSampler(const std::string& name, TexturePtr sampler)
{
	m_samplers[name] = sampler;
}

void RHIFrameGraph::SetRenderTarget(const std::string& name, RHI::RHITexturePtr sampler)
{
	m_renderTargets[name] = sampler;
}

void RHIFrameGraph::SetSurface(const std::string& name, RHI::RHISurfacePtr surface)
{
	m_surfaces[name] = surface;
}

void RHIFrameGraph::FillFrameData(RHI::RHICommandListPtr transferCmdList, RHI::RHISceneViewSnapshot& snapshot, float deltaTime, float worldTime) const
{
	SAILOR_PROFILE_FUNCTION();

	RHI::UboFrameData frameData;

	snapshot.m_frameBindings = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(snapshot.m_frameBindings, "frameData", sizeof(RHI::UboFrameData), 0, RHI::EShaderBindingType::UniformBuffer);

	/* TODO: Add that for post processes only
	for (auto& sampler : m_samplers)
	{
		// TODO: Bind sampler by name
		Sailor::RHI::Renderer::GetDriver()->AddSamplerToShaderBindings(snapshot.m_frameBindings, sampler.m_first, sampler.m_second->GetRHI(), 1);
	}*/

	frameData.m_cameraPosition = snapshot.m_cameraPosition;
	frameData.m_projection = snapshot.m_camera->GetProjectionMatrix();
	frameData.m_currentTime = worldTime;
	frameData.m_deltaTime = deltaTime;
	frameData.m_view = snapshot.m_camera->GetViewMatrix();

	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList, snapshot.m_frameBindings->GetOrAddShaderBinding("frameData"), &frameData, sizeof(frameData));
}

void RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView, TVector<RHI::RHICommandListPtr>& outTransferCommandLists, TVector<RHI::RHICommandListPtr>& outCommandLists)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto driverCommands = renderer->GetDriverCommands();

	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		SAILOR_PROFILE_BLOCK("FrameGraph");
		auto cmdList = renderer->GetDriver()->CreateCommandList(false, false);
		auto transferCmdList = renderer->GetDriver()->CreateCommandList(false, true);

		driverCommands->BeginCommandList(cmdList, true);
		driverCommands->BeginCommandList(transferCmdList, true);
		FillFrameData(transferCmdList, snapshot, rhiSceneView->m_deltaTime, rhiSceneView->m_currentTime);
		
		driverCommands->SetDefaultViewport(cmdList);

		for (auto& node : m_graph)
		{
			node->Process(this, transferCmdList, cmdList, snapshot);
			//TODO: Submit Transfer command lists
		}

		driverCommands->EndCommandList(cmdList);
		driverCommands->EndCommandList(transferCmdList);
		SAILOR_PROFILE_END_BLOCK();

		outCommandLists.Emplace(std::move(cmdList));
		outTransferCommandLists.Emplace(transferCmdList);
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

RHI::RHISurfacePtr RHIFrameGraph::GetSurface(const std::string& name)
{
	if (!m_surfaces.ContainsKey(name))
	{
		return nullptr;
	}

	return m_surfaces[name];
}