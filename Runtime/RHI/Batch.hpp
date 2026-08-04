#pragma once
#include "Containers/Map.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/Texture.h"
#include "RHI/RenderTarget.h"
#include <limits>
#include <mutex>

using namespace GraphicsDriver::Vulkan;

namespace Sailor::RHI
{	
	class RHIBatch
	{
	public:

		RHIMaterialPtr m_material;

		// Here we store the vertex and index bindings that could be shared during rendering (not meshes)
		RHIMeshPtr m_mesh;
		RHIShaderBindingSetPtr m_textureBindings;
		uint32_t m_supportedMeshesPerBatch = std::numeric_limits<uint32_t>::max();

		RHIBatch() = default;
		RHIBatch(const RHIMaterialPtr& material, const RHIMeshPtr& mesh) : m_material(material), m_mesh(mesh) {}

		bool operator==(const RHIBatch& rhs) const
		{
			const bool bSameBatch =
				m_material->GetBindings()->GetCompatibilityHashCode() == rhs.m_material->GetBindings()->GetCompatibilityHashCode() &&
				m_material->GetVertexShader() == rhs.m_material->GetVertexShader() &&
				m_material->GetFragmentShader() == rhs.m_material->GetFragmentShader() &&
				m_material->GetRenderState() == rhs.m_material->GetRenderState() &&
				m_mesh->m_vertexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_vertexBuffer->GetCompatibilityHashCode() &&
				m_mesh->m_indexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_indexBuffer->GetCompatibilityHashCode() &&
				m_textureBindings == rhs.m_textureBindings;

			return bSameBatch;
		}

		size_t GetHash() const
		{
			const size_t renderStateHash = Sailor::GetHash(m_material->GetRenderState());

			size_t hash = m_material->GetBindings()->GetCompatibilityHashCode();

			HashCombine(hash, m_mesh->m_vertexBuffer->GetCompatibilityHashCode(), m_mesh->m_indexBuffer->GetCompatibilityHashCode(), renderStateHash);
			HashCombine(hash, m_textureBindings);
			return hash;
		}
	};

	template<typename TPerInstanceData>
	using TDrawCalls = TMap<RHIBatch, TMap<RHI::RHIMeshPtr, TVector<TPerInstanceData>>>;

	template<typename TPerInstanceData>
	DrawCallStats RHIRecordDrawCallGPUCulling(uint32_t start,
		uint32_t end,
		const TVector<RHIBatch>& vecBatches,
		RHI::RHICommandListPtr graphicsCmdList,
		RHI::RHICommandListPtr transferCmdList,
		std::function<TVector<RHIShaderBindingSetPtr>(const RHIBatch&)> shaderBindings,
		const TDrawCalls<TPerInstanceData>& drawCalls,
		const TVector<uint32_t>& storageIndex,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange,
		RHIShaderPtr computeCullingShader,
		RHIShaderBindingSetPtr& indirectCommandBufferBinding,
		const TVector<RHIShaderBindingSetPtr>& cullingDistpatchBindings,
		std::mutex* transferCommandListMutex = nullptr)
	{
		SAILOR_PROFILE_FUNCTION();
		DrawCallStats stats;
		if (start >= end || end > vecBatches.Num() || end > storageIndex.Num())
		{
			return stats;
		}

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

			Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(indirectCommandBufferBinding,
				indirectCommandBuffer,
				"drawIndexedIndirect",
				0);
		}
		else if (!indirectCommandBufferBinding->HasBinding("drawIndexedIndirect"))
		{
			// The buffer can have been allocated by the non-culling path. In that case
			// its storage-buffer descriptor still has to be created before dispatch.
			Sailor::RHI::Renderer::GetDriver()->AddBufferToShaderBindings(indirectCommandBufferBinding,
				indirectCommandBuffer,
				"drawIndexedIndirect",
				0);
		}

		RHIMaterialPtr prevMaterial = nullptr;
		RHIShaderBindingSetPtr prevTextureBindings = nullptr;
		RHIBufferPtr prevVertexBuffer = nullptr;
		RHIBufferPtr prevIndexBuffer = nullptr;

		uint32_t firstInstanceIndex = storageIndex[start];
		uint32_t totalNumInstances = 0;
		uint32_t totalNumBatches = 0;

#if defined(_WIN32)
		constexpr uint32_t MaxMeshesPerIndirectBatch = 16384;
#else
		constexpr uint32_t MaxMeshesPerIndirectBatch = 128;
#endif

