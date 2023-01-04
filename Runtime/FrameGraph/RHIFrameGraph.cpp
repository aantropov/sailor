#include "RHIFrameGraph.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/VertexDescription.h"
#include "RHI/CommandList.h"
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

FrameGraphNodePtr RHIFrameGraph::GetGraphNode(const std::string& tag)
{
	const size_t index = m_graph.FindIf([&](const auto& lhs) { return lhs->GetTag() == tag; });
	if (index != -1)
	{
		return m_graph[index];
	}

	return nullptr;
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
	frameData.m_invProjection = snapshot.m_camera->GetInvProjection();
	frameData.m_currentTime = worldTime;
	frameData.m_deltaTime = deltaTime;
	frameData.m_view = snapshot.m_camera->GetViewMatrix();
	frameData.m_viewportSize = glm::ivec2(App::GetViewportWindow()->GetWidth(), App::GetViewportWindow()->GetHeight());

	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList, snapshot.m_frameBindings->GetOrAddShaderBinding("frameData"), &frameData, sizeof(frameData));
}

void RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView, TVector<RHI::RHICommandListPtr>& outTransferCommandLists, TVector<RHI::RHICommandListPtr>& outCommandLists)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto driverCommands = renderer->GetDriverCommands();

	if (!m_postEffectPlane)
	{
		m_postEffectPlane = renderer->GetDriver()->CreateMesh();
		m_postEffectPlane->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3UV2C4>();
		m_postEffectPlane->m_bounds = Math::AABB(vec3(0), vec3(1, 1, 1));

		TVector<VertexP3N3UV2C4> ndcQuad(4);
		ndcQuad[0].m_texcoord = vec2(0.0f, 0.0f);
		ndcQuad[1].m_texcoord = vec2(1.0f, 0.0f);
		ndcQuad[2].m_texcoord = vec2(0.0f, 1.0f);
		ndcQuad[3].m_texcoord = vec2(1.0f, 1.0f);

		ndcQuad[0].m_position = vec3(-1.0f, -1.0f, 0.0f);
		ndcQuad[1].m_position = vec3(1.0f, -1.0f, 0.0f);
		ndcQuad[2].m_position = vec3(-1.0f, 1.0f, 0.0f);
		ndcQuad[3].m_position = vec3(1.0f, 1.0f, 0.0f);

		const TVector<uint32_t> indices = { 0, 1, 2, 2, 1, 3 };

		RHI::Renderer::GetDriver()->UpdateMesh(m_postEffectPlane, &ndcQuad[0], ndcQuad.Num() * sizeof(VertexP3N3UV2C4), &indices[0], sizeof(uint32_t) * indices.Num());
	}

	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		SAILOR_PROFILE_BLOCK("FrameGraph");
		auto cmdList = renderer->GetDriver()->CreateCommandList(false, false);
		auto transferCmdList = renderer->GetDriver()->CreateCommandList(false, true);

		RHI::Renderer::GetDriver()->SetDebugName(cmdList, "FrameGraph:Graphics");
		RHI::Renderer::GetDriver()->SetDebugName(transferCmdList, "FrameGraph:Transfer");

		driverCommands->BeginCommandList(cmdList, true);
		driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics", glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));

		driverCommands->BeginCommandList(transferCmdList, true);
		driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer", glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));

		FillFrameData(transferCmdList, snapshot, rhiSceneView->m_deltaTime, rhiSceneView->m_currentTime);

		driverCommands->SetDefaultViewport(cmdList);

		for (auto& node : m_graph)
		{
			node->Process(this, transferCmdList, cmdList, snapshot);
			//TODO: Submit Transfer command lists
		}

		driverCommands->EndDebugRegion(cmdList);
		driverCommands->EndCommandList(cmdList);

		driverCommands->EndDebugRegion(transferCmdList);
		driverCommands->EndCommandList(transferCmdList);
		SAILOR_PROFILE_END_BLOCK();

		outCommandLists.Emplace(std::move(cmdList));
		outTransferCommandLists.Emplace(transferCmdList);
	}
}

TexturePtr RHIFrameGraph::GetSampler(const std::string& name)
{
	if (!m_samplers.ContainsKey(name))
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