#include "DepthPrepassNode.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"

using namespace Sailor;
using namespace Sailor::RHI;

#ifndef _SAILOR_IMPORT_
const char* DepthPrepassNode::m_name = "DepthPrepass";
#endif

RHI::RHIMaterialPtr DepthPrepassNode::GetOrAddDepthMaterial(RHI::RHIVertexDescriptionPtr vertexDescription)
{
	auto& material = m_depthOnlyMaterials.At_Lock(vertexDescription->GetVertexAttributeBits());

	if (!material)
	{
		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/DepthOnly.shader");
		ShaderSetPtr pShader;

		if (App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader))
		{
			RenderState renderState = RHI::RenderState(true, true, 0.0f, ECullMode::Back, EBlendMode::None, EFillMode::Fill, GetHash(std::string("DepthOnly")), true);
			material = RHI::Renderer::GetDriver()->CreateMaterial(vertexDescription, RHI::EPrimitiveTopology::TriangleList, renderState, pShader);
		}
	}
	m_depthOnlyMaterials.Unlock(vertexDescription->GetVertexAttributeBits());

	return material;
}

RHI::ESortingOrder DepthPrepassNode::GetSortingOrder() const
{
	const std::string& sortOrder = GetStringParam("Sorting");

	if (!sortOrder.empty())
	{
		return magic_enum::enum_cast<RHI::ESortingOrder>(sortOrder).value_or(RHI::ESortingOrder::FrontToBack);
	}

	return RHI::ESortingOrder::FrontToBack;
}

struct PerInstanceData
{
	glm::mat4 model;

	bool operator==(const PerInstanceData& rhs) const { return this->model == model; }

	size_t GetHash() const
	{
		hash<glm::mat4> p;
		return p(model);
	}
};

class Batch
{
public:

	RHIMaterialPtr m_material;
	RHIMeshPtr m_mesh;

	Batch() = default;
	Batch(const RHIMaterialPtr& material, const RHIMeshPtr& mesh) : m_material(material), m_mesh(mesh) {}

	bool operator==(const Batch& rhs) const
	{
		return
			m_material->GetBindings()->GetCompatibilityHashCode() == rhs.m_material->GetBindings()->GetCompatibilityHashCode() &&
			m_material->GetVertexShader() == rhs.m_material->GetVertexShader() &&
			m_material->GetFragmentShader() == rhs.m_material->GetFragmentShader() &&
			m_material->GetRenderState() == rhs.m_material->GetRenderState() &&
			m_mesh->m_vertexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_vertexBuffer->GetCompatibilityHashCode() &&
			m_mesh->m_indexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_indexBuffer->GetCompatibilityHashCode();
	}

	size_t GetHash() const
	{
		size_t hash = m_material->GetBindings()->GetCompatibilityHashCode();
		HashCombine(hash, m_mesh->m_vertexBuffer->GetCompatibilityHashCode(), m_mesh->m_indexBuffer->GetCompatibilityHashCode());
		return hash;
	}
};

void RecordDrawCall(uint32_t start,
	uint32_t end,
	const TVector<Batch>& vecBatches,
	RHI::RHICommandListPtr cmdList,
	const RHI::RHISceneViewSnapshot& sceneView,
	RHI::RHIShaderBindingSetPtr perInstanceData,
	const TMap<Batch, TMap<RHI::RHIMeshPtr, TVector<PerInstanceData>>>& drawCalls,
	const TVector<uint32_t>& storageIndex)
{
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	for (uint32_t j = start; j < end; j++)
	{
		auto& material = vecBatches[j].m_material;
		auto& mesh = vecBatches[j].m_mesh;

		TVector<RHIShaderBindingSetPtr> sets({ sceneView.m_frameBindings, perInstanceData });

		commands->BindMaterial(cmdList, material);
		commands->BindShaderBindings(cmdList, material, sets);

		commands->BindVertexBuffer(cmdList, mesh->m_vertexBuffer, 0);
		commands->BindIndexBuffer(cmdList, mesh->m_indexBuffer, 0);

		TVector<RHI::DrawIndexedIndirectData> drawIndirect;
		drawIndirect.Reserve(drawCalls[vecBatches[j]].Num());

		uint32_t ssboOffset = 0;
		for (auto& instancedDrawCall : drawCalls[vecBatches[j]])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = instancedDrawCall.Second();

			RHI::DrawIndexedIndirectData data{};
			data.m_indexCount = (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t);
			data.m_instanceCount = (uint32_t)matrices.Num();
			data.m_firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
			data.m_vertexOffset = mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();
			data.m_firstInstance = storageIndex[j] + ssboOffset;

			drawIndirect.Emplace(std::move(data));

			ssboOffset += (uint32_t)matrices.Num();
		}

		const size_t size = sizeof(RHI::DrawIndexedIndirectData) * drawIndirect.Num();

		RHIBufferPtr indirectCommandBuffer = driver->CreateIndirectBuffer(size);
		commands->UpdateBuffer(cmdList, indirectCommandBuffer, drawIndirect.GetData(), size);
		commands->DrawIndexedIndirect(cmdList, indirectCommandBuffer, 0, (uint32_t)drawIndirect.Num(), sizeof(RHI::DrawIndexedIndirectData));
	}
}

