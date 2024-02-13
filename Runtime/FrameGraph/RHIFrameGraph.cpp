#include "RHIFrameGraph.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/VertexDescription.h"
#include "RHI/RenderTarget.h"
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

void RHIFrameGraph::SetSampler(const std::string& name, RHI::RHITexturePtr sampler)
{
	m_samplers[name] = sampler;
}

void RHIFrameGraph::SetRenderTarget(const std::string& name, RHI::RHIRenderTargetPtr sampler)
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

	frameData.m_cameraPosition = snapshot.m_cameraTransform.m_position;
	frameData.m_projection = snapshot.m_camera->GetProjectionMatrix();
	frameData.m_invProjection = snapshot.m_camera->GetInvProjection();
	frameData.m_cameraZNearZFar = vec2(snapshot.m_camera->GetZNear(), snapshot.m_camera->GetZFar());
	frameData.m_currentTime = worldTime;
	frameData.m_deltaTime = deltaTime;
	frameData.m_view = snapshot.m_camera->GetViewMatrix();
	frameData.m_viewportSize = glm::ivec2(App::GetMainWindow()->GetRenderArea().x, App::GetMainWindow()->GetRenderArea().y);

	RHI::Renderer::GetDriverCommands()->UpdateShaderBinding(transferCmdList, snapshot.m_frameBindings->GetOrAddShaderBinding("frameData"), &frameData, sizeof(frameData));
}

TVector<Sailor::Tasks::TaskPtr<void, void>> RHIFrameGraph::Prepare(RHI::RHISceneViewPtr rhiSceneView)
{
	TVector<Sailor::Tasks::TaskPtr<void, void>> res;

	auto frameRefPtr = this->ToRefPtr<RHIFrameGraph>();
	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		for (auto& node : m_graph)
		{
			auto task = node->Prepare(frameRefPtr, snapshot);
			if (task.IsValid())
			{
				res.Emplace(std::move(task));
			}
		}
	}

	return res;
}

