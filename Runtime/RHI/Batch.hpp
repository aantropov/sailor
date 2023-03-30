#pragma once
#include "Containers/Map.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{
	class RHIBatch
	{
	public:

		RHIMaterialPtr m_material;

		// Here we store the vertex and index bindings that could be shared during rendering (not meshes)
		RHIMeshPtr m_mesh;

		RHIBatch() = default;
		RHIBatch(const RHIMaterialPtr& material, const RHIMeshPtr& mesh) : m_material(material), m_mesh(mesh) {}

		bool operator==(const RHIBatch& rhs) const
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

	template<typename TPerInstanceData>
	using TDrawCalls = TMap<RHIBatch, TMap<RHI::RHIMeshPtr, TVector<TPerInstanceData>>>;

	template<typename TPerInstanceData>
	void RHIRecordDrawCall(uint32_t start,
		uint32_t end,
		const TVector<RHIBatch>& vecBatches,
		RHI::RHICommandListPtr cmdList,
		std::function<TVector<RHIShaderBindingSetPtr>(RHIMaterialPtr)> shaderBindings,
		const TDrawCalls<TPerInstanceData>& drawCalls,
		const TVector<uint32_t>& storageIndex,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors)
	{
		SAILOR_PROFILE_BLOCK("Record draw calls");

		auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
		auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

		size_t indirectBufferSize = 0;
		for (uint32_t j = start; j < end; j++)
		{
			indirectBufferSize += drawCalls[vecBatches[j]].Num() * sizeof(RHI::DrawIndexedIndirectData);
		}

		if (!indirectCommandBuffer.IsValid() || indirectCommandBuffer->GetSize() < indirectBufferSize)
		{
			const size_t slack = 256;

			indirectCommandBuffer.Clear();
			indirectCommandBuffer = driver->CreateIndirectBuffer(indirectBufferSize + slack);
		}

		RHIMaterialPtr prevMaterial = nullptr;
		RHIBufferPtr prevVertexBuffer = nullptr;
		RHIBufferPtr prevIndexBuffer = nullptr;

		size_t indirectBufferOffset = 0;
		for (uint32_t j = start; j < end; j++)
		{
			auto& material = vecBatches[j].m_material;
			auto& mesh = vecBatches[j].m_mesh;
			auto& drawCall = drawCalls[vecBatches[j]];

			if (prevMaterial != material)
			{
				TVector<RHIShaderBindingSetPtr> sets = shaderBindings(material);

				commands->BindMaterial(cmdList, material);
				commands->SetViewport(cmdList, (float)viewport.x, (float)viewport.y,
					(float)viewport.z,
					(float)viewport.w,
					glm::vec2(scissors.x, scissors.y), 
					glm::vec2(scissors.z, scissors.w),
					0.0f, 
					1.0f);

				commands->BindShaderBindings(cmdList, material, sets);
				prevMaterial = material;
			}

			if (prevVertexBuffer != mesh->m_vertexBuffer)
			{
				commands->BindVertexBuffer(cmdList, mesh->m_vertexBuffer, 0);
				prevVertexBuffer = mesh->m_vertexBuffer;
			}

			if (prevIndexBuffer != mesh->m_indexBuffer)
			{
				commands->BindIndexBuffer(cmdList, mesh->m_indexBuffer, 0);
				prevIndexBuffer = mesh->m_indexBuffer;
			}

			TVector<RHI::DrawIndexedIndirectData> drawIndirect;
			drawIndirect.Reserve(drawCall.Num());

			uint32_t ssboOffset = 0;
			for (auto& instancedDrawCall : drawCall)
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

			const size_t bufferSize = sizeof(RHI::DrawIndexedIndirectData) * drawIndirect.Num();
			commands->UpdateBuffer(cmdList, indirectCommandBuffer, drawIndirect.GetData(), bufferSize, indirectBufferOffset);
			commands->DrawIndexedIndirect(cmdList, indirectCommandBuffer, indirectBufferOffset, (uint32_t)drawIndirect.Num(), sizeof(RHI::DrawIndexedIndirectData));

			indirectBufferOffset += bufferSize;
		}
	}

};
