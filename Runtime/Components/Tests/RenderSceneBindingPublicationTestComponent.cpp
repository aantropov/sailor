#include "Components/Tests/RenderSceneBindingPublicationTestComponent.h"
#include "Platform/Time.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/RenderSceneNode.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/RenderTarget.h"
#include "RHI/VertexDescription.h"
#include <array>
#include <cmath>
#include <cstring>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	constexpr uint32_t Side = 8u;
	const EMemoryPropertyFlags HostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;

	class RenderSceneProbe final : public Framegraph::RenderSceneNode
	{
	public:
		using Framegraph::RenderSceneNode::SubmissionResources;

		TRefPtr<SubmissionResources> GetResources(const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, scene.m_cameraIndex, 0u);
		}
	};

	struct PublishedBindings
	{
		RHIShaderBindingSetPtr m_bindings;
		VulkanDescriptorSetPtr m_native;
		uint64_t m_revision;

		explicit PublishedBindings(RHIShaderBindingSetPtr bindings) :
			m_bindings(bindings), m_native(bindings->m_vulkan.m_descriptorSet),
			m_revision(bindings->GetDescriptorRevision())
		{}

		bool Unchanged(RHIShaderBindingSetPtr bindings) const
		{
			return bindings == m_bindings && bindings->m_vulkan.m_descriptorSet == m_native &&
				m_native && m_native->IsCompiled() && bindings->GetDescriptorRevision() == m_revision;
		}
	};

	struct RestoreView
	{
		RHITexturePtr m_texture;
		VulkanImageViewPtr m_view;

		explicit RestoreView(RHITexturePtr texture) : m_texture(texture), m_view(texture->m_vulkan.m_imageView) {}
		void Restore() { m_texture->m_vulkan.m_imageView = m_view; }
		~RestoreView() { Restore(); }
	};

	std::string ValidatePublication(ShaderSetPtr shader)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		VkImageFormatProperties formatProperties{};
		if (vkGetPhysicalDeviceImageFormatProperties(device->GetPhysicalDevice(), VK_FORMAT_R32G32B32A32_SFLOAT,
			VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
			VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0u, &formatProperties) != VK_SUCCESS ||
			!(formatProperties.sampleCounts & device->GetCurrentMsaaSamples()))
			return "RenderScene float target does not support the current MSAA sample count";

		std::array<glm::vec4, Side * Side> pixels;
		for (uint32_t y = 0u; y < Side; ++y)
			for (uint32_t x = 0u; x < Side; ++x)
				pixels[y * Side + x] = glm::vec4((x + 1u) / 16.0f, (y + 1u) / 16.0f, (x + y + 1u) / 32.0f, 1.0f);
		auto textureA = driver->CreateTexture(pixels.data(), sizeof(pixels), glm::ivec3(Side, Side, 1),
			1u, ETextureType::Texture2D, EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		auto reversedPixels = pixels;
		for (auto& pixel : reversedPixels) pixel = glm::vec4(1.0f) - pixel;
		auto textureB = driver->CreateTexture(reversedPixels.data(), sizeof(reversedPixels), glm::ivec3(Side, Side, 1),
			1u, ETextureType::Texture2D, EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		const auto textureView = [&](RHITexturePtr texture)
		{
			auto result = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false, texture->GetDefaultLayout());
			result->m_vulkan = texture->m_vulkan;
			return result;
		};
		auto transmission = textureView(textureA);
		auto sceneDepth = textureView(textureA);
		auto cells = textureView(textureA);
		std::array<bool, 4> useTextureB{};

		auto setup = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(setup, true);
		auto color = driver->CreateRenderTarget(setup, glm::ivec2(Side), 1u, EFormat::R32G32B32A32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		auto depth = driver->CreateRenderTarget(setup, glm::ivec2(Side), 1u, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
		commands->EndCommandList(setup);
		auto setupFence = RHIFencePtr::Make();
		if (!driver->SubmitCommandList(setup, setupFence)) return "RenderScene target initialization submission failed";
		setupFence->Wait(5000000000ull);
		if (!setupFence->IsFinished()) return "RenderScene target initialization exceeded five seconds";
		setup->m_vulkan.m_commandBuffer->Reset();

		auto vertices = RHIVertexDescriptionPtr::Make();
		vertices->SetVertexStride(sizeof(glm::vec2));
		const RenderState state(false, false, 0.0f, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0u, true);
		auto material = driver->CreateMaterial(vertices, EPrimitiveTopology::TriangleList, state, shader);
		if (!material || !material->GetVersion() || !material->GetVersion()->GetBindings())
			return "RenderScene test material could not be created";
		auto mesh = RHIMeshPtr::Make();
		mesh->m_vertexDescription = vertices;
		const glm::vec2 positions[] = { {-1, -1}, {3, -1}, {-1, 3} };
		const uint32_t indices[] = { 0u, 1u, 2u };
		mesh->m_vertexBuffer = driver->CreateBuffer(sizeof(positions), EBufferUsageBit::VertexBuffer_Bit, HostMemory);
		mesh->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
		std::memcpy(mesh->m_vertexBuffer->GetPointer(), positions, sizeof(positions));
		std::memcpy(mesh->m_indexBuffer->GetPointer(), indices, sizeof(indices));

		RHISceneViewSnapshot scene;
		scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
		scene.m_submissionContext->BeginSubmission(78u, 0u);
		scene.m_frameBindings = driver->CreateShaderBindings();
		scene.m_rhiLightsData = driver->CreateShaderBindings();
		if (!driver->AddSamplerToShaderBindings(scene.m_rhiLightsData, "lightingSampler"_h, textureA, 7u))
			return "RenderScene initial lighting source could not be created";
		auto graph = RHIFrameGraphPtr::Make();
		graph->SetRenderTarget("DepthBuffer"_h, depth);
		auto node = TRefPtr<RenderSceneProbe>::Make();
		node->SetString("Tag"_h, "BindingPublication");
		node->SetString("GPUCulling"_h, "false");
		node->SetRHIResource("color"_h, color);
		node->SetRHIResource("transmissionFramebuffer"_h, transmission);
		node->SetRHIResource("sceneDepth"_h, sceneDepth);
		node->SetRHIResource("globalIlluminationProbeCellIndicesSampler"_h, cells);
		auto resources = node->GetResources(scene);
		RHIBatch batch(material, mesh);
		batch.m_textureBindings = driver->CreateShaderBindings();
		RenderSceneProbe::PerInstanceData instance{};
		instance.model = glm::mat4(1.0f);
		instance.sphereBounds = glm::vec4(0, 0, 0, 1);
		resources->m_packet.Add(batch, mesh, instance);
		resources->m_packet.Finalize();
		if (resources->m_packet.GetNumInstances() != 1u || resources->m_packet.GetGroups().Num() != 1u)
			return "RenderScene fixture did not prepare one real draw group";

		const auto draw = [&](const char* name, const PublishedBindings* rejected = nullptr) -> std::string
		{
			const PublishedBindings lights(scene.m_rhiLightsData);
			auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(upload, true);
			commands->BeginCommandList(graphics, true);
			upload->m_vulkan.m_commandBuffer->AddDependency(scene.m_submissionContext);
			graphics->m_vulkan.m_commandBuffer->AddDependency(scene.m_submissionContext);
			commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			const glm::vec4 sentinel(-1.0f);
			commands->ImageMemoryBarrier(graphics, color, EImageLayout::TransferDstOptimal);
			commands->ClearImage(graphics, color, sentinel);
			node->Process(graph, upload, graphics, scene);
			const auto stats = node->GetDrawCallStats();
			const bool unchanged = rejected && rejected->Unchanged(resources->m_perInstanceData);
			const bool expectedDraw = rejected ? stats.m_numBatches == 0u && stats.m_numInstances == 0u :
				stats.m_numBatches == 1u && stats.m_numInstances == 1u;
			const auto discard = [&]()
			{
				commands->EndCommandList(upload);
				commands->EndCommandList(graphics);
				upload->m_vulkan.m_commandBuffer->Reset();
				graphics->m_vulkan.m_commandBuffer->Reset();
			};
			if (!expectedDraw || (rejected && !unchanged) || !lights.Unchanged(scene.m_rhiLightsData))
			{
				discard();
				return std::format("{}: cache unchanged={}, batches={}, instances={} (expected {} draw)",
					name, unchanged, stats.m_numBatches, stats.m_numInstances, rejected ? "no" : "one");
			}
			if (!rejected && (!resources->m_perInstanceData ||
				!resources->m_perInstanceData->m_vulkan.m_descriptorSet ||
				!resources->m_perInstanceData->m_vulkan.m_descriptorSet->IsCompiled() ||
				resources->m_perInstanceData->GetShaderBindings().Num() != 5u ||
				resources->m_perInstanceData->GetOrAddShaderBinding("g_transmissionFramebufferSampler"_h)->GetTextureBinding() != transmission ||
				resources->m_perInstanceData->GetOrAddShaderBinding("g_sceneDepthSampler"_h)->GetTextureBinding() != sceneDepth ||
				resources->m_perInstanceData->GetOrAddShaderBinding("g_globalIlluminationProbeCellIndicesSampler"_h)->GetTextureBinding() != cells))
			{
				discard();
				return std::format("{}: pass bindings did not publish the current inputs", name);
			}
			auto readback = driver->CreateBuffer(sizeof(pixels), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
			commands->ImageMemoryBarrier(graphics, color, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(graphics, color, readback);
			commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
			commands->EndCommandList(upload);
			commands->EndCommandList(graphics);
			auto ready = driver->CreateWaitSemaphore();
			auto uploadFence = RHIFencePtr::Make();
			auto graphicsFence = RHIFencePtr::Make();
			if (!driver->SubmitCommandList(upload, uploadFence, ready))
				return std::format("{}: packet upload submission failed", name);
			if (!driver->SubmitCommandList(graphics, graphicsFence, nullptr, ready))
				return std::format("{}: graphics submission failed", name);
			graphicsFence->Wait(5000000000ull);
			uploadFence->Wait(5000000000ull);
			if (!graphicsFence->IsFinished() || !uploadFence->IsFinished())
				return std::format("{}: upload/draw did not finish within five seconds per fence", name);
			const auto* actual = static_cast<const float*>(readback->GetPointer());
			for (uint32_t pixel = 0u; pixel < pixels.size(); ++pixel)
			{
				for (uint32_t channel = 0u; channel < 4u; ++channel)
				{
					const float expected = rejected ? sentinel[channel] :
						(useTextureB[channel] ? reversedPixels[pixel][channel] : pixels[pixel][channel]);
					const float value = actual[pixel * 4u + channel];
					if (!std::isfinite(value) || std::abs(value - expected) > 0.00001f)
						return std::format("{} pixel {} channel {}: expected {}, got {}", name, pixel, channel, expected, value);
				}
			}
			upload->m_vulkan.m_commandBuffer->Reset();
			graphics->m_vulkan.m_commandBuffer->Reset();
			return {};
		};

		if (auto error = draw("initial A"); !error.empty()) return error;
		const StringHash inputs[] = { "transmissionFramebuffer"_h, "sceneDepth"_h, "globalIlluminationProbeCellIndicesSampler"_h };
		RHITexturePtr* textures[] = { &transmission, &sceneDepth, &cells };
		for (uint32_t scenario = 0u; scenario < 3u; ++scenario)
		{
			const PublishedBindings before(resources->m_perInstanceData);
			auto& texture = *textures[scenario];
			texture = textureView(textureB);
			node->SetRHIResource(inputs[scenario], texture);
			RestoreView restore(texture);
			texture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, texture->m_vulkan.m_image);
			if (auto error = draw(inputs[scenario].ToString().c_str(), &before); !error.empty()) return error;
			restore.Restore();
			useTextureB[scenario] = true;
			if (auto error = draw("same-request retry"); !error.empty()) return error;
			if (resources->m_perInstanceData != before.m_bindings ||
				resources->m_perInstanceData->m_vulkan.m_descriptorSet == before.m_native ||
				resources->m_perInstanceData->GetDescriptorRevision() != before.m_revision + 1u || !before.m_native->IsCompiled())
				return std::format("{}: retry did not update exactly one pass binding", inputs[scenario].ToString());
			const PublishedBindings stable(resources->m_perInstanceData);
			if (auto error = draw("stable request"); !error.empty()) return error;
			if (!stable.Unchanged(resources->m_perInstanceData)) return "unchanged pass inputs rebuilt descriptors";
		}

		const PublishedBindings pass(resources->m_perInstanceData);
		if (!driver->AddSamplerToShaderBindings(scene.m_rhiLightsData, "lightingSampler"_h, textureB, 7u))
			return "lighting update failed";
		useTextureB[3] = true;
		if (auto error = draw("lighting revision changed"); !error.empty()) return error;
		if (!pass.Unchanged(resources->m_perInstanceData)) return "lighting revision rebuilt pass bindings";
		scene.m_rhiLightsData = driver->CreateShaderBindings();
		if (!driver->AddSamplerToShaderBindings(scene.m_rhiLightsData, "lightingSampler"_h, textureA, 7u))
			return "replacement lighting set failed";
		useTextureB[3] = false;
		if (auto error = draw("lighting identity changed"); !error.empty()) return error;
		if (!pass.Unchanged(resources->m_perInstanceData)) return "lighting identity rebuilt pass bindings";

		auto firstNode = node;
		auto firstResources = resources;
		node = TRefPtr<RenderSceneProbe>::Make();
		node->SetString("Tag"_h, "BindingPublicationOtherPass");
		node->SetString("GPUCulling"_h, "false");
		node->SetRHIResource("color"_h, color);
		transmission = sceneDepth = cells = textureA;
		for (const auto input : inputs) node->SetRHIResource(input, textureA);
		resources = node->GetResources(scene);
		resources->m_packet.Add(batch, mesh, instance);
		resources->m_packet.Finalize();
		useTextureB.fill(false);
		if (auto error = draw("second pass, same lighting"); !error.empty()) return error;
		if (resources->m_perInstanceData == firstResources->m_perInstanceData ||
			!pass.Unchanged(firstResources->m_perInstanceData)) return "passes did not retain independent local inputs";
		node = firstNode;
		resources = firstResources;
		transmission = resources->m_perInstanceData->GetOrAddShaderBinding("g_transmissionFramebufferSampler"_h)->GetTextureBinding();
		sceneDepth = resources->m_perInstanceData->GetOrAddShaderBinding("g_sceneDepthSampler"_h)->GetTextureBinding();
		cells = resources->m_perInstanceData->GetOrAddShaderBinding("g_globalIlluminationProbeCellIndicesSampler"_h)->GetTextureBinding();
		useTextureB = { true, true, true, false };
		if (auto error = draw("first pass reused"); !error.empty()) return error;
		if (!pass.Unchanged(resources->m_perInstanceData)) return "second pass invalidated the first pass";
		return {};
	}
}

void RenderSceneBindingPublicationTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (m_validation)
	{
		if (!m_validation->IsFinished()) return;
		const auto& error = m_validation->GetResult();
		if (!error.empty()) { MarkFailed(error); return; }
		AddJournalEvent("RenderSceneBindingEvidence",
			"All 8x8 pixels passed for shared lighting plus three independent pass samplers; invalid view/retry and warm reuse passed for each sampler. Lighting revision/identity changes preserved pass descriptors; two passes retained separate inputs with shared lighting.");
		MarkPassed();
		return;
	}
	if (!m_shader)
	{
		if (auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Tests/Shaders/RenderSceneBindingPublication.shader"))
			App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_shader);
	}
	if (m_shader && m_shader->IsReady())
	{
		m_validation = Tasks::CreateTaskWithResult<std::string>("RenderScene binding publication validation"_h,
			[shader = m_shader]() { return ValidatePublication(shader); }, EThreadType::RHI);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("RenderScene binding publication shader did not become ready within 30 seconds");
	}
}