void RHIFrameGraph::Process(RHI::RHISceneViewPtr rhiSceneView,
	TVector<RHI::RHICommandListPtr>& outTransferCommandLists,
	TVector<RHI::RHICommandListPtr>& outCommandLists,
	RHISemaphorePtr& outWaitSemaphore)
{
	SAILOR_PROFILE_FUNCTION();

	auto renderer = App::GetSubmodule<RHI::Renderer>();
	auto& driver = RHI::Renderer::GetDriver();
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

	bool bShouldRecalculateCompatibility = false;
	if (auto g_irradianceCubemap = GetSampler("g_irradianceCubemap"))
	{
		if (g_irradianceCubemap != rhiSceneView->m_rhiLightsData->GetOrAddShaderBinding("g_irradianceCubemap")->GetTextureBinding())
		{
			renderer->GetDriver()->AddSamplerToShaderBindings(rhiSceneView->m_rhiLightsData, "g_irradianceCubemap", g_irradianceCubemap, 3);
			bShouldRecalculateCompatibility = true;
		}
	}

	if (auto g_brdfSampler = GetSampler("g_brdfSampler"))
	{
		if (g_brdfSampler != rhiSceneView->m_rhiLightsData->GetOrAddShaderBinding("g_brdfSampler")->GetTextureBinding())
		{
			renderer->GetDriver()->AddSamplerToShaderBindings(rhiSceneView->m_rhiLightsData, "g_brdfSampler", g_brdfSampler, 4);
			bShouldRecalculateCompatibility = true;
		}
	}

	if (auto g_envCubemap = GetSampler("g_envCubemap"))
	{
		if (g_envCubemap != rhiSceneView->m_rhiLightsData->GetOrAddShaderBinding("g_envCubemap")->GetTextureBinding())
		{
			renderer->GetDriver()->AddSamplerToShaderBindings(rhiSceneView->m_rhiLightsData, "g_envCubemap", g_envCubemap, 5);
			bShouldRecalculateCompatibility = true;
		}
	}

	// TODO: Move to another place
	if (auto g_aoSampler = GetRenderTarget("g_AO"))
	{
		if (g_aoSampler != rhiSceneView->m_rhiLightsData->GetOrAddShaderBinding("g_aoSampler")->GetTextureBinding())
		{
			renderer->GetDriver()->AddSamplerToShaderBindings(rhiSceneView->m_rhiLightsData, "g_aoSampler", g_aoSampler, 9);
			bShouldRecalculateCompatibility = true;
		}
	}

	if (bShouldRecalculateCompatibility)
	{
		rhiSceneView->m_rhiLightsData->RecalculateCompatibility();
	}

	for (auto& snapshot : rhiSceneView->m_snapshots)
	{
		SAILOR_PROFILE_BLOCK("FrameGraph");

		auto cmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
		auto transferCmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Compute);

		driver->SetDebugName(cmdList, "FrameGraph:Graphics");
		driver->SetDebugName(transferCmdList, "FrameGraph:Transfer");

		driverCommands->BeginCommandList(cmdList, true);
		driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics", glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));

		driverCommands->BeginCommandList(transferCmdList, true);
		driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer", glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));

		driverCommands->BeginDebugRegion(transferCmdList, "Fill Frame Data", DebugContext::Color_CmdTransfer);
		{
			FillFrameData(transferCmdList, snapshot, rhiSceneView->m_deltaTime, rhiSceneView->m_currentTime);
		}
		driverCommands->EndDebugRegion(transferCmdList);

		RHI::RHISemaphorePtr chainSemaphore{};

		TVector<Tasks::ITaskPtr> tasks;
		tasks.Reserve(2);

		auto frameRefPtr = this->ToRefPtr<RHIFrameGraph>();
		for (auto& node : m_graph)
		{
			node->Process(frameRefPtr, transferCmdList, cmdList, snapshot);

			const uint32_t numRecordedCommands = transferCmdList->GetNumRecordedCommands() + cmdList->GetNumRecordedCommands();
			const uint32_t gpuCost = transferCmdList->GetGPUCost() + cmdList->GetGPUCost();
			if (gpuCost > MaxGpuCost || numRecordedCommands > MaxRecordedCommands)
			{
				SAILOR_PROFILE_BLOCK("Chaining command lists");

				driverCommands->EndDebugRegion(cmdList);
				driverCommands->EndCommandList(cmdList);

				driverCommands->EndDebugRegion(transferCmdList);
				driverCommands->EndCommandList(transferCmdList);

				// Create tasks
				{
					SAILOR_PROFILE_BLOCK("Create RHI submit cmd lists tasks");
					RHI::RHISemaphorePtr newChainSemaphore = driver->CreateWaitSemaphore();

					tasks.RemoveAll([](const auto& task) { return task == nullptr || task->IsFinished(); });

					auto submitCmdList1 = Tasks::CreateTask("Submit chaining cmd lists",
						[=]()
						{
							RHI::Renderer::GetDriver()->SubmitCommandList(transferCmdList, RHIFencePtr::Make(), newChainSemaphore, chainSemaphore);
						}, Tasks::EThreadType::RHI);

					if (tasks.Num() > 0)
					{
						submitCmdList1->Join(tasks[tasks.Num() - 1]);
					}

					submitCmdList1->Run();

					chainSemaphore = driver->CreateWaitSemaphore();

					auto submitCmdList2 = Tasks::CreateTask("Submit chaining cmd lists",
						[=]()
						{
							RHI::Renderer::GetDriver()->SubmitCommandList(cmdList, RHIFencePtr::Make(), chainSemaphore, newChainSemaphore);
						}, Tasks::EThreadType::RHI);

					submitCmdList2->Join(submitCmdList1);
					submitCmdList2->Run();

					tasks.AddRange({ submitCmdList1, submitCmdList2 });
					SAILOR_PROFILE_END_BLOCK();
				}

				// New command lists
				{
					SAILOR_PROFILE_BLOCK("Create new command lists");

					cmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Graphics);
					transferCmdList = renderer->GetDriver()->CreateCommandList(false, RHI::ECommandListQueue::Compute);

					driver->SetDebugName(cmdList, "FrameGraph:Graphics");
					driver->SetDebugName(transferCmdList, "FrameGraph:Transfer");

					driverCommands->BeginCommandList(cmdList, true);
					driverCommands->BeginDebugRegion(cmdList, "FrameGraph:Graphics", glm::vec4(0.75f, 1.0f, 0.75f, 0.1f));

					driverCommands->BeginCommandList(transferCmdList, true);
					driverCommands->BeginDebugRegion(transferCmdList, "FrameGraph:Transfer", glm::vec4(0.75f, 0.75f, 1.0f, 0.1f));

					SAILOR_PROFILE_END_BLOCK();
				}
				SAILOR_PROFILE_END_BLOCK();
			}
			//TODO: Submit Transfer command lists
		}

		driverCommands->EndDebugRegion(cmdList);
		driverCommands->EndCommandList(cmdList);

		driverCommands->EndDebugRegion(transferCmdList);
		driverCommands->EndCommandList(transferCmdList);
		SAILOR_PROFILE_END_BLOCK();

		SAILOR_PROFILE_BLOCK("Wait for submitting of chaining command lists");
		for (auto& task : tasks)
		{
			task->Wait();
		}
		SAILOR_PROFILE_END_BLOCK();

		outWaitSemaphore = chainSemaphore;
		outCommandLists.Emplace(std::move(cmdList));
		outTransferCommandLists.Emplace(transferCmdList);
	}
}

RHI::RHITexturePtr RHIFrameGraph::GetSampler(const std::string& name)
{
	if (!m_samplers.ContainsKey(name))
	{
		return RHITexturePtr();
	}

	return m_samplers[name];
}

RHI::RHIRenderTargetPtr RHIFrameGraph::GetRenderTarget(const std::string& name)
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