		size_t indirectBufferOffset = 0;
		for (uint32_t j = start; j < end; j++)
		{
			auto& material = vecBatches[j].m_material;
			auto& mesh = vecBatches[j].m_mesh;
			auto& drawCall = drawCalls[vecBatches[j]];

			const bool bMaterialChanged = prevMaterial != material;
			const bool bTextureBindingsChanged = prevTextureBindings != vecBatches[j].m_textureBindings;
			if (bMaterialChanged)
			{
				commands->BindMaterial(graphicsCmdList, material);
				commands->SetViewport(graphicsCmdList, (float)viewport.x, (float)viewport.y,
					(float)viewport.z,
					(float)viewport.w,
					glm::vec2(scissors.x, scissors.y),
					glm::vec2(scissors.z, scissors.w),
					depthRange.x,
					depthRange.y);
				prevMaterial = material;
			}
			if (bMaterialChanged || bTextureBindingsChanged)
			{
				TVector<RHIShaderBindingSetPtr> sets = shaderBindings(vecBatches[j]);
				commands->BindShaderBindings(graphicsCmdList, material, sets);
				prevTextureBindings = vecBatches[j].m_textureBindings;
			}

			if (prevVertexBuffer != mesh->m_vertexBuffer)
			{
				commands->BindVertexBuffer(graphicsCmdList, mesh->m_vertexBuffer, 0);
				prevVertexBuffer = mesh->m_vertexBuffer;
			}

			if (prevIndexBuffer != mesh->m_indexBuffer)
			{
				commands->BindIndexBuffer(graphicsCmdList, mesh->m_indexBuffer, 0);
				prevIndexBuffer = mesh->m_indexBuffer;
			}

			firstInstanceIndex = std::min(firstInstanceIndex, storageIndex[j]);

			uint32_t ssboOffset = 0;
			const uint32_t meshesPerBatchLimit = (std::min)(MaxMeshesPerIndirectBatch, vecBatches[j].m_supportedMeshesPerBatch);
			uint32_t meshesInCurrentBatch = 0;
			TVector<RHI::DrawIndexedIndirectData> drawIndirect;
			drawIndirect.Reserve((std::min)((uint32_t)drawCall.Num(), meshesPerBatchLimit));

			auto flushIndirectBatch = [&]()
			{
				if (drawIndirect.IsEmpty())
				{
					return;
				}

				const size_t bufferSize = sizeof(RHI::DrawIndexedIndirectData) * drawIndirect.Num();
				if (transferCommandListMutex)
				{
					std::lock_guard<std::mutex> transferCommandListLock(*transferCommandListMutex);
					commands->UpdateBuffer(transferCmdList, indirectCommandBuffer, drawIndirect.GetData(), bufferSize, indirectBufferOffset);
				}
				else
				{
					commands->UpdateBuffer(transferCmdList, indirectCommandBuffer, drawIndirect.GetData(), bufferSize, indirectBufferOffset);
				}
				commands->DrawIndexedIndirect(graphicsCmdList, indirectCommandBuffer, indirectBufferOffset, (uint32_t)drawIndirect.Num(), sizeof(RHI::DrawIndexedIndirectData));
				stats.m_numBatches++;
				indirectBufferOffset += bufferSize;
				drawIndirect.Clear();
				meshesInCurrentBatch = 0;
			};

			for (const auto& instancedDrawCall : drawCall)
			{
				auto& mesh = instancedDrawCall.First();
				auto& matrices = *instancedDrawCall.Second();

				RHI::DrawIndexedIndirectData data{};
				data.m_indexCount = (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t);
				data.m_instanceCount = (uint32_t)matrices.Num();
				data.m_firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
				data.m_vertexOffset = mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();
				data.m_firstInstance = storageIndex[j] + ssboOffset;
				drawIndirect.Emplace(std::move(data));
				meshesInCurrentBatch++;

				ssboOffset += (uint32_t)matrices.Num();

				totalNumBatches++;
				totalNumInstances += (uint32_t)matrices.Num();

				if (meshesInCurrentBatch >= meshesPerBatchLimit)
				{
					flushIndirectBatch();
				}
			}

			flushIndirectBatch();
		}

		struct PushConstants
		{
			uint32_t m_numBatches = 0;
			uint32_t m_numInstances = 0;
			uint32_t m_firstInstanceIndex = 0;
			uint32_t m_phase = 0;
			uint32_t m_bEnableOcclusion = 0;
		};

