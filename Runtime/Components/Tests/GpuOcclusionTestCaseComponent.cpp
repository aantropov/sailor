#include "Components/Tests/GpuOcclusionTestCaseComponent.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/RenderSceneNode.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/GpuCulling.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include <cstring>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	constexpr uint32_t NumInstances = 513u;
	constexpr uint32_t StoragePrefix = 3u;
	constexpr uint32_t OutputStart = 7u;
	constexpr uint32_t CandidateStart = OutputStart + NumInstances + 11u;

	bool ExpectedVisible(uint32_t instance, uint32_t pattern, bool occlusion, bool cameraBack)
	{
		const uint32_t kind = instance % 8u;
		if (kind == 3u) return false;
		if (!occlusion || pattern == 0u) return true;
		if (cameraBack) return false;
		if (pattern == 1u) return kind == 1u || kind == 2u || kind == 7u;
		return kind != 0u;
	}

	std::string ValidateGpuCulling(RHIShaderPtr culling, RHIShaderPtr depthInput, RHIShaderPtr depthMips)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const EMemoryPropertyFlags hostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
		for (uint32_t scenario = 0u; scenario != 8u; ++scenario)
		{
			const uint32_t pattern = scenario < 6u ? scenario / 2u : (scenario == 6u ? 1u : 0u);
			const bool occlusion = scenario >= 6u || (scenario % 2u) != 0u;
			const bool cameraBack = scenario >= 6u;
			const glm::ivec2 inputExtent(63, 35);
			TVector<float> depth(inputExtent.x * inputExtent.y);
			for (uint32_t i = 0u; i < depth.Num(); ++i)
			{
				depth[i] = pattern == 1u || (pattern == 2u && i % inputExtent.x >= 31u) ? 0.1f : 0.0f;
			}
			auto source = driver->CreateTexture(depth.GetData(), depth.Num() * sizeof(float),
				glm::ivec3(inputExtent, 1), 1u, ETextureType::Texture2D, ETextureFormat::R32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp);

			auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(cmd, true);
			auto pyramid = driver->CreateRenderTarget(cmd, glm::ivec2(31, 17), 5u,
				ETextureFormat::R32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::Storage_Bit | ETextureUsageBit::Sampled_Bit);
			for (uint32_t mip = 0u; mip != pyramid->GetMipLevels(); ++mip)
			{
				auto read = mip == 0u ? source : pyramid->GetMipLayer(mip - 1u);
				auto write = pyramid->GetMipLayer(mip);
				auto bindings = driver->CreateShaderBindings();
				if (mip == 0u)
				{
					driver->AddSamplerToShaderBindings(bindings, "inputDepth", read, 0u);
					commands->ImageMemoryBarrierForComputeSampling(cmd, read);
				}
				else
				{
					driver->AddStorageImageToShaderBindings(bindings, "inputDepth", read, 0u);
					commands->ImageMemoryBarrier(cmd, read, EImageLayout::ComputeRead);
				}
				driver->AddStorageImageToShaderBindings(bindings, "outputDepth", write, 1u);
				commands->ImageMemoryBarrier(cmd, write, EImageLayout::ComputeWrite);
				const glm::vec2 extent(write->GetExtent());
				commands->Dispatch(cmd, mip == 0u ? depthInput : depthMips,
					(static_cast<uint32_t>(extent.x) + 7u) / 8u,
					(static_cast<uint32_t>(extent.y) + 7u) / 8u, 1u,
					{ bindings }, &extent, sizeof(extent));
			}
			commands->ImageMemoryBarrierForComputeSampling(cmd, pyramid);

			TVector<Framegraph::RenderSceneNode::PerInstanceData> instances(StoragePrefix + NumInstances);
			for (auto& instance : instances)
			{
				instance.model = glm::mat4(1.0f);
				instance.sphereBounds = glm::vec4(0.0f, 0.0f, 0.0f, 0.5f);
			}
			const glm::vec3 positions[] = {
				{ 6.0f, 0.0f, -20.0f }, { 0.0f, 0.0f, -5.0f },
				{ 0.0f, 0.0f, -0.8f }, { 1000.0f, 0.0f, -20.0f },
				{ -6.0f, 0.0f, -20.0f }, { 0.0f, 0.0f, -20.0f },
				{ 1.0f, 0.0f, -20.0f }, { 0.0f, 0.0f, -20.0f } };
			for (uint32_t i = 0u; i != NumInstances; ++i)
			{
				auto& instance = instances[StoragePrefix + i];
				const uint32_t kind = i % 8u;
				instance.model = glm::translate(glm::mat4(1.0f), positions[kind]);
				if (kind == 6u)
				{
					instance.model[0][0] = 8.0f;
					instance.model[1][0] = 3.0f;
				}
				if (kind == 7u) instance.sphereBounds.w = 10.0f;
			}
			TVector<uint32_t> indices(CandidateStart + NumInstances + 9u);
			for (auto& index : indices) index = 0u;
			for (uint32_t i = 0u; i != NumInstances; ++i)
				indices[CandidateStart + i] = StoragePrefix + i;
			DrawIndexedIndirectData draws[] = {
				{ 36u, 256u, 0u, 0, OutputStart },
				{ 36u, 256u, 0u, 0, OutputStart + 256u },
				{ 36u, 1u, 0u, 0, OutputStart + 512u } };
			UboFrameData frame{};
			frame.m_view = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, cameraBack ? -30.0f : 0.0f));
			frame.m_projection = Math::PerspectiveInfiniteRH(glm::radians(90.0f), 63.0f / 35.0f, 1.0f);
			frame.m_invProjection = glm::inverse(frame.m_projection);
			frame.m_cameraPosition = glm::vec4(0.0f, 0.0f, cameraBack ? 30.0f : 0.0f, 1.0f);
			frame.m_viewportSize = inputExtent;
			frame.m_cameraZNearZFar = glm::vec2(1.0f, 1000.0f);

			const auto makeBuffer = [&](const void* data, size_t size, EBufferUsageFlags usage)
			{
				auto buffer = driver->CreateBuffer(size, usage, hostMemory);
				std::memcpy(buffer->GetPointer(), data, size);
				return buffer;
			};
			auto dataBuffer = makeBuffer(instances.GetData(), instances.Num() * sizeof(instances[0]), EBufferUsageBit::StorageBuffer_Bit);
			auto indexBuffer = makeBuffer(indices.GetData(), indices.Num() * sizeof(uint32_t), EBufferUsageBit::StorageBuffer_Bit);
			auto drawBuffer = makeBuffer(draws, sizeof(draws), EBufferUsageBit::StorageBuffer_Bit | EBufferUsageBit::IndirectBuffer_Bit);
			auto frameBuffer = makeBuffer(&frame, sizeof(frame), EBufferUsageBit::UniformBuffer_Bit);
			auto depthBindings = driver->CreateShaderBindings();
			driver->AddSamplerToShaderBindings(depthBindings, "depthHighZ", pyramid, 0u);
			auto instanceBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(instanceBindings, dataBuffer, "data", 0u);
			driver->AddBufferToShaderBindings(instanceBindings, indexBuffer, "indices", 1u);
			auto drawBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(drawBindings, drawBuffer, "drawIndexedIndirect", 0u);
			auto frameBindings = driver->CreateShaderBindings();
			driver->AddBufferToShaderBindings(frameBindings, frameBuffer, "frameData", 0u);
			const GpuCullingPushConstants constants{
				3u, NumInstances, OutputStart, StoragePrefix, CandidateStart, 0u, occlusion ? 1u : 0u };
			RecordGpuCullingDispatches(*commands, cmd, culling,
				{ depthBindings, instanceBindings, drawBindings, frameBindings },
				constants, Renderer::GPUCullingGroupSize, true);
			commands->MemoryBarrier(cmd, static_cast<EAccessFlags>(EAccessBit::ShaderWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(cmd);
			auto fence = RHIFencePtr::Make();
			if (!driver->SubmitCommandList(cmd, fence)) return "GPU validation submission failed";
			fence->Wait(5000000000ull);
			if (!fence->IsFinished()) return "GPU validation fence exceeded five seconds";

			const auto* actualDraws = static_cast<const DrawIndexedIndirectData*>(drawBuffer->GetPointer());
			const auto* actualIndices = static_cast<const uint32_t*>(indexBuffer->GetPointer());
			for (uint32_t batch = 0u; batch != 3u; ++batch)
			{
				uint32_t visible = 0u;
				for (uint32_t local = 0u; local != draws[batch].m_instanceCount; ++local)
				{
					const uint32_t instance = batch * 256u + local;
					if (!ExpectedVisible(instance, pattern, occlusion, cameraBack)) continue;
					if (actualIndices[draws[batch].m_firstInstance + visible] != StoragePrefix + instance)
						return std::format("scenario {} batch {}: compacted visible index {} is incorrect", scenario, batch, visible);
					++visible;
				}
				if (actualDraws[batch].m_instanceCount != visible ||
					actualDraws[batch].m_firstInstance != draws[batch].m_firstInstance ||
					actualDraws[batch].m_indexCount != draws[batch].m_indexCount)
					return std::format("scenario {} batch {}: expected {} instances, got {}", scenario, batch, visible, actualDraws[batch].m_instanceCount);
			}
			for (uint32_t i = 0u; i != indices.Num(); ++i)
			{
				if (i >= OutputStart && i < OutputStart + NumInstances) continue;
				if (actualIndices[i] != indices[i])
					return std::format("scenario {}: immutable candidate/guard index {} was overwritten", scenario, i);
			}
		}
		return {};
	}
}

