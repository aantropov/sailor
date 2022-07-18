#include "RHIFrameGraph.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"

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

void RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView, TVector<RHI::RHICommandListPtr>& outCommandLists)
{
	auto snapshots = rhiSceneView->GetSnapshots();

	for (auto& snapshot : snapshots)
	{
		SAILOR_PROFILE_BLOCK("Render meshes");
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
