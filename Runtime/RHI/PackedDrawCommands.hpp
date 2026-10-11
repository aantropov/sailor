#pragma once

#include "RHI/PackedDraw.hpp"
#include "RHI/Renderer.h"
#include "RHI/GpuCulling.h"

namespace Sailor::RHI
{
	template<typename TPerInstanceData>
	void RHIUploadPackedDrawPacket(
		TPackedDrawPacket<TPerInstanceData>& packet,
		RHICommandListPtr transferCmdList,
		RHIShaderBindingSetPtr instanceBindings,
		RHIBufferPtr& indirectCommandBuffer,
		bool bCullInstances = false,
		RHIShaderBindingSetPtr* indirectCommandBufferBinding = nullptr)
	{
		SAILOR_PROFILE_FUNCTION();
		const uint32_t numInstances = packet.GetNumDrawInstances();
		const uint32_t numStorageInstances = packet.GetNumStorageInstances();
		const auto& groups = packet.GetGroups();
		if (numInstances == 0u || numStorageInstances == 0u ||
			groups.IsEmpty() || !instanceBindings)
		{
			return;
		}

		auto& driver = App::GetSubmodule<RHI::Renderer>()->GetDriver();
		auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
		auto storageBinding = instanceBindings->GetOrAddShaderBinding("data"_h);
		auto indexBinding = instanceBindings->GetOrAddShaderBinding("indices"_h);
		const uint32_t firstStorageInstance = storageBinding->GetStorageInstanceIndex();
		const uint32_t firstCandidateInstance = indexBinding->GetStorageInstanceIndex();
		// GPU culling compacts into the second half of the same flight-local SSBO.
		// The first half remains immutable until the logical view changes, so the
		// shader can restore the compacted stream without another CPU upload.
		const uint32_t firstIndexInstance = bCullInstances ?
			firstCandidateInstance + numInstances : firstCandidateInstance;

		const bool bStorageAllocationChanged =
			packet.m_uploadedStorageBinding != storageBinding ||
			packet.m_uploadedFirstStorageInstance != firstStorageInstance;
		auto& instanceUploads = packet.m_instanceUploads;
		instanceUploads.Clear(false);
		uint32_t segmentOffset = 0u;
		for (size_t index = 0u;
			index < TPackedDrawPacket<TPerInstanceData>::NumMobilitySegments;
			++index)
		{
			const auto mobility = static_cast<EMobilityType>(index);
			const auto& payload = packet.GetPayload(mobility);
			const auto& sharedPayload = packet.GetSharedPayload(mobility);
			const uint32_t segmentCount = payload.GetNumStorageInstances();
			const bool bSegmentLayoutChanged =
				packet.m_uploadedSegmentOffsets[index] != segmentOffset ||
				packet.m_uploadedSegmentCounts[index] != segmentCount;
			const bool bSharedPayloadChanged =
				packet.m_uploadedSharedPayloads[index] != sharedPayload;
			if (payload.IsPagedArena())
			{
				if (mobility == EMobilityType::Stationary)
				{
					// The contiguous fallback owns this comparison mirror. Invalidate it
					// while a paged version is active so a later mode switch cannot compare
					// against data older than the GPU's current arena pages.
					packet.m_uploadedStationaryInstances.Clear(false);
				}
				using ArenaPage = TPackedDrawArenaPage<TPerInstanceData>;
				const auto& previousPayload = packet.m_uploadedSharedPayloads[index];
				const bool bCanDiffPages = !bStorageAllocationChanged &&
					!bSegmentLayoutChanged && sharedPayload && previousPayload &&
					previousPayload->IsPagedArena() &&
					previousPayload->m_arenaCapacity == payload.m_arenaCapacity &&
					previousPayload->m_arenaPages.Num() == payload.m_arenaPages.Num();
				static const ArenaPage ZeroPage{};
				auto appendPage = [&](uint32_t pageIndex)
					{
						const uint32_t pageOffset = pageIndex * ArenaPage::NumInstances;
						if (pageOffset >= segmentCount)
						{
							return;
						}
						const uint32_t pageCount = (std::min)(
							ArenaPage::NumInstances,
							segmentCount - pageOffset);
						const auto& page = payload.m_arenaPages[pageIndex];
						instanceUploads.Add({
							page ? page->m_instances.data() : ZeroPage.m_instances.data(),
							segmentOffset + pageOffset,
							pageCount });
					};

				if (!bCanDiffPages)
				{
					if (segmentCount > 0u &&
						(bStorageAllocationChanged || bSegmentLayoutChanged ||
							!sharedPayload || bSharedPayloadChanged))
					{
						for (uint32_t pageIndex = 0u;
							pageIndex < payload.m_arenaPages.Num(); ++pageIndex)
						{
							appendPage(pageIndex);
						}
					}
				}
				else if (bSharedPayloadChanged)
				{
					for (uint32_t pageIndex = 0u;
						pageIndex < payload.m_arenaPages.Num(); ++pageIndex)
					{
						if (previousPayload->m_arenaPages[pageIndex] !=
							payload.m_arenaPages[pageIndex])
						{
							appendPage(pageIndex);
						}
					}
				}
			}
			else if (mobility == EMobilityType::Static)
			{
				// Static payload pointer identity is the immutable version. A freed
				// flight can patch a same-layout successor directly from the two
				// retained versions, without keeping another per-flight CPU mirror.
				const auto& previousPayload = packet.m_uploadedSharedPayloads[index];
				const bool bCanDiffVersions = !bStorageAllocationChanged &&
					!bSegmentLayoutChanged && sharedPayload && previousPayload &&
					previousPayload->m_instances.Num() == payload.m_instances.Num();
				if (!bCanDiffVersions)
				{
					if (segmentCount > 0u &&
						(bStorageAllocationChanged || bSegmentLayoutChanged ||
							!sharedPayload || bSharedPayloadChanged))
					{
						instanceUploads.Add({
							payload.m_instances.GetData(),
							segmentOffset,
							segmentCount });
					}
				}
				else if (bSharedPayloadChanged)
				{
					uint32_t dirtyBegin = (std::numeric_limits<uint32_t>::max)();
					auto appendDirtyRange = [&](uint32_t begin, uint32_t end)
						{
							instanceUploads.Add({
								&payload.m_instances[begin],
								segmentOffset + begin,
								end - begin });
						};
					for (uint32_t instanceIndex = 0u;
						instanceIndex < segmentCount; ++instanceIndex)
					{
						const bool bChanged = !(previousPayload->m_instances[instanceIndex] ==
							payload.m_instances[instanceIndex]);
						if (bChanged && dirtyBegin == (std::numeric_limits<uint32_t>::max)())
						{
							dirtyBegin = instanceIndex;
						}
						else if (!bChanged &&
							dirtyBegin != (std::numeric_limits<uint32_t>::max)())
						{
							appendDirtyRange(dirtyBegin, instanceIndex);
							dirtyBegin = (std::numeric_limits<uint32_t>::max)();
						}
					}
					if (dirtyBegin != (std::numeric_limits<uint32_t>::max)())
					{
						appendDirtyRange(dirtyBegin, segmentCount);
					}
				}
			}
			else if (mobility == EMobilityType::Stationary)
			{
				auto& uploaded = packet.m_uploadedStationaryInstances;
				const bool bStationaryCountChanged = uploaded.Num() != segmentCount;
				if (bStorageAllocationChanged || bSegmentLayoutChanged || bStationaryCountChanged)
				{
					if (segmentCount > 0u)
					{
						instanceUploads.Add({
							payload.m_instances.GetData(),
							segmentOffset,
							segmentCount });
						uploaded = payload.m_instances;
					}
					else
					{
						uploaded.Clear(false);
					}
				}
				else if (bSharedPayloadChanged || !sharedPayload)
				{
					uint32_t dirtyBegin = (std::numeric_limits<uint32_t>::max)();
					auto appendDirtyRange = [&](uint32_t begin, uint32_t end)
						{
							const uint32_t count = end - begin;
							instanceUploads.Add({
								&payload.m_instances[begin],
								segmentOffset + begin,
								count });
							std::memcpy(
								&uploaded[begin],
								&payload.m_instances[begin],
								sizeof(TPerInstanceData) * count);
						};
					for (uint32_t instanceIndex = 0u; instanceIndex < segmentCount; ++instanceIndex)
					{
						const bool bChanged =
							!(uploaded[instanceIndex] == payload.m_instances[instanceIndex]);
						if (bChanged && dirtyBegin == (std::numeric_limits<uint32_t>::max)())
						{
							dirtyBegin = instanceIndex;
						}
						else if (!bChanged && dirtyBegin != (std::numeric_limits<uint32_t>::max)())
						{
							appendDirtyRange(dirtyBegin, instanceIndex);
							dirtyBegin = (std::numeric_limits<uint32_t>::max)();
						}
					}
					if (dirtyBegin != (std::numeric_limits<uint32_t>::max)())
					{
						appendDirtyRange(dirtyBegin, segmentCount);
					}
				}
			}
			else if (segmentCount > 0u)
			{
				// Dynamic records deliberately remain flight-local and are rewritten
				// on every submission, independently of pointer or hash stability.
				instanceUploads.Add({
					payload.m_instances.GetData(),
					segmentOffset,
					segmentCount });
			}

			packet.m_uploadedSharedPayloads[index] = sharedPayload;
			packet.m_uploadedSegmentOffsets[index] = segmentOffset;
			packet.m_uploadedSegmentCounts[index] = segmentCount;
			segmentOffset += segmentCount;
		}
		packet.m_uploadedStorageBinding = storageBinding;
		packet.m_uploadedFirstStorageInstance = firstStorageInstance;
		packet.m_metrics.m_dirtyInstanceRanges = static_cast<uint32_t>(instanceUploads.Num());
		packet.m_metrics.m_bReusedInstancePayload = instanceUploads.IsEmpty();
		packet.m_metrics.m_bSharedImmutablePayload = packet.HasSharedImmutablePayload();

		const size_t instanceIndexBytes =
			sizeof(uint32_t) * packet.m_instanceIndices.Num();
		const bool bIndexContentsChanged =
			packet.m_uploadedInstanceIndices.Num() !=
				packet.m_instanceIndices.Num() ||
			(packet.m_instanceIndices.Num() > 0u && std::memcmp(
				packet.m_uploadedInstanceIndices.GetData(),
				packet.m_instanceIndices.GetData(),
				instanceIndexBytes) != 0);
		const bool bResetIndices = bStorageAllocationChanged ||
			packet.m_uploadedIndexBinding != indexBinding ||
			packet.m_uploadedFirstIndexInstance != firstCandidateInstance ||
			bIndexContentsChanged;
		packet.m_uploadedIndexBinding = indexBinding;
		packet.m_uploadedFirstIndexInstance = firstCandidateInstance;

		packet.m_indirectCommands.Resize(groups.Num());
		for (uint32_t groupIndex = 0u; groupIndex < groups.Num(); ++groupIndex)
		{
			const auto& group = groups[groupIndex];
			auto& command = packet.m_indirectCommands[groupIndex];
			command.m_indexCount = group.m_mesh->GetIndexCount();
			command.m_instanceCount = group.m_numInstances;
			command.m_firstIndex = group.m_mesh->GetFirstIndex();
			command.m_vertexOffset = group.m_mesh->GetVertexOffset();
			command.m_firstInstance = firstIndexInstance + group.m_firstInstance;
		}

		for (const auto& upload : instanceUploads)
		{
			const size_t rangeSize = sizeof(TPerInstanceData) * upload.m_count;
			commands->UpdateShaderBinding(
				transferCmdList,
				storageBinding,
				upload.m_data,
				rangeSize,
				sizeof(TPerInstanceData) * upload.m_offset);
			packet.m_metrics.m_instanceUploadBytes += rangeSize;
		}
		if (bResetIndices)
		{
			packet.m_resolvedInstanceIndices.Resize(packet.m_instanceIndices.Num());
			for (uint32_t index = 0u; index < packet.m_instanceIndices.Num(); ++index)
			{
				packet.m_resolvedInstanceIndices[index] =
					firstStorageInstance + packet.m_instanceIndices[index];
			}
			commands->UpdateShaderBinding(
				transferCmdList,
				indexBinding,
				packet.m_resolvedInstanceIndices.GetData(),
				instanceIndexBytes,
				0u);
			packet.m_uploadedInstanceIndices = packet.m_instanceIndices;
			packet.m_metrics.m_indexUploadBytes += instanceIndexBytes;
		}

		const size_t indirectBufferSize = sizeof(DrawIndexedIndirectData) * packet.m_indirectCommands.Num();
		bool bIndirectBufferChanged = false;
		if (!indirectCommandBuffer || indirectCommandBuffer->GetSize() < indirectBufferSize)
		{
			constexpr size_t IndirectBufferSlack = 256u;
			indirectCommandBuffer = driver->CreateIndirectBuffer(indirectBufferSize + IndirectBufferSlack);
			bIndirectBufferChanged = true;
			if (indirectCommandBufferBinding && *indirectCommandBufferBinding)
			{
				driver->AddBufferToShaderBindings(
					*indirectCommandBufferBinding,
					indirectCommandBuffer,
					"drawIndexedIndirect"_h,
					0u);
			}
		}
		else if (bCullInstances && indirectCommandBufferBinding &&
			*indirectCommandBufferBinding &&
			!(*indirectCommandBufferBinding)->HasBinding("drawIndexedIndirect"_h))
		{
			driver->AddBufferToShaderBindings(
				*indirectCommandBufferBinding,
				indirectCommandBuffer,
				"drawIndexedIndirect"_h,
				0u);
		}

		const bool bResetIndirectCommands = bCullInstances ||
			bIndirectBufferChanged ||
			packet.m_uploadedIndirectBuffer != indirectCommandBuffer ||
			packet.m_uploadedIndirectCommands.Num() != packet.m_indirectCommands.Num() ||
			(packet.m_indirectCommands.Num() > 0u && std::memcmp(
				packet.m_uploadedIndirectCommands.GetData(),
				packet.m_indirectCommands.GetData(),
				indirectBufferSize) != 0);
		if (bResetIndirectCommands)
		{
			commands->UpdateBuffer(
				transferCmdList,
				indirectCommandBuffer,
				packet.m_indirectCommands.GetData(),
				indirectBufferSize,
				0u);
			packet.m_uploadedIndirectCommands = packet.m_indirectCommands;
			packet.m_uploadedIndirectBuffer = indirectCommandBuffer;
			packet.m_metrics.m_indirectUploadBytes += indirectBufferSize;
		}
	}

