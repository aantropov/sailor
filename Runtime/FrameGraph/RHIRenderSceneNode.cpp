#include "RHIRenderSceneNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* RHIRenderSceneNode::m_name = "RenderScene";
#endif

void RHIRenderSceneNode::Initialize(RHIFrameGraphPtr FrameGraph)
{
}

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
void RHIRenderSceneNode::Process(TVector<RHI::RHICommandListPtr>& transferCommandLists, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	SAILOR_PROFILE_FUNCTION();

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
	TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, perInstanceData, nullptr });

	TVector<glm::mat4x4> gpuMatricesData;
	gpuMatricesData.AddDefault(numMeshes);
	size_t ssboIndex = 0;

	for (auto& material : materials)
	{
		const bool bIsMaterialReady = material &&
			material->GetVertexShader() &&
			material->GetFragmentShader() &&
			material->GetBindings() &&
			material->GetBindings()->GetShaderBindings().Num() > 0;

		if (!bIsMaterialReady)
		{
			continue;
		}

		sets[2] = material->GetBindings();

		commands->BindMaterial(commandList, material);
		commands->BindShaderBindings(commandList, material, sets);

		for (auto& instancedDrawCall : drawCalls[material])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = instancedDrawCall.Second();

			commands->BindVertexBuffers(commandList, { mesh->m_vertexBuffer });
			commands->BindIndexBuffer(commandList, mesh->m_indexBuffer);

			memcpy(&gpuMatricesData[ssboIndex], matrices.GetData(), sizeof(glm::mat4x4) * matrices.Num());

			// Draw Batch
			commands->DrawIndexed(commandList, mesh->m_indexBuffer, (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t),
				(uint32_t)matrices.Num(), 0, 0, storageBinding->GetStorageInstanceIndex() + (uint32_t)ssboIndex);

			ssboIndex += matrices.Num();
		}
	}

	// Update matrices
	if (gpuMatricesData.Num() > 0)
	{
		RHICommandListPtr updateMatricesCmdList = App::GetSubmodule<Renderer>()->GetDriver()->CreateCommandList(false, true);
		commands->BeginCommandList(updateMatricesCmdList, true);
		commands->UpdateShaderBinding(updateMatricesCmdList, storageBinding,
			gpuMatricesData.GetData(),
			sizeof(glm::mat4x4) * gpuMatricesData.Num(),
			sizeof(glm::mat4x4) * storageBinding->GetStorageInstanceIndex());
		commands->EndCommandList(updateMatricesCmdList);
		transferCommandLists.Add(updateMatricesCmdList);
	}	
}

void RHIRenderSceneNode::Clear()
{
}