#include "Components/Tests/PushConstantsTestComponent.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "RHI/Buffer.h"
#include "RHI/Fence.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/VertexDescription.h"
#include <cmath>
#include <cstring>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	const EMemoryPropertyFlags HostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;

	bool SubmitAndWait(RHICommandListPtr cmd)
	{
		auto commands = Renderer::GetDriverCommands();
		commands->MemoryBarrier(cmd, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		commands->EndCommandList(cmd);
		auto fence = RHIFencePtr::Make();
		if (!Renderer::GetDriver()->SubmitCommandList(cmd, fence)) return false;
		fence->Wait(5000000000ull);
		return fence->IsFinished();
	}

	std::string ValidateCompute(const std::array<ShaderSetPtr, 2>& shaders)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		std::array<RHIBufferPtr, 4> readbacks;
		for (uint32_t round = 0u; round < 2u; ++round)
		{
			for (uint32_t variant = 0u; variant < shaders.size(); ++variant)
			{
				auto output = driver->CreateRenderTarget(cmd, glm::ivec2(4, 1), 1u, EFormat::R32_SFLOAT,
					ETextureFiltration::Nearest, ETextureClamping::Clamp,
					ETextureUsageBit::Storage_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
				auto bindings = driver->CreateShaderBindings();
				driver->AddStorageImageToShaderBindings(bindings, "outputValue", output, 0u);
				std::array<uint32_t, 8> constants;
				constants.fill(0xdeadbeefu);
				constants[variant == 0u ? 0u : 4u] = round == 0u ? 17u : 43u;
				const uint32_t size = round == 0u ? (variant == 0u ? 4u : 20u) : sizeof(constants);
				commands->ImageMemoryBarrier(cmd, output, EImageLayout::ComputeWrite);
				commands->Dispatch(cmd, shaders[variant]->GetComputeShaderRHI(), 1u, 1u, 1u,
					{ bindings }, constants.data(), size);
				commands->ImageMemoryBarrier(cmd, output, EImageLayout::TransferSrcOptimal);
				auto& readback = readbacks[round * 2u + variant];
				readback = driver->CreateBuffer(4u * sizeof(float), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
				commands->CopyImageToBuffer(cmd, output, readback);
			}
		}
		if (!SubmitAndWait(cmd)) return "push constants compute submission did not finish within five seconds";
		for (uint32_t scenario = 0u; scenario < readbacks.size(); ++scenario)
		{
			const auto* actual = static_cast<const float*>(readbacks[scenario]->GetPointer());
			for (uint32_t index = 0u; index < 4u; ++index)
			{
				const float expected = static_cast<float>((scenario < 2u ? 17u : 43u) + index);
				if (actual[index] != expected)
					return std::format("compute constants scenario {} element {}: expected {}, got {}",
						scenario, index, expected, actual[index]);
			}
		}
		return {};
	}

	std::string ValidateGraphics(const std::array<ShaderSetPtr, 4>& shaders)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto vertices = RHIVertexDescriptionPtr::Make();
		const RenderState state(false, false, 0.0f, false, ECullMode::None,
			EBlendMode::None, EFillMode::Fill, 0u, false);
		std::array<RHIMaterialPtr, 4> materials;
		for (uint32_t variant = 0u; variant < shaders.size(); ++variant)
		{
			materials[variant] = driver->CreateMaterial(vertices, EPrimitiveTopology::TriangleList, state, shaders[variant]);
			if (!materials[variant]) return std::format("push constants graphics material {} could not be created", variant);
		}
		const uint32_t triangle[] = { 0u, 1u, 2u };
		auto indices = driver->CreateBuffer(sizeof(triangle), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
		std::memcpy(indices->GetPointer(), triangle, sizeof(triangle));
		auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(cmd, true);
		commands->MemoryBarrier(cmd, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
		std::array<RHIBufferPtr, 8> readbacks;
		std::array<glm::vec4, 8> expected;
		for (uint32_t scenario = 0u; scenario < readbacks.size(); ++scenario)
		{
			const uint32_t variant = scenario % 4u;
			expected[scenario] = glm::vec4(0.125f * (scenario + 1u), 0.25f, 0.75f, 1.0f);
			auto output = driver->CreateRenderTarget(cmd, glm::ivec2(8, 8), 1u, EFormat::R32G32B32A32_SFLOAT,
				ETextureFiltration::Nearest, ETextureClamping::Clamp,
				ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
			commands->BeginRenderPass(cmd, TVector<RHITexturePtr>{ output }, {}, glm::ivec4(0, 0, 8, 8),
				glm::ivec2(0), true, glm::vec4(-1.0f), 0.0f, false, false);
			commands->BindMaterial(cmd, materials[variant]);
			commands->BindIndexBuffer(cmd, indices, indices->GetOffset());
			commands->SetViewport(cmd, 0, 0, 8, 8, glm::vec2(0), glm::vec2(8), 0, 1);
			std::array<glm::vec4, 3> constants{ glm::vec4(1, 0, 0, 0), expected[scenario], glm::vec4(-100) };
			if (variant < 2u) constants[0] = expected[scenario];
			const uint32_t size = scenario < 4u ? (variant < 2u ? 16u : 32u) : sizeof(constants);
			commands->PushConstants(cmd, materials[variant], size, constants.data());
			commands->DrawIndexed(cmd, 3u);
			commands->EndRenderPass(cmd);
			commands->ImageMemoryBarrier(cmd, output, EImageLayout::TransferSrcOptimal);
			readbacks[scenario] = driver->CreateBuffer(64u * sizeof(glm::vec4), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
			commands->CopyImageToBuffer(cmd, output, readbacks[scenario]);
		}
		if (!SubmitAndWait(cmd)) return "push constants graphics submission did not finish within five seconds";
		for (uint32_t scenario = 0u; scenario < readbacks.size(); ++scenario)
		{
			const auto* actual = static_cast<const float*>(readbacks[scenario]->GetPointer());
			for (uint32_t pixel = 0u; pixel < 64u; ++pixel)
			{
				for (uint32_t channel = 0u; channel < 4u; ++channel)
				{
					const float value = actual[pixel * 4u + channel];
					if (!std::isfinite(value) || std::abs(value - expected[scenario][channel]) > 0.00001f)
						return std::format("graphics constants scenario {} pixel {} channel {}: expected {}, got {}",
							scenario, pixel, channel, expected[scenario][channel], value);
				}
			}
		}
		return {};
	}
}

void PushConstantsTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (m_validation)
	{
		if (!m_validation->IsFinished()) return;
		const auto& error = m_validation->GetResult();
		if (!error.empty()) { MarkFailed(error); return; }
		AddJournalEvent("PushConstantsEvidence",
			"4 compute and 8 graphics readbacks passed: scalar, vertex-only, fragment-only, mixed stages, nonzero offsets and padded host data");
		MarkPassed();
		return;
	}
	auto* registry = App::GetSubmodule<AssetRegistry>();
	auto* compiler = App::GetSubmodule<ShaderCompiler>();
	const auto load = [&](ShaderSetPtr& shader, const char* path, const TVector<std::string>& defines)
	{
		if (!shader)
		{
			if (auto info = registry->GetAssetInfoPtr(path)) compiler->LoadShader(info->GetFileId(), shader, defines);
		}
		return shader && shader->IsReady();
	};
	bool ready = load(m_computeShaders[0], "Tests/Shaders/PushConstantsCompute.shader", {});
	ready &= load(m_computeShaders[1], "Tests/Shaders/PushConstantsCompute.shader", { "NONZERO_OFFSET" });
	ready &= load(m_graphicsShaders[0], "Tests/Shaders/PushConstantsGraphics.shader", {});
	ready &= load(m_graphicsShaders[1], "Tests/Shaders/PushConstantsGraphics.shader", { "VERTEX_COLOR" });
	ready &= load(m_graphicsShaders[2], "Tests/Shaders/PushConstantsGraphics.shader", { "OFFSET_COLOR" });
	ready &= load(m_graphicsShaders[3], "Tests/Shaders/PushConstantsGraphics.shader", { "MIXED_STAGES" });
	if (ready)
	{
		m_validation = Tasks::CreateTaskWithResult<std::string>("Push constants GPU validation",
			[compute = m_computeShaders, graphics = m_graphicsShaders]()
			{
				auto error = ValidateCompute(compute);
				if (error.empty()) error = ValidateGraphics(graphics);
				return error;
			}, EThreadType::RHI);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("push constants validation shaders did not become ready within 30 seconds");
	}
}