	template<typename TPerInstanceData, typename TShaderBindingsCallback>
	DrawCallStats RHIDrawPackedDrawPacket(
		TPackedDrawPacket<TPerInstanceData>& packet,
		RHICommandListPtr graphicsCmdList,
		TShaderBindingsCallback&& collectShaderBindings,
		RHIBufferPtr indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange)
	{
		SAILOR_PROFILE_FUNCTION();
		DrawCallStats stats;
		auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
		const auto& groups = packet.GetGroups();
		uint32_t runBegin = 0u;
		auto& drawBindingSets = packet.m_drawBindingSets;
		while (runBegin < groups.Num())
		{
			const auto& firstGroup = groups[runBegin];
			const uint32_t runEnd = GetPackedDrawRunEnd(groups, runBegin);

			const auto& batch = firstGroup.m_batch;
			commands->BindMaterial(graphicsCmdList, batch.m_material);
			commands->SetViewport(
				graphicsCmdList,
				static_cast<float>(viewport.x),
				static_cast<float>(viewport.y),
				static_cast<float>(viewport.z),
				static_cast<float>(viewport.w),
				glm::vec2(scissors.x, scissors.y),
				glm::vec2(scissors.z, scissors.w),
				depthRange.x,
				depthRange.y);
			drawBindingSets.Clear(false);
			collectShaderBindings(batch, drawBindingSets);
			if (commands->BindShaderBindings(
				graphicsCmdList,
				batch.m_material,
				drawBindingSets))
			{
				commands->BindVertexBuffer(graphicsCmdList, batch.m_mesh->m_vertexBuffer, 0u);
				commands->BindIndexBuffer(graphicsCmdList, batch.m_mesh->m_indexBuffer, 0u);
				commands->DrawIndexedIndirect(
					graphicsCmdList,
					indirectCommandBuffer,
					sizeof(DrawIndexedIndirectData) * runBegin,
					runEnd - runBegin,
					sizeof(DrawIndexedIndirectData));
				++stats.m_numBatches;
				for (uint32_t groupIndex = runBegin; groupIndex < runEnd; ++groupIndex)
				{
					stats.m_numInstances += groups[groupIndex].m_numInstances;
				}
			}
			runBegin = runEnd;
		}
		drawBindingSets.Clear(false);

		return stats;
	}

