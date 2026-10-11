#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/BloomNode.h"
#include "FrameGraph/EyeAdaptationNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "GraphicsDriver/Vulkan/VulkanDescriptors.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/RenderTarget.h"
#include "RHI/Renderer.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <glm/gtc/packing.hpp>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	ShaderSetPtr Load(std::string_view path)
	{
		const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(path);
		ShaderSetPtr shader;
		Require(info && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(info->GetFileId(), shader) && shader->IsReady(),
			"production post-processing shader must load");
		return shader;
	}

	class BloomProbe : public BloomNode
	{
	public:
		using BloomNode::PushConstantsDownscale;
		void Shaders(ShaderSetPtr down, ShaderSetPtr up) { m_pComputeDownscaleShader = down; m_pComputeUpscaleShader = up; }
		RHIShaderBindingSetPtr FirstBinding() const { return m_computeDownscaleBindings.IsEmpty() ? nullptr : m_computeDownscaleBindings[0]; }
	};

	class ExposureProbe : public EyeAdaptationNode
	{
	public:
		void Shaders(ShaderSetPtr histogram, ShaderSetPtr average) { m_pComputeHistogramShader = histogram; m_pComputeAverageShader = average; }
	};

	RHIRenderTargetPtr Texture(uint32_t side, uint32_t levels, EFormat format)
	{
		return Renderer::GetDriver()->CreateRenderTarget(glm::ivec2(side), levels, format,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::Storage_Bit | ETextureUsageBit::Sampled_Bit |
			ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
	}

	void Clear(RHICommandListPtr command, RHIRenderTargetPtr image, const glm::vec4& color)
	{
		auto commands = Renderer::GetDriverCommands();
		commands->ImageMemoryBarrier(command, image, EImageLayout::TransferDstOptimal);
		commands->ClearImage(command, image, color);
		commands->ImageMemoryBarrier(command, image, image->GetDefaultLayout());
	}

	void Publish(BaseFrameGraphNode& node, RHIFrameGraphPtr graph, StringHash slot, StringHash name,
		RHIRenderTargetPtr target, bool named, bool surface)
	{
		RHIResourcePtr resource = surface ? RHIResourcePtr(Renderer::GetDriver()->CreateSurface(target)) : RHIResourcePtr(target);
		Require(resource.IsValid(), "post-processing resource must initialize");
		if (!named) node.SetRHIResource(slot, resource);
		else if (surface) graph->SetSurface(name, resource.DynamicCast<RHISurface>());
		else graph->SetRenderTarget(name, target);
	}

	struct Readback
	{
		RHIBufferPtr m_buffer;
		uint32_t m_pixels;
		bool m_bIsHalf;
		float Read(size_t pixel, uint32_t channel) const
		{
			if (!m_bIsHalf) return static_cast<const float*>(m_buffer->GetPointer())[pixel];
			const auto words = static_cast<const uint32_t*>(m_buffer->GetPointer());
			return glm::unpackHalf2x16(words[pixel * 2 + channel / 2])[channel % 2];
		}
	};

	Readback Read(RHICommandListPtr command, RHITexturePtr image)
	{
		auto commands = Renderer::GetDriverCommands();
		Readback result{ {}, uint32_t(image->GetExtent().x * image->GetExtent().y), image->GetFormat() == EFormat::R16G16B16A16_SFLOAT };
		result.m_buffer = Renderer::GetDriver()->CreateBuffer(result.m_pixels * (result.m_bIsHalf ? 8 : 4),
			EBufferUsageBit::BufferTransferDst_Bit, EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
		commands->ImageMemoryBarrier(command, image, EImageLayout::TransferSrcOptimal);
		commands->CopyImageToBuffer(command, image, result.m_buffer);
		return result;
	}

	struct RecordedFrame
	{
		RHICommandListPtr m_command;
		std::vector<std::pair<Readback, Readback>> m_images;
		void Complete(std::string_view label)
		{
			Require(Renderer::GetDriver()->SubmitCommandList_Immediate(m_command), "post-processing commands must complete");
			for (const auto& [actual, expected] : m_images)
				for (uint32_t pixel = 0; pixel < actual.m_pixels; ++pixel)
					for (uint32_t channel = 0; channel < (actual.m_bIsHalf ? 4u : 1u); ++channel)
					{
						const float a = actual.Read(pixel, channel), b = expected.Read(pixel, channel);
						if (!std::isfinite(a) || std::abs(a - b) > 0.002f * std::max(1.0f, std::abs(b)))
							throw std::runtime_error(std::string(label) + ": reused node differs from fresh node, actual=" +
								std::to_string(a) + ", expected=" + std::to_string(b));
					}
		}
	};

	void Finish(RecordedFrame& frame)
	{
		auto commands = Renderer::GetDriverCommands();
		commands->MemoryBarrier(frame.m_command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		commands->EndCommandList(frame.m_command);
	}

	void TestBloomThreshold(ShaderSetPtr shader)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		for (bool named : { true, false })
		for (uint32_t branch = 0; branch < 3; ++branch)
		{
			auto input = Texture(19, 1, EFormat::R16G16B16A16_SFLOAT);
			auto output = Texture(9, 1, EFormat::R16G16B16A16_SFLOAT);
			auto average = Texture(1, 1, EFormat::R32_SFLOAT);
			auto bindings = driver->CreateShaderBindings();
			driver->AddStorageImageToShaderBindings(bindings, "u_input_texture"_h, input, 0);
			driver->AddStorageImageToShaderBindings(bindings, "u_output_image"_h, output, 1);
			driver->AddSamplerToShaderBindings(bindings, "u_average_luminance"_h, average, 2);
			BloomProbe::PushConstantsDownscale payload;
			// Each field assignment must define every byte consumed by the shader.
			std::memset(&payload, 0xa5, sizeof(payload));
			payload.m_threshold = branch == 2 ? glm::vec4(1, 0.5f, 1, 0.5f) : glm::vec4(8, 7.5f, 1, 0.5f);
			payload.m_bIsThresholdEnabled = branch != 0;
			RecordedFrame frame{ driver->CreateCommandList(false, ECommandListQueue::Graphics), {} };
			commands->BeginCommandList(frame.m_command, true);
			Clear(frame.m_command, input, glm::vec4(4, 2, 1, 1));
			Clear(frame.m_command, output, glm::vec4(-17));
			Clear(frame.m_command, average, glm::vec4(1.0f / 9.6f));
			commands->ImageMemoryBarrier(frame.m_command, input, EImageLayout::ComputeRead);
			commands->ImageMemoryBarrier(frame.m_command, output, EImageLayout::ComputeWrite);
			commands->ImageMemoryBarrier(frame.m_command, average, EImageLayout::ShaderReadOnlyOptimal);
			commands->Dispatch(frame.m_command, named ? shader->GetDebugComputeShaderRHI() : shader->GetComputeShaderRHI(),
				2, 2, 1, { bindings }, &payload, sizeof(payload));
			const auto result = Read(frame.m_command, output);
			Finish(frame);
			Require(driver->SubmitCommandList_Immediate(frame.m_command), "Bloom threshold dispatch must complete");
			// A constant field survives downsampling; threshold removes one unit
			// from its peak before the first-mip Karis compression.
			glm::vec4 expected = branch == 0 ? glm::vec4(4, 2, 1, 1) : glm::vec4(0, 0, 0, 1);
			if (branch == 2)
				expected = glm::vec4(3, 1.5f, 0.75f, 1) /
					(1.0f + glm::dot(glm::vec3(3, 1.5f, 0.75f), glm::vec3(0.2126729f, 0.7151522f, 0.0721750f)));
			for (uint32_t pixel = 0; pixel < result.m_pixels; ++pixel)
			for (uint32_t channel = 0; channel < 4; ++channel)
			{
				const float actual = result.Read(pixel, channel);
				if (!std::isfinite(actual) || std::abs(actual - expected[channel]) > 0.002f)
					throw std::runtime_error(std::format("Bloom threshold named={} branch={} pixel={} channel={}: actual={}, expected={}",
						named, branch, pixel, channel, actual, expected[channel]));
			}
			std::cout << "Bloom threshold named=" << named << " branch=" << branch
				<< ": poisoned payload, all pixels and independent reference passed\n";
		}
	}

	void TestBloom(ShaderSetPtr down, ShaderSetPtr up, bool named, bool surface)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = RHIFrameGraphPtr::Make();
		auto node = TRefPtr<BloomProbe>::Make();
		auto configure = [&](BloomProbe& bloom)
		{
			bloom.Shaders(down, up);
			bloom.SetVec4("threshold"_h, glm::vec4(0.1f));
			bloom.SetVec4("knee"_h, glm::vec4(0.05f));
			bloom.SetVec4("scatter"_h, glm::vec4(0.5f));
			bloom.SetVec4("bloomIntensity"_h, glm::vec4(0.75f));
			bloom.SetVec4("dirtIntensity"_h, glm::vec4(0.5f));
		};
		configure(*node);
		if (named)
		{
			node->SetRHIResource_Unresolved("bloom"_h, "BloomOutput"_h);
			node->SetRHIResource_Unresolved("averageLuminanceSampler"_h, "BloomAverage"_h);
		}
		auto target = Texture(16, 3, EFormat::R16G16B16A16_SFLOAT);
		auto average = Texture(1, 1, EFormat::R32_SFLOAT);
		auto dirt = Texture(4, 1, EFormat::R16G16B16A16_SFLOAT);
		RHIShaderBindingSetPtr warmBinding;
		GraphicsDriver::Vulkan::VulkanDescriptorSetPtr warmDescriptor;
		RecordedFrame pending;
		for (uint32_t index = 0; index < 9; ++index)
		{
			if (index == 2 || index == 7) average = Texture(1, 1, EFormat::R32_SFLOAT);
			if (index == 3 || index == 7) dirt = Texture(4, 1, EFormat::R16G16B16A16_SFLOAT);
			if (index == 4) target = Texture(32, 5, EFormat::R16G16B16A16_SFLOAT);
			if (index == 5 || index == 7) target = Texture(8, 2, EFormat::R16G16B16A16_SFLOAT);
			if (index == 8) { node->Clear(); configure(*node); }
			Publish(*node, graph, "bloom"_h, "BloomOutput"_h, target, named, surface);
			Publish(*node, graph, "averageLuminanceSampler"_h, "BloomAverage"_h, average, named, false);
			graph->SetSampler("g_lensDirtSampler"_h, dirt);
			auto referenceTarget = Texture(target->GetExtent().x, target->GetMipLevels(), target->GetFormat());
			auto reference = TRefPtr<BloomProbe>::Make();
			configure(*reference);
			reference->SetRHIResource("bloom"_h, referenceTarget);
			reference->SetRHIResource("averageLuminanceSampler"_h, average);
			RecordedFrame frame{ driver->CreateCommandList(false, ECommandListQueue::Graphics), {} };
			commands->BeginCommandList(frame.m_command, true);
			Clear(frame.m_command, target, glm::vec4(4, 2, 1, 1));
			Clear(frame.m_command, referenceTarget, glm::vec4(4, 2, 1, 1));
			Clear(frame.m_command, average, glm::vec4(index < 2 ? 0.5f : index < 7 ? 0.25f : 1.0f));
			Clear(frame.m_command, dirt, glm::vec4(index < 3 ? 0.125f : index < 7 ? 0.75f : 0.25f));
			RHISceneViewSnapshot scene;
			reference->Process(graph, frame.m_command, frame.m_command, scene);
			node->Process(graph, frame.m_command, frame.m_command, scene);
			for (uint32_t mip = 0; mip < target->GetMipLevels(); ++mip)
				frame.m_images.emplace_back(Read(frame.m_command, target->GetMipLayer(mip)), Read(frame.m_command, referenceTarget->GetMipLayer(mip)));
			Finish(frame);
			if (index == 0)
			{
				warmBinding = node->FirstBinding();
				warmDescriptor = warmBinding ? warmBinding->m_vulkan.m_descriptorSet : nullptr;
			}
			if (index == 1)
				Require(warmBinding && node->FirstBinding() == warmBinding && warmBinding->m_vulkan.m_descriptorSet == warmDescriptor,
					"unchanged Bloom resources must reuse RHI bindings and native descriptor sets");
			if (index == 6) { pending = std::move(frame); continue; }
			if (index == 7) pending.Complete("Bloom retained flight");
			frame.Complete("Bloom frame " + std::to_string(index));
			Require(frame.m_images[0].second.Read(0, 0) > 4.1f, "fresh Bloom reference must add visible energy");
		}
		std::cout << "Bloom resources named=" << named << " surface=" << surface
			<< ": nine frames, all mip pixels, warm reuse, replacement, retained flights and Clear passed\n";
	}

	void TestExposure(ShaderSetPtr histogram, ShaderSetPtr averageShader, bool named, bool surface)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = RHIFrameGraphPtr::Make();
		auto node = TRefPtr<ExposureProbe>::Make();
		auto configure = [&](ExposureProbe& exposure)
		{
			exposure.Shaders(histogram, averageShader);
			exposure.SetVec4("metering"_h, glm::vec4(0, 0, 1, 0));
			exposure.SetVec4("adaptation"_h, glm::vec4(-16, 16, 1000, 1000));
		};
		configure(*node);
		if (named)
		{
			node->SetRHIResource_Unresolved("hdrColor"_h, "MeterInput"_h);
			node->SetRHIResource_Unresolved("averageLuminance"_h, "MeterOutput"_h);
		}
		auto input = Texture(19, 1, EFormat::R16G16B16A16_SFLOAT);
		auto output = Texture(1, 1, EFormat::R32_SFLOAT);
		RecordedFrame pending;
		for (uint32_t index = 0; index < 6; ++index)
		{
			if (index == 2 || index == 4) input = Texture(23, 1, EFormat::R16G16B16A16_SFLOAT);
			if (index == 3 || index == 4) output = Texture(1, 1, EFormat::R32_SFLOAT);
			if (index == 5) { node->Clear(); configure(*node); }
			Publish(*node, graph, "hdrColor"_h, "MeterInput"_h, input, named, surface);
			Publish(*node, graph, "averageLuminance"_h, "MeterOutput"_h, output, named, false);
			auto referenceOutput = Texture(1, 1, EFormat::R32_SFLOAT);
			auto reference = TRefPtr<ExposureProbe>::Make();
			configure(*reference);
			reference->SetRHIResource("hdrColor"_h, input);
			reference->SetRHIResource("averageLuminance"_h, referenceOutput);
			RecordedFrame frame{ driver->CreateCommandList(false, ECommandListQueue::Graphics), {} };
			commands->BeginCommandList(frame.m_command, true);
			Clear(frame.m_command, input, glm::vec4(index < 2 ? 0.5f : index < 4 ? 4.0f : 16.0f));
			if (index == 0 || index == 3 || index == 4) Clear(frame.m_command, output, glm::vec4(-17));
			RHISceneViewSnapshot scene;
			scene.m_deltaTime = 1;
			reference->Process(graph, frame.m_command, frame.m_command, scene);
			node->Process(graph, frame.m_command, frame.m_command, scene);
			frame.m_images.emplace_back(Read(frame.m_command, output), Read(frame.m_command, referenceOutput));
			Finish(frame);
			if (index == 3) { pending = std::move(frame); continue; }
			if (index == 4) pending.Complete("Exposure retained flight");
			frame.Complete("Exposure frame " + std::to_string(index));
			Require(frame.m_images[0].second.Read(0, 0) > 0.1f, "fresh exposure reference must meter the HDR input");
		}
		std::cout << "Exposure resources named=" << named << " surface=" << surface
			<< ": six frames, replaced input/output, retained flights and Clear passed\n";
	}
}

namespace Sailor::Tests
{
	void RunPostProcessingCommandTests()
	{
		const auto down = Load("Shaders/ComputeBloomDownscale.shader"), up = Load("Shaders/ComputeBloomUpscale.shader");
		const auto histogram = Load("Shaders/ComputeHistogram.shader"), average = Load("Shaders/ComputeAverageLuminance.shader");
		auto task = Tasks::CreateTaskWithResult<std::string>("Post-processing resource replacement"_h, [=]()
		{
			std::string failures;
			try { TestBloomThreshold(down); }
			catch (const std::exception& error) { failures += std::format("{}\n", error.what()); }
			for (bool named : { false, true })
			for (bool surface : { false, true })
			{
				try { TestBloom(down, up, named, surface); }
				catch (const std::exception& error)
				{
					failures += std::format("Bloom named={} surface={}: {}\n", named, surface, error.what());
				}
				try { TestExposure(histogram, average, named, surface); }
				catch (const std::exception& error)
				{
					failures += std::format("Exposure named={} surface={}: {}\n", named, surface, error.what());
				}
			}
			return failures;
		}, EThreadType::Render);
		task->Run();
		task->Wait();
		Require(task->GetResult().empty(), task->GetResult());
	}
}
