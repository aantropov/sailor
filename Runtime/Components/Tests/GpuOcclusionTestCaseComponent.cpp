#include "Components/Tests/GpuOcclusionTestCaseComponent.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/FrameGraphNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "FrameGraph/RenderSceneNode.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/GpuCulling.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/VertexDescription.h"
#include <algorithm>
#include <cmath>
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

	std::string ValidateRasterizedDepth(const ShaderSetPtr& coverageShader, uint32_t& outSamples)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const EMemoryPropertyFlags hostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
		auto vertexDescription = RHIVertexDescriptionPtr::Make();
		const RenderState state(true, true, 0.0f, false, ECullMode::None);
		auto material = driver->CreateMaterial(vertexDescription, EPrimitiveTopology::TriangleList, state, coverageShader);
		if (!material) return "depth coverage material could not be created";
		const uint32_t triangle[] = { 0u, 1u, 2u };
		auto indices = driver->CreateBuffer(sizeof(triangle), EBufferUsageBit::IndexBuffer_Bit, hostMemory);
		std::memcpy(indices->GetPointer(), triangle, sizeof(triangle));

		struct Coverage
		{
			float m_depth;
			uint32_t m_sampleMask;
			glm::uvec2 m_edge;
		};
		struct Readback
		{
			RHIBufferPtr m_buffer;
			TVector<float> m_expected;
			uint32_t m_round;
			uint32_t m_mip;
		};

		for (uint32_t scenario = 0u; scenario < 10u; ++scenario)
		{
			const uint32_t pattern = scenario % 5u;
			const bool fullResolution = scenario >= 5u;
			const glm::ivec2 extent = fullResolution ? glm::ivec2(64, 36) : glm::ivec2(63, 35);
			const glm::ivec2 pyramidExtent = fullResolution ? extent : glm::ivec2(31, 17);
			constexpr EFormat depthFormat = EFormat::D32_SFLOAT_S8_UINT;
			auto rawDepth = driver->GetOrAddMsaaFramebufferRenderTarget(depthFormat, extent);
			const uint32_t samples = static_cast<uint32_t>(rawDepth->GetMsaaSamples());
			outSamples = samples;
			if (samples > 32u)
			{
				return std::format("depth raster validation supports up to 32 actual samples, got {}", samples);
			}
			if (samples == 1u && pattern >= 1u && pattern <= 3u) continue;

			auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(cmd, true);
			commands->MemoryBarrier(cmd, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			auto resolvedDepth = driver->CreateRenderTarget(cmd, extent, 1u, depthFormat,
				ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::DepthStencilAttachment_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferDst_Bit);
			auto pyramid = driver->CreateRenderTarget(cmd, pyramidExtent, fullResolution ? 7u : 5u,
				ETextureFormat::R32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::Storage_Bit | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
			auto graph = RHIFrameGraphPtr::Make();
			graph->SetRenderTarget("DepthBuffer", resolvedDepth);
			Framegraph::FrameGraphBuilder builder;
			auto highZ = builder.CreateNode("DepthHighZ");
			if (!highZ)
			{
				commands->EndCommandList(cmd);
				return "the runtime factory did not create DepthHighZ";
			}
			highZ->SetRHIResource("src", resolvedDepth);
			highZ->SetRHIResource("dst", pyramid);
			TVector<Readback> readbacks;
			for (uint32_t round = 0u; round < 2u; ++round)
			{
				// The second draw relies on DepthHighZ restoring the raw attachment.
				commands->BeginRenderPass(cmd, TVector<RHITexturePtr>{}, resolvedDepth,
					glm::ivec4(0, 0, extent.x, extent.y), glm::ivec2(0), round == 0u,
					glm::vec4(0.0f), 0.0f, true, true);
				commands->BindMaterial(cmd, material);
				commands->BindIndexBuffer(cmd, indices, indices->GetOffset());
				commands->SetViewport(cmd, 0.0f, 0.0f, static_cast<float>(extent.x), static_cast<float>(extent.y),
					glm::vec2(0.0f), glm::vec2(extent), 0.0f, 1.0f);
				Coverage coverage{ round == 0u ? 0.1f : 0.2f, ~0u, glm::uvec2(extent) };
				if (round == 0u)
				{
					if (pattern == 1u) coverage.m_sampleMask = 1u;
					if (pattern == 2u) coverage.m_sampleMask = 2u;
					if (pattern == 3u)
					{
						const Coverage farDepth{ 0.025f, ~0u, glm::uvec2(extent) };
						commands->PushConstants(cmd, material, sizeof(farDepth), &farDepth);
						commands->DrawIndexed(cmd, 3u);
						coverage.m_sampleMask &= ~(1u << (samples - 1u));
					}
					if (pattern == 4u) coverage.m_edge -= glm::uvec2(1u);
				}
				commands->PushConstants(cmd, material, sizeof(coverage), &coverage);
				commands->DrawIndexed(cmd, 3u);
				commands->EndRenderPass(cmd);

				if (samples > 1u)
				{
					// A native MIN resolve could otherwise hide an incorrect source choice.
					commands->ImageMemoryBarrier(cmd, resolvedDepth, EImageLayout::TransferDstOptimal);
					commands->ClearDepthStencil(cmd, resolvedDepth, 0.875f);
					commands->ImageMemoryBarrier(cmd, resolvedDepth, resolvedDepth->GetDefaultLayout());
				}
				graph->ResetCurrentDepthPyramids();
				highZ->Process(graph, {}, cmd, RHISceneViewSnapshot{});
				if (!graph->HasCurrentDepthPyramid(pyramid))
				{
					commands->EndCommandList(cmd);
					return "DepthHighZ skipped the real graph source after shader preload";
				}

				TVector<float> expected(extent.x * extent.y);
				for (int32_t y = 0; y < extent.y; ++y)
				{
					for (int32_t x = 0; x < extent.x; ++x)
					{
						float depth = round == 0u ? 0.1f : 0.2f;
						if (round == 0u && (pattern == 1u || pattern == 2u ||
							(pattern == 4u && (x == extent.x - 1 || y == extent.y - 1)))) depth = 0.0f;
						if (round == 0u && pattern == 3u) depth = 0.025f;
						expected[y * extent.x + x] = depth;
					}
				}
				commands->ImageMemoryBarrier(cmd, pyramid, EImageLayout::TransferSrcOptimal);
				glm::ivec2 inputSize = extent;
				for (uint32_t mip = 0u; mip < pyramid->GetMipLevels(); ++mip)
				{
					auto layer = pyramid->GetMipLayer(mip);
					const glm::ivec2 outputSize = layer->GetExtent();
					TVector<float> reduced(outputSize.x * outputSize.y);
					for (int32_t y = 0; y < outputSize.y; ++y)
					{
						for (int32_t x = 0; x < outputSize.x; ++x)
						{
							const glm::ivec2 begin = glm::ivec2(x, y) * inputSize / outputSize;
							const glm::ivec2 end = ((glm::ivec2(x, y) + 1) * inputSize + outputSize - 1) / outputSize;
							float depth = 1.0f;
							for (int32_t sy = begin.y; sy < end.y; ++sy)
								for (int32_t sx = begin.x; sx < end.x; ++sx)
									depth = (std::min)(depth, expected[sy * inputSize.x + sx]);
							reduced[y * outputSize.x + x] = depth;
						}
					}
					auto buffer = driver->CreateBuffer(reduced.Num() * sizeof(float), EBufferUsageBit::BufferTransferDst_Bit, hostMemory);
					commands->CopyImageToBuffer(cmd, layer, buffer);
					expected = std::move(reduced);
					readbacks.Add(Readback{ buffer, expected, round, mip });
					inputSize = outputSize;
				}
			}
			commands->MemoryBarrier(cmd, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(cmd);
			auto fence = RHIFencePtr::Make();
			if (!driver->SubmitCommandList(cmd, fence)) return "depth raster validation submission failed";
			fence->Wait(5000000000ull);
			if (!fence->IsFinished()) return "depth raster validation fence exceeded five seconds";
			for (auto& readback : readbacks)
			{
				const auto* actual = static_cast<const float*>(readback.m_buffer->GetPointer());
				for (size_t pixel = 0; pixel < readback.m_expected.Num(); ++pixel)
				{
					if (!std::isfinite(actual[pixel]) || std::abs(actual[pixel] - readback.m_expected[pixel]) > 0.00001f)
						return std::format("{}x raster scenario {} round {} mip {} pixel {}: expected {}, got {}",
							samples, scenario, readback.m_round, readback.m_mip, pixel, readback.m_expected[pixel], actual[pixel]);
				}
			}
		}
		return {};
	}

	std::string ValidateGpuCulling(RHIShaderPtr culling, RHIShaderPtr depthInput, RHIShaderPtr depthMips)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		const EMemoryPropertyFlags hostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;
		for (uint32_t scenario = 0u; scenario != 16u; ++scenario)
		{
			const uint32_t cullingScenario = scenario % 8u;
			const uint32_t pattern = cullingScenario < 6u ? cullingScenario / 2u : (cullingScenario == 6u ? 1u : 0u);
			const bool occlusion = cullingScenario >= 6u || (cullingScenario % 2u) != 0u;
			const bool cameraBack = cullingScenario >= 6u;
			// Cover the odd footprint reduction as well as the 1:1 copy and even 2x2 mips.
			const bool fullResolution = scenario >= 8u;
			const glm::ivec2 inputExtent = fullResolution ? glm::ivec2(64, 36) : glm::ivec2(63, 35);
			const glm::ivec2 pyramidExtent = fullResolution ? inputExtent : glm::ivec2(31, 17);
			TVector<float> depth(inputExtent.x * inputExtent.y);
			for (uint32_t i = 0u; i < depth.Num(); ++i)
			{
				depth[i] = pattern == 1u || (pattern == 2u && i % inputExtent.x >= inputExtent.x / 2u) ? 0.1f : 0.0f;
			}
			auto source = driver->CreateTexture(depth.GetData(), depth.Num() * sizeof(float),
				glm::ivec3(inputExtent, 1), 1u, ETextureType::Texture2D, ETextureFormat::R32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp);

			auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(cmd, true);
			auto pyramid = driver->CreateRenderTarget(cmd, pyramidExtent, fullResolution ? 7u : 5u,
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
			frame.m_projection = Math::PerspectiveInfiniteRH(glm::radians(90.0f),
				static_cast<float>(inputExtent.x) / inputExtent.y, 1.0f);
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
		const auto& result = m_validation->GetResult();
		if (!result.m_error.empty()) { MarkFailed(result.m_error); return; }
		AddJournalEvent("GpuOcclusionEvidence",
			std::format("16 culling scenarios and {} raster scenarios passed at {}x; real DepthHighZ, numeric mips and attachment reuse; {}",
				result.m_samples > 1u ? 10u : 4u, result.m_samples,
				result.m_samples > 1u ? "all-sample MIN and raw source selection verified" : "single-sample path only, multisample coverage not exercised"),
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
	if (!m_depthMipShader || !m_depthInputShader || !m_depthMsaaShader)
	{
		if (auto info = registry->GetAssetInfoPtr("Shaders/ComputeDepthHighZ.shader"))
		{
			compiler->LoadShader(info->GetFileId(), m_depthMipShader);
			compiler->LoadShader(info->GetFileId(), m_depthInputShader, { "DEPTH_INPUT" });
			compiler->LoadShader(info->GetFileId(), m_depthMsaaShader, { "MSAA_DEPTH_INPUT" });
		}
	}
	if (!m_depthCoverageShader)
	{
		if (auto info = registry->GetAssetInfoPtr("Tests/Shaders/DepthCoverage.shader"))
			compiler->LoadShader(info->GetFileId(), m_depthCoverageShader);
	}
	if (m_cullingShader && m_cullingShader->IsReady() &&
		m_depthMipShader && m_depthMipShader->IsReady() &&
		m_depthInputShader && m_depthInputShader->IsReady() &&
		m_depthMsaaShader && m_depthMsaaShader->IsReady() &&
		m_depthCoverageShader && m_depthCoverageShader->IsReady())
	{
		m_gpuStartTimeMs = Utils::GetCurrentTimeMs();
		m_validation = Tasks::CreateTaskWithResult<ValidationResult>("GPU occlusion validation",
			[culling = m_cullingShader->GetComputeShaderRHI(),
				input = m_depthInputShader->GetComputeShaderRHI(),
				mips = m_depthMipShader->GetComputeShaderRHI(),
				coverage = m_depthCoverageShader]()
			{
				ValidationResult result;
				result.m_error = ValidateGpuCulling(culling, input, mips);
				if (result.m_error.empty()) result.m_error = ValidateRasterizedDepth(coverage, result.m_samples);
				return result;
			}, EThreadType::RHI);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("GPU validation shaders did not become ready within 30 seconds");
	}
}