	template<typename TPerInstanceData, typename TShaderBindingsCallback, typename TBeforeDrawCallback>
	DrawCallStats RHIRecordPackedDrawPacketImpl(
		TPackedDrawPacket<TPerInstanceData>& packet,
		RHICommandListPtr graphicsCmdList,
		RHICommandListPtr transferCmdList,
		TShaderBindingsCallback&& collectShaderBindings,
		RHIShaderBindingSetPtr instanceBindings,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange,
		RHIShaderPtr computeCullingShader,
		RHIShaderBindingSetPtr* indirectCommandBufferBinding,
		const TVector<RHIShaderBindingSetPtr>& cullingDispatchBindings,
		RHICommandListPtr cullingCommandList,
		bool bEnableOcclusion,
		TBeforeDrawCallback&& beforeDraw)
	{
		SAILOR_PROFILE_FUNCTION();
		const uint32_t numInstances = packet.GetNumDrawInstances();
		if (numInstances == 0u || packet.GetNumStorageInstances() == 0u ||
			packet.GetGroups().IsEmpty() || !instanceBindings)
		{
			return {};
		}

		RHIUploadPackedDrawPacket(packet, transferCmdList, instanceBindings,
			indirectCommandBuffer, computeCullingShader.IsValid(), indirectCommandBufferBinding);
		if (computeCullingShader)
		{
			auto commands = App::GetSubmodule<RHI::Renderer>()->GetDriverCommands();
			if (!cullingCommandList)
			{
				cullingCommandList = transferCmdList;
			}
			GpuCullingPushConstants constants;
			constants.m_numBatches = static_cast<uint32_t>(packet.GetGroups().Num());
			constants.m_numInstances = numInstances;
			constants.m_firstCandidateInstance = packet.m_uploadedFirstIndexInstance;
			constants.m_firstInstanceIndex = constants.m_firstCandidateInstance + numInstances;
			constants.m_firstStorageInstance = packet.m_uploadedFirstStorageInstance;
			constants.m_bEnableOcclusion = bEnableOcclusion ? 1u : 0u;
			commands->BeginDebugRegion(cullingCommandList, "GPU Culling"_h, DebugContext::Color_CmdCompute);
			RecordGpuCullingDispatches(*commands, cullingCommandList, computeCullingShader,
				cullingDispatchBindings, constants, Renderer::GPUCullingGroupSize,
				cullingCommandList == graphicsCmdList);
			commands->EndDebugRegion(cullingCommandList);
		}
		if (!beforeDraw())
		{
			return {};
		}
		return RHIDrawPackedDrawPacket(packet, graphicsCmdList, collectShaderBindings,
			indirectCommandBuffer, viewport, scissors, depthRange);
	}

