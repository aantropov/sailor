#include "RHIRenderSceneNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* RHIRenderSceneNode::m_name = "RenderScene";
#endif

RHI::ESortingOrder RHIRenderSceneNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetStringParam("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

/*
https://developer.nvidia.com/vulkan-shader-resource-binding
*/
void RHIRenderSceneNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	TMap<RHIMaterialPtr, TMap<RHI::RHIMeshPtr, TVector<glm::mat4x4>>> drawCalls;
	TSet<RHIMaterialPtr> materials;

	uint32_t numMeshes = 0;

	SAILOR_PROFILE_BLOCK("Filter sceneView by tag");
	for (auto& proxy : sceneView.m_proxies)
	{
		for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
		{
			const bool bIsMaterialReady = proxy.GetMaterials().Num() > i;
			if (!bIsMaterialReady)
			{
				break;
			}

			const auto& mesh = proxy.m_meshes[i];
			const auto& material = proxy.GetMaterials()[i];

			if (material->GetRenderState().GetTag() == GetHash(GetStringParam("Tag")))
			{
				drawCalls[material][mesh].Add(proxy.m_worldMatrix);
				materials.Insert(material);

				numMeshes++;
			}
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Create storage for matrices");
	RHI::RHIShaderBindingSetPtr perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	RHI::RHIShaderBindingPtr storageBinding = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(perInstanceData, "data", sizeof(glm::mat4x4), numMeshes, 0);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Prepare command list");
	TVector<glm::mat4x4> gpuMatricesData;
	gpuMatricesData.AddDefault(numMeshes);

	auto colorAttachment = GetResourceParam("color").StaticCast<RHI::RHITexture>();
	if (!colorAttachment)
	{
		colorAttachment = frameGraph->GetRenderTarget("BackBuffer");
	}

	auto depthAttachment = GetResourceParam("depthStencil").StaticCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	const size_t numThreads = scheduler->GetNumRHIThreads() + 1;
	const size_t materialsPerThread = (materials.Num()) / numThreads;

	TVector<RHICommandListPtr> secondaryCommandLists(materials.Num() > numThreads ? (numThreads - 1) : 0);
	TVector<Tasks::ITaskPtr> tasks;

	auto vecMaterials = materials.ToVector();

	SAILOR_PROFILE_BLOCK("Calculate SSBO offsets");
	size_t ssboIndex = 0;
	TVector<uint32_t> storageIndex(vecMaterials.Num());
	for (uint32_t j = 0; j < vecMaterials.Num(); j++)
	{
		auto& material = vecMaterials[j];

		const bool bIsMaterialReady = material &&
			material->GetVertexShader() &&
			material->GetFragmentShader() &&
			material->GetBindings() &&
			material->GetBindings()->GetShaderBindings().Num() > 0;

		if (!bIsMaterialReady)
		{
			continue;
		}

		bool bIsInited = false;
		for (auto& instancedDrawCall : drawCalls[material])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = instancedDrawCall.Second();

			memcpy(&gpuMatricesData[ssboIndex], matrices.GetData(), sizeof(glm::mat4x4) * matrices.Num());

			if (!bIsInited)
			{
				storageIndex[j] = storageBinding->GetStorageInstanceIndex() + (uint32_t)ssboIndex;
				bIsInited = true;
			}
			ssboIndex += matrices.Num();
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Create secondary command lists");
	for (uint32_t i = 0; i < secondaryCommandLists.Num(); i++)
	{
		const uint32_t start = (uint32_t)materialsPerThread * i;
		const uint32_t end = (uint32_t)materialsPerThread * (i + 1);

		auto task = Tasks::Scheduler::CreateTask("Record draw calls in secondary command list",
			[&, i = i, start = start, end = end]()
		{
			RHICommandListPtr cmdList = driver->CreateCommandList(true, false);
			commands->BeginSecondaryCommandList(cmdList, true, true);
			commands->SetDefaultViewport(cmdList);

			for (uint32_t j = start; j < end; j++)
			{
				auto& material = vecMaterials[j];

				const bool bIsMaterialReady = material &&
					material->GetVertexShader() &&
					material->GetFragmentShader() &&
					material->GetBindings() &&
					material->GetBindings()->GetShaderBindings().Num() > 0;

				if (!bIsMaterialReady)
				{
					continue;
				}

				TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, perInstanceData, material->GetBindings() });

				commands->BindMaterial(cmdList, material);
				commands->BindShaderBindings(cmdList, material, sets);

				uint32_t ssboOffset = 0;
				for (auto& instancedDrawCall : drawCalls[material])
				{
					auto& mesh = instancedDrawCall.First();
					auto& matrices = instancedDrawCall.Second();

					commands->BindVertexBuffers(cmdList, { mesh->m_vertexBuffer });
					commands->BindIndexBuffer(cmdList, mesh->m_indexBuffer);

					// Draw Batch
					commands->DrawIndexed(cmdList, mesh->m_indexBuffer, (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t),
						(uint32_t)matrices.Num(), 0, 0, storageIndex[j] + ssboOffset);

					ssboOffset += (uint32_t)matrices.Num();
				}
			}

			commands->EndCommandList(cmdList);
			secondaryCommandLists[i] = std::move(cmdList);
		}, Tasks::EThreadType::RHI);

		task->Run();
		tasks.Add(task);
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Fill transfer command list with matrices data");
	if (gpuMatricesData.Num() > 0)
	{
		commands->UpdateShaderBinding(transferCommandList, storageBinding,
			gpuMatricesData.GetData(),
			sizeof(glm::mat4x4) * gpuMatricesData.Num(),
			sizeof(glm::mat4x4) * storageBinding->GetStorageInstanceIndex());
	}
	SAILOR_PROFILE_END_BLOCK();

	commands->ImageMemoryBarrier(commandList, colorAttachment, colorAttachment->GetFormat(), colorAttachment->GetDefaultLayout(), EImageLayout::ColorAttachmentOptimal);

	SAILOR_PROFILE_BLOCK("Record draw calls in primary command list");
	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		true,
		glm::vec4(0.0f),
		true);

	for (size_t j = secondaryCommandLists.Num() * materialsPerThread; j < vecMaterials.Num(); j++)
	{
		auto& material = vecMaterials[j];

		const bool bIsMaterialReady = material &&
			material->GetVertexShader() &&
			material->GetFragmentShader() &&
			material->GetBindings() &&
			material->GetBindings()->GetShaderBindings().Num() > 0;

		if (!bIsMaterialReady)
		{
			continue;
		}

		TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, perInstanceData, material->GetBindings() });

		commands->BindMaterial(commandList, material);
		commands->BindShaderBindings(commandList, material, sets);

		uint32_t ssboOffset = 0;
		for (auto& instancedDrawCall : drawCalls[material])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = instancedDrawCall.Second();

			commands->BindVertexBuffers(commandList, { mesh->m_vertexBuffer });
			commands->BindIndexBuffer(commandList, mesh->m_indexBuffer);

			// Draw Batch
			commands->DrawIndexed(commandList, mesh->m_indexBuffer, (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t),
				(uint32_t)matrices.Num(), 0, 0, storageIndex[j] + ssboOffset);

			ssboOffset += (uint32_t)matrices.Num();
		}
	}
	commands->EndRenderPass(commandList);
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Wait for secondary command lists");
	for (auto& task : tasks)
	{
		task->Wait();
	}
	SAILOR_PROFILE_END_BLOCK();

	commands->RenderSecondaryCommandBuffers(commandList,
		secondaryCommandLists,
		TVector<RHI::RHITexturePtr>{ colorAttachment },
		depthAttachment,
		glm::vec4(0, 0, colorAttachment->GetExtent().x, colorAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		true,
		glm::vec4(0.0f),
		true);
	commands->ImageMemoryBarrier(commandList, colorAttachment, colorAttachment->GetFormat(), EImageLayout::ColorAttachmentOptimal, colorAttachment->GetDefaultLayout());

	SAILOR_PROFILE_END_BLOCK();
}

void RHIRenderSceneNode::Clear()
{
}