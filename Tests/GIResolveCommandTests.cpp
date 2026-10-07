#include "Sailor.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/GlobalIlluminationResolveNode.h"
#include "FrameGraph/RHIFrameGraph.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/GlobalIllumination.h"
#include "RHI/RenderTarget.h"
#include "RHI/Renderer.h"
#include "RHI/SceneView.h"
#include "RHI/Shader.h"
#include "RHI/Surface.h"
#include <array>
#include <bit>
#include <format>
#include <iostream>
#include <stdexcept>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::Framegraph;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	RHIShaderBindingSetPtr CreateProbeCells()
	{
		RHIGlobalIlluminationGpuHeader header{};
		header.m_counts = { 1, 3, 2, 16 };
		header.m_stateAndDebug = { 1, 0, 0, 0 };
		header.m_settings = { 1, 2, std::bit_cast<uint32_t>(1.0f), 0 };
		header.m_volumeMin = { -2, -2, 0, 0 };
		header.m_volumeMax = { 2, 2, 1, 0 };
		std::array<RHIGlobalIlluminationGpuBvhNode, 3> nodes{};
		nodes[0].m_minAndLeft = { -2, -2, 0, std::bit_cast<float>(1u) };
		nodes[0].m_maxAndRight = { 2, 2, 1, std::bit_cast<float>(2u) };
		std::array<RHIGlobalIlluminationGpuBrick, 2> bricks{};
		for (uint32_t i = 0; i < bricks.size(); ++i)
		{
			bricks[i].m_minAndSubdivision = { -2, -2, float(i) * 0.5f, 0 };
			bricks[i].m_maxAndFirstProbe = { 2, 2, float(i + 1) * 0.5f, std::bit_cast<float>(i * 8) };
			bricks[i].m_probeCountsAndValidCount = { 2, 2, 2, 8 };
			nodes[i + 1].m_minAndLeft = glm::vec4(glm::vec3(bricks[i].m_minAndSubdivision), std::bit_cast<float>(0x80000000u | i));
			nodes[i + 1].m_maxAndRight = glm::vec4(glm::vec3(bricks[i].m_maxAndFirstProbe), 0);
		}
		auto& driver = Renderer::GetDriver();
		auto bindings = driver->CreateShaderBindings();
		const auto add = [&](StringHash name, uint32_t slot, const void* data, size_t size)
		{
			auto buffer = driver->CreateBuffer_Immediate(data, size, EBufferUsageBit::StorageBuffer_Bit);
			Require(buffer.IsValid(), "GI resolve fixture buffer must upload");
			driver->AddBufferToShaderBindings(bindings, buffer, name, slot);
		};
		add("globalIlluminationHeader"_h, 12, &header, sizeof(header));
		add("globalIlluminationBvh"_h, 13, nodes.data(), sizeof(nodes));
		add("globalIlluminationBricks"_h, 14, bricks.data(), sizeof(bricks));
		return bindings;
	}

	RHIRenderTargetPtr Texture(uint32_t side, EFormat format)
	{
		const auto attachment = IsDepthFormat(format) ? ETextureUsageBit::DepthStencilAttachment_Bit : ETextureUsageBit::ColorAttachment_Bit;
		return Renderer::GetDriver()->CreateRenderTarget(glm::ivec2(side), 1, format,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			attachment | ETextureUsageBit::Sampled_Bit | ETextureUsageBit::TextureTransferSrc_Bit |
			ETextureUsageBit::TextureTransferDst_Bit | (IsDepthFormat(format) ? 0 : ETextureUsageBit::Storage_Bit));
	}

	void Clear(RHICommandListPtr command, RHIRenderTargetPtr target, float value)
	{
		auto commands = Renderer::GetDriverCommands();
		commands->ImageMemoryBarrier(command, target, EImageLayout::TransferDstOptimal);
		if (IsDepthFormat(target->GetFormat())) commands->ClearDepthStencil(command, target, value, 0);
		else commands->ClearImage(command, target, glm::vec4(value));
	}

	struct RecordedFrame
	{
		RHICommandListPtr m_command;
		RHIBufferPtr m_pixels;
		uint32_t m_side{};
		float m_expected{};
		void Complete()
		{
			Require(Renderer::GetDriver()->SubmitCommandList_Immediate(m_command), "GI resolve commands must complete");
			const auto pixels = static_cast<const glm::vec4*>(m_pixels->GetPointer());
			for (uint32_t pixel = 0; pixel < m_side * m_side; ++pixel)
				if (pixels[pixel] != glm::vec4(m_expected))
					throw std::runtime_error(std::format("GI resolve pixel {}: ({}, {}, {}, {}), expected {}",
						pixel, pixels[pixel].x, pixels[pixel].y, pixels[pixel].z, pixels[pixel].w, m_expected));
		}
	};

	void TestResolve(bool named, bool surface, bool depthAspect)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto graph = RHIFrameGraphPtr::Make();
		auto node = TRefPtr<GlobalIlluminationResolveNode>::Make();
		if (named)
		{
			node->SetRHIResource_Unresolved("depthSampler"_h, "ResolveDepth"_h);
			node->SetRHIResource_Unresolved("probeCellIndices"_h, "ResolveCells"_h);
		}
		RHISceneViewSnapshot scene;
		scene.m_rhiLightsData = CreateProbeCells();
		scene.m_frameBindings = driver->CreateShaderBindings();
		UboFrameData frameData{};
		frameData.m_view = frameData.m_projection = frameData.m_invProjection = glm::mat4(1);
		auto frameBinding = driver->AddBufferToShaderBindings(scene.m_frameBindings, "frameData"_h, sizeof(frameData), 0, EShaderBindingType::UniformBuffer);
		const auto format = depthAspect ? EFormat::D32_SFLOAT : EFormat::R32_SFLOAT;
		auto input = Texture(22, format);
		auto output = Texture(11, EFormat::R32G32B32A32_SFLOAT);
		RecordedFrame pending;
		for (uint32_t frame = 0; frame < 6; ++frame)
		{
			if (frame >= 2) input = Texture(frame == 5 ? 34 : 26, format);
			if (frame == 3 || frame == 5) output = Texture(frame == 5 ? 17 : 13, output->GetFormat());
			if (frame == 5) node->Clear();
			const auto publish = [&](StringHash slot, StringHash name, RHIRenderTargetPtr texture)
			{
				const auto resource = surface ? RHIResourcePtr(driver->CreateSurface(texture)) : RHIResourcePtr(texture);
				if (!named) node->SetRHIResource(slot, resource);
				else if (surface) graph->SetSurface(name, resource.DynamicCast<RHISurface>());
				else graph->SetRenderTarget(name, texture);
			};
			publish("depthSampler"_h, "ResolveDepth"_h, input);
			publish("probeCellIndices"_h, "ResolveCells"_h, output);
			const float depth = frame == 4 ? 0.0f : frame == 2 || frame == 5 ? 0.75f : 0.25f;
			RecordedFrame recording;
			recording.m_command = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			recording.m_side = output->GetExtent().x;
			recording.m_expected = depth == 0 ? 0 : depth < 0.5f ? 1 : 2;
			recording.m_pixels = driver->CreateBuffer(recording.m_side * recording.m_side * sizeof(glm::vec4),
				EBufferUsageBit::BufferTransferDst_Bit, EMemoryPropertyBit::HostCoherent | EMemoryPropertyBit::HostVisible);
			commands->BeginCommandList(recording.m_command, true);
			commands->UpdateShaderBinding(recording.m_command, frameBinding, &frameData, sizeof(frameData));
			Clear(recording.m_command, input, depth);
			Clear(recording.m_command, output, -17);
			node->Process(graph, recording.m_command, recording.m_command, scene);
			commands->ImageMemoryBarrier(recording.m_command, output, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(recording.m_command, output, recording.m_pixels);
			commands->MemoryBarrier(recording.m_command, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(recording.m_command);
			if (frame == 2) { pending = std::move(recording); continue; }
			if (frame == 3) pending.Complete();
			recording.Complete();
		}
		std::cout << "GI resolve named=" << named << " surface=" << surface << " depthAspect=" << depthAspect
			<< ": six frames, exact brick indices, replacement, retained commands and Clear passed\n";
	}
}

namespace Sailor::Tests
{
	void RunGIResolveCommandTests()
	{
		const auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/GlobalIlluminationResolve.shader");
		ShaderSetPtr shader;
		Require(info && App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(info->GetFileId(), shader) && shader->IsReady(),
			"production GI resolve shader must load before recording");
		auto task = Tasks::CreateTaskWithResult<std::string>("GI resolve resource contracts"_h, []()
		{
			std::string failures;
			for (bool named : { false, true })
			for (bool surface : { false, true })
			for (bool depthAspect : { false, true })
			{
				try { TestResolve(named, surface, depthAspect); }
				catch (const std::exception& error)
				{
					failures += std::format("GI resolve named={} surface={} depthAspect={}: {}\n", named, surface, depthAspect, error.what());
				}
			}
			return failures;
		}, EThreadType::Render);
		task->Run();
		task->Wait();
		Require(task->GetResult().empty(), task->GetResult());
	}
}