		PushConstants constants{};
		constants.m_numBatches = totalNumBatches;
		constants.m_numInstances = totalNumInstances;
		constants.m_firstInstanceIndex = firstInstanceIndex;
		// The Hi-Z texture belongs to the previous frame, while the frame and
		// instance transforms are current. Occlusion is not conservative until
		// matching camera/object history is supplied; frustum culling remains on.
		constants.m_bEnableOcclusion = 0;
		stats.m_numInstances = totalNumInstances;

		auto recordCullingDispatch = [&]()
			{
				commands->BeginDebugRegion(transferCmdList, "GPU Culling", DebugContext::Color_CmdCompute);

				const EAccessFlags uploadWrites = static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit) |
					static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit);
				const EAccessFlags shaderReadWrite = static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit) |
					static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit);
				commands->MemoryBarrier(transferCmdList, uploadWrites, shaderReadWrite);

				constants.m_phase = 0;
				const uint32_t cullingGroupsX = (std::max)(1u, (constants.m_numInstances + RHI::Renderer::GPUCullingGroupSize - 1u) / RHI::Renderer::GPUCullingGroupSize);
				commands->Dispatch(transferCmdList, computeCullingShader, cullingGroupsX, 1, 1, cullingDistpatchBindings, &constants, sizeof(constants));

				commands->MemoryBarrier(transferCmdList,
					static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit),
					shaderReadWrite);

				constants.m_phase = 1;
				const uint32_t compactionGroupsX = (std::max)(1u, (constants.m_numBatches + RHI::Renderer::GPUCullingGroupSize - 1u) / RHI::Renderer::GPUCullingGroupSize);
				commands->Dispatch(transferCmdList, computeCullingShader, compactionGroupsX, 1, 1, cullingDistpatchBindings, &constants, sizeof(constants));
				commands->EndDebugRegion(transferCmdList);
			};

		if (transferCommandListMutex)
		{
			std::lock_guard<std::mutex> transferCommandListLock(*transferCommandListMutex);
			recordCullingDispatch();
		}
		else
		{
			recordCullingDispatch();
		}

		return stats;
	}

	template<typename TPerInstanceData>
	DrawCallStats RHIRecordDrawCall(uint32_t start,
		uint32_t end,
		const TVector<RHIBatch>& vecBatches,
		RHI::RHICommandListPtr cmdList,
		RHI::RHICommandListPtr transferCmdList,
		std::function<TVector<RHIShaderBindingSetPtr>(const RHIBatch&)> shaderBindings,
		const TDrawCalls<TPerInstanceData>& drawCalls,
		const TVector<uint32_t>& storageIndex,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange = glm::vec2(0.0f, 1.0f),
		RHIShaderBindingSetPtr* indirectCommandBufferBinding = nullptr,
		std::mutex* transferCommandListMutex = nullptr)
	{
		SAILOR_PROFILE_FUNCTION();
		DrawCallStats stats;

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

			// The same indirect buffer is used by the optional GPU-culling path.
			// Keep its storage descriptor synchronized when streaming adds enough
			// draw calls to grow the buffer while culling is temporarily unavailable.
			if (indirectCommandBufferBinding && *indirectCommandBufferBinding)
			{
				driver->AddBufferToShaderBindings(
					*indirectCommandBufferBinding,
					indirectCommandBuffer,
					"drawIndexedIndirect",
					0);
			}
		}

		RHIMaterialPtr prevMaterial = nullptr;
		RHIShaderBindingSetPtr prevTextureBindings = nullptr;
		RHIBufferPtr prevVertexBuffer = nullptr;
		RHIBufferPtr prevIndexBuffer = nullptr;

		size_t indirectBufferOffset = 0;
		for (uint32_t j = start; j < end; j++)
		{
			auto& material = vecBatches[j].m_material;
			auto& mesh = vecBatches[j].m_mesh;
			auto& drawCall = drawCalls[vecBatches[j]];

			const bool bMaterialChanged = prevMaterial != material;
			const bool bTextureBindingsChanged = prevTextureBindings != vecBatches[j].m_textureBindings;
			if (bMaterialChanged)
			{
				commands->BindMaterial(cmdList, material);
				commands->SetViewport(cmdList, (float)viewport.x, (float)viewport.y,
					(float)viewport.z,
					(float)viewport.w,
					glm::vec2(scissors.x, scissors.y),
					glm::vec2(scissors.z, scissors.w),
					depthRange.x,
					depthRange.y);
				prevMaterial = material;
			}
			if (bMaterialChanged || bTextureBindingsChanged)
			{
				TVector<RHIShaderBindingSetPtr> sets = shaderBindings(vecBatches[j]);
				commands->BindShaderBindings(cmdList, material, sets);
				prevTextureBindings = vecBatches[j].m_textureBindings;
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
			for (const auto& instancedDrawCall : drawCall)
			{
				auto& mesh = instancedDrawCall.First();
				auto& matrices = *instancedDrawCall.Second();

				RHI::DrawIndexedIndirectData data{};
				data.m_indexCount = (uint32_t)mesh->m_indexBuffer->GetSize() / sizeof(uint32_t);
				data.m_instanceCount = (uint32_t)matrices.Num();
				data.m_firstIndex = (uint32_t)mesh->m_indexBuffer->GetOffset() / sizeof(uint32_t);
				data.m_vertexOffset = mesh->m_vertexBuffer->GetOffset() / (uint32_t)mesh->m_vertexDescription->GetVertexStride();
				data.m_firstInstance = storageIndex[j] + ssboOffset;

				drawIndirect.Emplace(std::move(data));

				ssboOffset += (uint32_t)matrices.Num();
				stats.m_numInstances += (uint32_t)matrices.Num();
			}

			const size_t bufferSize = sizeof(RHI::DrawIndexedIndirectData) * drawIndirect.Num();
			if (transferCommandListMutex)
			{
				std::lock_guard<std::mutex> transferCommandListLock(*transferCommandListMutex);
				commands->UpdateBuffer(transferCmdList, indirectCommandBuffer, drawIndirect.GetData(), bufferSize, indirectBufferOffset);
			}
			else
			{
				commands->UpdateBuffer(transferCmdList, indirectCommandBuffer, drawIndirect.GetData(), bufferSize, indirectBufferOffset);
			}
			commands->DrawIndexedIndirect(cmdList, indirectCommandBuffer, indirectBufferOffset, (uint32_t)drawIndirect.Num(), sizeof(RHI::DrawIndexedIndirectData));
			stats.m_numBatches++;

			indirectBufferOffset += bufferSize;
		}

		return stats;
	}

	template<typename TPerInstanceData>
	DrawCallStats RHIDrawCall(uint32_t start,
		uint32_t end,
		const TVector<RHIBatch>& vecBatches,
		RHI::RHICommandListPtr cmdList,
		std::function<TVector<RHIShaderBindingSetPtr>(const RHIBatch&)> shaderBindings,
		const TDrawCalls<TPerInstanceData>& drawCalls,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange = glm::vec2(0.0f, 1.0f))
	{
		SAILOR_PROFILE_FUNCTION();
		DrawCallStats stats;

		auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();

		RHIMaterialPtr prevMaterial = nullptr;
		RHIShaderBindingSetPtr prevTextureBindings = nullptr;
		RHIBufferPtr prevVertexBuffer = nullptr;
		RHIBufferPtr prevIndexBuffer = nullptr;

		size_t indirectBufferOffset = 0;
		for (uint32_t j = start; j < end; j++)
		{
			auto& material = vecBatches[j].m_material;
			auto& mesh = vecBatches[j].m_mesh;
			auto& drawCall = drawCalls[vecBatches[j]];
			for (const auto& instancedDrawCall : drawCall)
			{
				stats.m_numInstances += (uint32_t)instancedDrawCall.Second()->Num();
			}

			const bool bMaterialChanged = prevMaterial != material;
			const bool bTextureBindingsChanged = prevTextureBindings != vecBatches[j].m_textureBindings;
			if (bMaterialChanged)
			{
				commands->BindMaterial(cmdList, material);
				commands->SetViewport(cmdList, (float)viewport.x, (float)viewport.y,
					(float)viewport.z,
					(float)viewport.w,
					glm::vec2(scissors.x, scissors.y),
					glm::vec2(scissors.z, scissors.w),
					depthRange.x,
					depthRange.y);
				prevMaterial = material;
			}
			if (bMaterialChanged || bTextureBindingsChanged)
			{
				TVector<RHIShaderBindingSetPtr> sets = shaderBindings(vecBatches[j]);
				commands->BindShaderBindings(cmdList, material, sets);
				prevTextureBindings = vecBatches[j].m_textureBindings;
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

			const size_t bufferSize = sizeof(RHI::DrawIndexedIndirectData) * drawCall.Num();
			commands->DrawIndexedIndirect(cmdList, indirectCommandBuffer, indirectBufferOffset, (uint32_t)drawCall.Num(), sizeof(RHI::DrawIndexedIndirectData));
			stats.m_numBatches++;

			indirectBufferOffset += bufferSize;
		}

		return stats;
	}
};