void DepthPrepassNode::Process(RHIFrameGraph* frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView)
{
	auto scheduler = App::GetSubmodule<Tasks::Scheduler>();
	auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
	auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

	TMap<Batch, TMap<RHI::RHIMeshPtr, TVector<PerInstanceData>>> drawCalls;
	TSet<Batch> batches;

	uint32_t numMeshes = 0;

	SAILOR_PROFILE_BLOCK("Filter sceneView by tag");
	for (auto& proxy : sceneView.m_proxies)
	{
		for (size_t i = 0; i < proxy.m_meshes.Num(); i++)
		{
			const bool bHasMaterial = proxy.GetMaterials().Num() > i;
			if (!bHasMaterial)
			{
				break;
			}

			//TODO: Depth only material from proxy
			const auto& mesh = proxy.m_meshes[i];
			const auto& depthMaterial = GetOrAddDepthMaterial(mesh->m_vertexDescription);

			const bool bIsDepthMaterialReady = depthMaterial &&
				depthMaterial->GetVertexShader() &&
				depthMaterial->GetFragmentShader() &&
				depthMaterial->GetRenderState().IsEnabledZWrite();

			if (!bIsDepthMaterialReady)
			{
				continue;
			}

			if (proxy.GetMaterials()[i]->GetRenderState().GetTag() == GetHash(GetStringParam("Tag")))
			{
				PerInstanceData data;
				data.model = proxy.m_worldMatrix;

				Batch batch(depthMaterial, mesh);

				drawCalls[batch][mesh].Add(data);
				batches.Insert(batch);

				numMeshes++;
			}
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	if (numMeshes == 0)
	{
		return;
	}

	SAILOR_PROFILE_BLOCK("Create storage for matrices");
	RHI::RHIShaderBindingSetPtr perInstanceData = Sailor::RHI::Renderer::GetDriver()->CreateShaderBindings();
	RHI::RHIShaderBindingPtr storageBinding = Sailor::RHI::Renderer::GetDriver()->AddSsboToShaderBindings(perInstanceData, "data", sizeof(PerInstanceData), numMeshes, 0);
	SAILOR_PROFILE_END_BLOCK();

	TVector<PerInstanceData> gpuMatricesData;
	gpuMatricesData.AddDefault(numMeshes);
	auto vecBatches = batches.ToVector();

	SAILOR_PROFILE_BLOCK("Calculate SSBO offsets");
	size_t ssboIndex = 0;
	TVector<uint32_t> storageIndex(vecBatches.Num());
	for (uint32_t j = 0; j < vecBatches.Num(); j++)
	{
		bool bIsInited = false;
		for (auto& instancedDrawCall : drawCalls[vecBatches[j]])
		{
			auto& mesh = instancedDrawCall.First();
			auto& matrices = instancedDrawCall.Second();

			memcpy(&gpuMatricesData[ssboIndex], matrices.GetData(), sizeof(PerInstanceData) * matrices.Num());

			if (!bIsInited)
			{
				storageIndex[j] = storageBinding->GetStorageInstanceIndex() + (uint32_t)ssboIndex;
				bIsInited = true;
			}
			ssboIndex += matrices.Num();
		}
	}
	SAILOR_PROFILE_END_BLOCK();

	SAILOR_PROFILE_BLOCK("Fill transfer command list with matrices data");
	if (gpuMatricesData.Num() > 0)
	{
		commands->UpdateShaderBinding(transferCommandList, storageBinding,
			gpuMatricesData.GetData(),
			sizeof(PerInstanceData) * gpuMatricesData.Num(),
			0);
	}
	SAILOR_PROFILE_END_BLOCK();

	auto depthAttachment = GetResourceParam("depthStencil").StaticCast<RHI::RHITexture>();
	if (!depthAttachment)
	{
		depthAttachment = frameGraph->GetRenderTarget("DepthBuffer");
	}

	SAILOR_PROFILE_BLOCK("Record draw calls in primary command list");
	commands->BeginRenderPass(commandList,
		TVector<RHI::RHITexturePtr>{},
		depthAttachment,
		glm::vec4(0, 0, depthAttachment->GetExtent().x, depthAttachment->GetExtent().y),
		glm::ivec2(0, 0),
		true,
		glm::vec4(0.0f),
		true,
		true);

	RecordDrawCall(0, (uint32_t)vecBatches.Num(), vecBatches, commandList, sceneView, perInstanceData, drawCalls, storageIndex);
	commands->EndRenderPass(commandList);
	SAILOR_PROFILE_END_BLOCK();
}

void DepthPrepassNode::Clear()
{
}