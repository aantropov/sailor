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
void RHIRenderSceneNode::Process(RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	static TVector<RHIMeshPtr> meshesToRender(64);
	static TVector<RHIMaterialPtr> materialsToRender(64);

	meshesToRender.Clear(false);
	materialsToRender.Clear(false);

	static TMap<RHIMaterialPtr, TMap<RHI::RHIMeshPtr, TVector<glm::mat4x4>>> drawCalls;
	static TSet<RHIMaterialPtr> materials;

	drawCalls.Clear();
	uint32_t numMeshes = 0;

	for (auto& proxy : sceneView.m_proxies)
	{
		for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
		{
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
	
	RHI::RHIShaderBindingSetPtr perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	RHI::RHIShaderBindingPtr storageBinding = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(perInstanceData, "data", sizeof(glm::mat4x4), numMeshes, 0);
	size_t ssboIndex = storageBinding->GetStorageInstanceIndex();
	
	RHICommandListPtr updateMatricesCmdList = App::GetSubmodule<Renderer>()->GetDriver()->CreateCommandList(false, true);
	commands->BeginCommandList(updateMatricesCmdList, true);

	TVector<RHIShaderBindingSetPtr> sets;

	sets.AddRange({ sceneView.m_frameBindings, perInstanceData, nullptr });

	for (auto& material : materials)
	{
		sets[2] = material->GetBindings();

		commands->BindMaterial(commandList, material);
		commands->BindShaderBindings(commandList, material, sets);

		for (auto& instancedDrawCall : drawCalls[material])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = instancedDrawCall.Second();

			commands->BindVertexBuffers(commandList, { mesh->m_vertexBuffer });
			commands->BindIndexBuffer(commandList, mesh->m_indexBuffer);

			commands->DrawIndexed(commandList, mesh->m_indexBuffer, (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t),
				(uint32_t)matrices.Num(), 0, 0, (uint32_t)ssboIndex);
			
			// Update matrices
			commands->UpdateShaderBinding(updateMatricesCmdList, storageBinding,
				matrices.GetData(),
				sizeof(glm::mat4x4) * matrices.Num(),
				sizeof(glm::mat4x4) * ssboIndex);

			ssboIndex += matrices.Num();
		}
	}

	commands->EndCommandList(updateMatricesCmdList);
	App::GetSubmodule<Renderer>()->GetDriver()->SubmitCommandList_Immediate(updateMatricesCmdList);
}

void RHIRenderSceneNode::Clear()
{
}