	template<typename TPerInstanceData, typename TShaderBindingsCallback>
	DrawCallStats RHIRecordPackedDrawPacket(
		TPackedDrawPacket<TPerInstanceData>& packet,
		RHICommandListPtr graphicsCmdList,
		RHICommandListPtr transferCmdList,
		TShaderBindingsCallback&& collectShaderBindings,
		RHIShaderBindingSetPtr instanceBindings,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange = glm::vec2(0.0f, 1.0f),
		RHIShaderPtr computeCullingShader = {},
		RHIShaderBindingSetPtr* indirectCommandBufferBinding = nullptr,
		const TVector<RHIShaderBindingSetPtr>& cullingDispatchBindings = {})
	{
		auto beforeDraw = []() { return true; };
		return RHIRecordPackedDrawPacketImpl(
			packet,
			graphicsCmdList,
			transferCmdList,
			collectShaderBindings,
			instanceBindings,
			indirectCommandBuffer,
			viewport,
			scissors,
			depthRange,
			computeCullingShader,
			indirectCommandBufferBinding,
			cullingDispatchBindings,
			transferCmdList,
			false,
			beforeDraw);
	}

	template<typename TPerInstanceData, typename TShaderBindingsCallback, typename TBeforeDrawCallback>
	DrawCallStats RHIRecordPackedDrawPacketWithCurrentDepthOcclusion(
		TPackedDrawPacket<TPerInstanceData>& packet,
		RHICommandListPtr graphicsCmdList,
		RHICommandListPtr transferCmdList,
		TShaderBindingsCallback&& collectShaderBindings,
		RHIShaderBindingSetPtr instanceBindings,
		RHIBufferPtr& indirectCommandBuffer,
		glm::ivec4 viewport,
		glm::uvec4 scissors,
		glm::vec2 depthRange,
		RHIShaderPtr computeCullingShader,
		RHIShaderBindingSetPtr* indirectCommandBufferBinding,
		const TVector<RHIShaderBindingSetPtr>& cullingDispatchBindings,
		TBeforeDrawCallback&& beforeDraw)
	{
		return RHIRecordPackedDrawPacketImpl(
			packet,
			graphicsCmdList,
			transferCmdList,
			collectShaderBindings,
			instanceBindings,
			indirectCommandBuffer,
			viewport,
			scissors,
			depthRange,
			computeCullingShader,
			indirectCommandBufferBinding,
			cullingDispatchBindings,
			graphicsCmdList,
			true,
			beforeDraw);
	}
}
