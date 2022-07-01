#include "RHIFrameGraph.h"
#include "RHI/SceneView.h"

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

void RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView)
{
	auto snapshots = rhiSceneView->GetSnapshots();

	for (auto& snapshot : snapshots)
	{
		for (auto& node : m_graph)
		{
			node->Process(snapshot);
		}
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
