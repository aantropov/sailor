#include "Components/Tests/RenderSceneBindingPublicationTestComponent.h"
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

	struct PublishedClone
	{
		RHIShaderBindingSetPtr m_clone;
		VulkanDescriptorSetPtr m_native;
		RHIShaderBindingSetPtr m_source;
		uint64_t m_sourceRevision;
		RHITexturePtr m_transmission;
		RHITexturePtr m_depth;
		RHITexturePtr m_cells;

		explicit PublishedClone(const RenderSceneProbe::SubmissionResources& resources) :
			m_clone(resources.m_nodeLightsBindings),
			m_native(m_clone ? m_clone->m_vulkan.m_descriptorSet : nullptr),
			m_source(resources.m_nodeLightsSource), m_sourceRevision(resources.m_nodeLightsSourceRevision),
			m_transmission(resources.m_transmissionTexture), m_depth(resources.m_sceneDepthTexture),
			m_cells(resources.m_globalIlluminationProbeCellIndicesTexture)
		{}

		bool Unchanged(const RenderSceneProbe::SubmissionResources& resources) const
		{
			return resources.m_nodeLightsBindings == m_clone && m_clone &&
				m_clone->m_vulkan.m_descriptorSet == m_native && m_native && m_native->IsCompiled() &&
				resources.m_nodeLightsSource == m_source && resources.m_nodeLightsSourceRevision == m_sourceRevision &&
				resources.m_transmissionTexture == m_transmission && resources.m_sceneDepthTexture == m_depth &&
				resources.m_globalIlluminationProbeCellIndicesTexture == m_cells;
		}
	};

	struct RestoreResource
	{
		RHITexturePtr m_texture;
		VulkanImageViewPtr m_view;
		RHIShaderBindingPtr m_binding;
		Memory::TManagedMemoryPtr<Memory::VulkanBufferMemoryPtr, RHIShaderBinding::VulkanBufferAllocator> m_buffer;

		void Restore()
		{
			if (m_texture) m_texture->m_vulkan.m_imageView = m_view;
			if (m_binding) m_binding->m_vulkan.m_valueBinding = m_buffer;
		}
		~RestoreResource() { Restore(); }
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
		auto transmission = driver->CreateTexture(pixels.data(), sizeof(pixels), glm::ivec3(Side, Side, 1),
			1u, ETextureType::Texture2D, EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		const auto textureView = [&](RHITexturePtr texture)
		{
			auto result = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false, texture->GetDefaultLayout());
			result->m_vulkan = texture->m_vulkan;
			return result;
		};
		auto sceneDepth = textureView(transmission);
		auto cells = textureView(transmission);

		auto setup = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(setup, true);
		auto color = driver->CreateRenderTarget(setup, glm::ivec2(Side), 1u, EFormat::R32G32B32A32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		auto depth = driver->CreateRenderTarget(setup, glm::ivec2(Side), 1u, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
		auto storageImage = driver->CreateRenderTarget(setup, glm::ivec2(Side), 1u, EFormat::R32G32B32A32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::Storage_Bit);
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
		if (!driver->AddSamplerToShaderBindings(scene.m_rhiLightsData, "copiedInput", transmission, 7u))
			return "RenderScene initial lighting source could not be created";
		auto graph = RHIFrameGraphPtr::Make();
		graph->SetRenderTarget("DepthBuffer", depth);
		auto node = TRefPtr<RenderSceneProbe>::Make();
		node->SetString("Tag", "BindingPublication");
		node->SetString("GPUCulling", "false");
		node->SetRHIResource("color", color);
		node->SetRHIResource("transmissionFramebuffer", transmission);
		node->SetRHIResource("sceneDepth", sceneDepth);
		node->SetRHIResource("globalIlluminationProbeCellIndicesSampler", cells);
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

		const auto draw = [&](const char* name, const PublishedClone* rejected = nullptr) -> std::string
		{
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
			const bool unchanged = rejected && rejected->Unchanged(*resources);
			const bool expectedDraw = rejected ? stats.m_numBatches == 0u && stats.m_numInstances == 0u :
				stats.m_numBatches == 1u && stats.m_numInstances == 1u;
			const auto discard = [&]()
			{
				commands->EndCommandList(upload);
				commands->EndCommandList(graphics);
				upload->m_vulkan.m_commandBuffer->Reset();
				graphics->m_vulkan.m_commandBuffer->Reset();
			};
			if (!expectedDraw || (rejected && !unchanged))
			{
				// Old code publishes an incomplete clone. Never submit its recorded draw.
				discard();
				return std::format("{}: cache unchanged={}, batches={}, instances={} (expected {} draw)",
					name, unchanged, stats.m_numBatches, stats.m_numInstances, rejected ? "no" : "one");
			}
			if (!rejected && (!resources->m_nodeLightsBindings || !resources->m_nodeLightsBindings->m_vulkan.m_descriptorSet ||
				!resources->m_nodeLightsBindings->m_vulkan.m_descriptorSet->IsCompiled() ||
				resources->m_nodeLightsSource != scene.m_rhiLightsData ||
				resources->m_nodeLightsSourceRevision != scene.m_rhiLightsData->GetDescriptorRevision() ||
				resources->m_transmissionTexture != transmission || resources->m_sceneDepthTexture != sceneDepth ||
				resources->m_globalIlluminationProbeCellIndicesTexture != cells))
			{
				discard();
				return std::format("{}: clone did not publish the complete current request", name);
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
					const float expected = rejected ? sentinel[channel] : pixels[pixel][channel];
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
		const char* cases[] = { "copied sampler", "copied storage image", "copied buffer", "unused GI override" };
		for (uint32_t scenario = 0u; scenario < 4u; ++scenario)
		{
			const PublishedClone before(*resources);
			RestoreResource restore;
			if (scenario < 3u)
			{
				scene.m_rhiLightsData = driver->CreateShaderBindings();
				if (scenario < 2u)
				{
					restore.m_texture = textureView(scenario == 0u ? transmission : storageImage);
					auto added = scenario == 0u ?
						driver->AddSamplerToShaderBindings(scene.m_rhiLightsData, "copiedInput", restore.m_texture, 7u) :
						driver->AddStorageImageToShaderBindings(scene.m_rhiLightsData, "copiedInput", restore.m_texture, 7u);
					if (!added) return std::format("{}: valid source could not be created", cases[scenario]);
					restore.m_view = restore.m_texture->m_vulkan.m_imageView;
					restore.m_texture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, restore.m_texture->m_vulkan.m_image);
				}
				else
				{
					auto buffer = driver->CreateBuffer(16u, EBufferUsageBit::StorageBuffer_Bit, HostMemory);
					restore.m_binding = driver->AddBufferToShaderBindings(scene.m_rhiLightsData, buffer, "copiedInput", 7u);
					if (!restore.m_binding) return "copied buffer: valid source could not be created";
					restore.m_buffer = restore.m_binding->m_vulkan.m_valueBinding;
					auto uncompiled = VulkanBufferPtr::Make(device, 16u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE);
					auto range = restore.m_buffer->Get();
					range.m_ptr.m_buffer = uncompiled;
					restore.m_binding->m_vulkan.m_valueBinding = decltype(restore.m_buffer)::Make(range,
						TWeakPtr<RHIShaderBinding::VulkanBufferAllocator>{});
				}
			}
			else
			{
				cells = textureView(transmission);
				node->SetRHIResource("globalIlluminationProbeCellIndicesSampler", cells);
				restore.m_texture = cells;
				restore.m_view = cells->m_vulkan.m_imageView;
				cells->m_vulkan.m_imageView = VulkanImageViewPtr::Make(device, cells->m_vulkan.m_image);
			}
			const auto requestSource = scene.m_rhiLightsData;
			const uint64_t requestRevision = requestSource->GetDescriptorRevision();
			if (auto error = draw(cases[scenario], &before); !error.empty()) return error;
			restore.Restore();
			if (scene.m_rhiLightsData != requestSource || requestSource->GetDescriptorRevision() != requestRevision)
				return "repair unexpectedly changed the source identity/revision";
			if (auto error = draw("same-request C"); !error.empty()) return std::format("{}: {}", cases[scenario], error);
			if (resources->m_nodeLightsBindings == before.m_clone ||
				resources->m_nodeLightsBindings->m_vulkan.m_descriptorSet == before.m_native ||
				!resources->m_nodeLightsBindings->HasBinding("copiedInput"))
				return std::format("{}: retry did not publish a new complete clone", cases[scenario]);
			const PublishedClone stable(*resources);
			if (auto error = draw("stable request"); !error.empty()) return error;
			if (!stable.Unchanged(*resources)) return "unchanged successful request republished its clone";
		}
		scene.m_rhiLightsData = driver->CreateShaderBindings();
		if (auto error = draw("nonnull empty source"); !error.empty()) return error;
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
			"Real nonempty Process passed A/failed B/same-request C/stable draw for sampler, storage image, buffer and unused GI override; all 8x8 pixels, rejected-draw sentinels and nonnull empty source passed with resolved current-MSAA output");
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
		m_validation = Tasks::CreateTaskWithResult<std::string>("RenderScene binding publication validation",
			[shader = m_shader]() { return ValidatePublication(shader); }, EThreadType::RHI);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("RenderScene binding publication shader did not become ready within 30 seconds");
	}
}