void GpuOcclusionTestCaseComponent::Tick(float)
{
	if (IsFinished()) return;
	if (m_validation)
	{
		if (!m_validation->IsFinished()) return;
		const auto& error = m_validation->GetResult();
		if (!error.empty()) { MarkFailed(error); return; }
		AddJournalEvent("GpuOcclusionEvidence",
			"8 GPU scenarios passed; 513 instances, 3 batches, nonzero offsets, NPOT Hi-Z, masked edge, near plane, shear, camera change",
			Utils::GetCurrentTimeMs() - m_gpuStartTimeMs);
		MarkPassed();
		return;
	}
	auto* registry = App::GetSubmodule<AssetRegistry>();
	auto* compiler = App::GetSubmodule<ShaderCompiler>();
	if (!m_cullingShader)
	{
		if (auto info = registry->GetAssetInfoPtr("Shaders/ComputeMeshCulling.shader"))
			compiler->LoadShader(info->GetFileId(), m_cullingShader, { "OCCLUSION_CULLING" });
	}
	if (!m_depthMipShader || !m_depthInputShader)
	{
		if (auto info = registry->GetAssetInfoPtr("Shaders/ComputeDepthHighZ.shader"))
		{
			compiler->LoadShader(info->GetFileId(), m_depthMipShader);
			compiler->LoadShader(info->GetFileId(), m_depthInputShader, { "DEPTH_INPUT" });
		}
	}
	if (m_cullingShader && m_cullingShader->IsReady() &&
		m_depthMipShader && m_depthMipShader->IsReady() &&
		m_depthInputShader && m_depthInputShader->IsReady())
	{
		m_gpuStartTimeMs = Utils::GetCurrentTimeMs();
		m_validation = Tasks::CreateTaskWithResult<std::string>("GPU occlusion validation",
			[culling = m_cullingShader->GetComputeShaderRHI(),
				input = m_depthInputShader->GetComputeShaderRHI(),
				mips = m_depthMipShader->GetComputeShaderRHI()]()
			{
				return ValidateGpuCulling(culling, input, mips);
			}, EThreadType::RHI);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("GPU validation shaders did not become ready within 30 seconds");
	}
}
