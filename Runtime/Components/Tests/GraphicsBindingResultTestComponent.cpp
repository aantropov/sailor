#include "Components/Tests/GraphicsBindingResultTestComponent.h"
#include "Components/Tests/BufferReadback.h"
#include "Platform/Time.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "FrameGraph/RenderSceneNode.h"
#include "GraphicsDriver/Vulkan/VulkanFramebuffer.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Mesh.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "RHI/VertexDescription.h"
#include <cmath>
#include <cstring>
#include <format>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace
{
	constexpr uint32_t Side = 8u;
	using Pixels = std::array<glm::vec4, Side * Side>;
	const glm::vec4 ColorA(0.125f, 0.25f, 0.5f, 1.0f);
	const glm::vec4 ColorC(0.75f, 0.5f, 0.25f, 1.0f);
	const glm::vec4 EmptyColor(0.25f, 0.75f, 0.125f, 1.0f);
	const glm::vec4 Sentinel(-1.0f);
	const EMemoryPropertyFlags HostMemory = EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent;

	class RenderSceneProbe final : public Framegraph::RenderSceneNode
	{
	public:
		using Framegraph::RenderSceneNode::SubmissionResources;
		TRefPtr<SubmissionResources> GetResources(const RHISceneViewSnapshot& scene)
		{
			return scene.m_submissionContext->GetOrAddFrameGraphResources<SubmissionResources>(this, 0u, 0u);
		}
	};

	class ImGuiProbe : public ImGuiApi
	{
	public:
		using ImGuiApi::ImGui_RenderDrawData;
	};

	struct RestoreView
	{
		RHITexturePtr m_texture;
		VulkanImageViewPtr m_view;
		explicit RestoreView(RHITexturePtr texture) : m_texture(texture), m_view(texture->m_vulkan.m_imageView) {}
		void Restore() { m_texture->m_vulkan.m_imageView = m_view; }
		~RestoreView() { Restore(); }
		void Invalidate()
		{
			m_texture->m_vulkan.m_imageView = VulkanImageViewPtr::Make(
				VulkanApi::GetInstance()->GetMainDevice(), m_texture->m_vulkan.m_image);
		}
	};

	std::string Submit(RHICommandListPtr upload, RHICommandListPtr graphics)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::HostRead_Bit));
		commands->EndCommandList(upload);
		commands->EndCommandList(graphics);
		auto ready = driver->CreateWaitSemaphore();
		auto uploadFence = RHIFencePtr::Make();
		auto graphicsFence = RHIFencePtr::Make();
		if (!driver->SubmitCommandList(upload, uploadFence, ready)) return "upload submission failed";
		if (!driver->SubmitCommandList(graphics, graphicsFence, nullptr, ready)) return "graphics submission failed";
		graphicsFence->Wait(5000000000ull);
		uploadFence->Wait(5000000000ull);
		if (!graphicsFence->IsFinished() || !uploadFence->IsFinished())
			return "submission exceeded five seconds per fence; pending command dependencies retained";
		upload->m_vulkan.m_commandBuffer->Reset();
		graphics->m_vulkan.m_commandBuffer->Reset();
		return {};
	}

	std::string CheckPixels(RHIBufferPtr readback, const Pixels& expected, const char* name)
	{
		const auto* values = static_cast<const float*>(readback->GetPointer());
		for (uint32_t pixel = 0u; pixel < expected.size(); ++pixel)
			for (uint32_t channel = 0u; channel < 4u; ++channel)
			{
				const float value = values[pixel * 4u + channel];
				if (!std::isfinite(value) || std::abs(value - expected[pixel][channel]) > 0.00001f)
					return std::format("{} pixel {} channel {}: expected {}, got {}", name, pixel, channel, expected[pixel][channel], value);
			}
		return {};
	}

	Pixels Solid(glm::vec4 color)
	{
		Pixels result;
		result.fill(color);
		return result;
	}

	enum class AttachmentCase { SingleSample, Multisample, PartialResolve, Legacy };

	std::string ValidateAttachmentLifetime(AttachmentCase testCase, bool submit)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		const std::string name = std::format("AttachmentLifetime_{}_{}", static_cast<uint32_t>(testCase), submit ? "GPU" : "gate");
		const bool multisample = testCase == AttachmentCase::Multisample || testCase == AttachmentCase::PartialResolve;
		const bool hasDepth = testCase == AttachmentCase::SingleSample || testCase == AttachmentCase::Multisample;
		const bool legacy = testCase == AttachmentCase::Legacy;
		const auto samples = multisample ? VK_SAMPLE_COUNT_2_BIT : VK_SAMPLE_COUNT_1_BIT;
		for (VkFormat format : { VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_D32_SFLOAT })
		{
			VkImageFormatProperties properties{};
			const auto usage = format == VK_FORMAT_D32_SFLOAT ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			if (vkGetPhysicalDeviceImageFormatProperties(device->GetPhysicalDevice(), format, VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, 0u, &properties) != VK_SUCCESS ||
				!(properties.sampleCounts & samples))
				return name + ": device lacks the required attachment format/sample count";
		}
		TVector<VulkanImageViewPtr> views;
		auto setup = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(setup, true);
		const auto makeView = [&](VkFormat format, VkSampleCountFlagBits count, VulkanImagePtr& image) -> VulkanImageViewPtr
		{
			const bool depth = format == VK_FORMAT_D32_SFLOAT;
			const auto layout = depth ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			const auto usage = depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
			image = VulkanApi::GetInstance()->CreateImage(device, { Side, Side, 1u }, 1u, VK_IMAGE_TYPE_2D,
				format, VK_IMAGE_TILING_OPTIMAL, usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SHARING_MODE_EXCLUSIVE, count, layout);
			// Transitions retain only the image, never the distinct view being tested.
			setup->m_vulkan.m_commandBuffer->ImageMemoryBarrier(image, format, VK_IMAGE_LAYOUT_UNDEFINED, layout);
			auto view = VulkanImageViewPtr::Make(device, image);
			view->Compile();
			views.Add(view);
			return view;
		};
		TVector<VulkanImageViewPtr> colors, resolves;
		TVector<VulkanImagePtr> outputImages;
		for (uint32_t i = 0u; i < (multisample ? 2u : 1u); ++i)
		{
			VulkanImagePtr image;
			colors.Add(makeView(VK_FORMAT_R32G32B32A32_SFLOAT, samples, image));
			if (!multisample) outputImages.Add(image);
			else if (testCase == AttachmentCase::PartialResolve && i == 1u) resolves.Add({});
			else
			{
				resolves.Add(makeView(VK_FORMAT_R32G32B32A32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, image));
				outputImages.Add(image);
			}
		}
		VulkanImageViewPtr depth, depthResolve;
		if (hasDepth)
		{
			VulkanImagePtr image;
			depth = makeView(VK_FORMAT_D32_SFLOAT, samples, image);
			if (multisample) depthResolve = makeView(VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_1_BIT, image);
			outputImages.Add(image);
		}
		auto emptyUpload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(emptyUpload, true);
		if (auto error = Submit(emptyUpload, setup); !error.empty()) return name + ": " + error;

		VulkanRenderPassPtr renderPass;
		VulkanFramebufferPtr framebuffer;
		if (legacy)
		{
			const VkAttachmentDescription attachment{ 0u, VK_FORMAT_R32G32B32A32_SFLOAT, VK_SAMPLE_COUNT_1_BIT,
				VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
				VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
			VulkanSubpassDescription subpass;
			subpass.m_colorAttachments = { { 0u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL } };
			renderPass = VulkanRenderPassPtr::Make(device, TVector<VkAttachmentDescription>{ attachment },
				TVector<VulkanSubpassDescription>{ subpass }, TVector<VkSubpassDependency>{});
			framebuffer = VulkanFramebufferPtr::Make(renderPass, colors, Side, Side, 1u);
		}
		TVector<RHICommandListPtr> recorded;
		TVector<RHIBufferPtr> readbacks;
		for (uint32_t i = 0u; i < (submit ? 1u : 2u); ++i)
		{
			auto cmd = driver->CreateCommandList(false, ECommandListQueue::Graphics);
			commands->BeginCommandList(cmd, true);
			auto native = cmd->m_vulkan.m_commandBuffer;
			if (legacy)
			{
				VkClearValue clear{};
				std::memcpy(clear.color.float32, &ColorC, sizeof(ColorC));
				native->BeginRenderPass(renderPass, framebuffer, { Side, Side }, VK_SUBPASS_CONTENTS_INLINE, {}, clear);
				native->EndRenderPass();
			}
			else
			{
				native->BeginRenderPassEx(colors, resolves, depth, depthResolve, { {}, { Side, Side } }, 0u, {},
					true, VulkanRenderPassClearValues(ColorC, 0.375f, 0u), true);
				native->EndRenderPassEx();
			}
			if (submit)
			{
				for (const auto& image : outputImages)
				{
					const size_t size = image->m_format == VK_FORMAT_D32_SFLOAT ? Side * Side * sizeof(float) : sizeof(Pixels);
					auto readback = driver->CreateBuffer(size, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
					std::memset(readback->GetPointer(), 0xa7, size);
					native->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
					native->ImageMemoryBarrier(image, image->m_format, image->m_defaultLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
					native->CopyImageToBuffer(*readback->m_vulkan.m_buffer->Get(), image, Side, Side, 1u);
					readbacks.Add(readback);
				}
				native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
			}
			commands->EndCommandList(cmd);
			recorded.Add(cmd);
		}
		bool retained = !legacy || framebuffer.NumRefs() == recorded.Num() + 1u;
		framebuffer.Clear();
		renderPass.Clear();
		colors.Clear();
		resolves.Clear();
		depth.Clear();
		depthResolve.Clear();
		for (const auto& view : views)
			retained &= view.NumRefs() == 1u + (legacy ? 1u : recorded.Num());
		if (!retained)
		{
			for (auto& cmd : recorded) cmd->m_vulkan.m_commandBuffer->Reset();
			return name + ": command list did not retain the attachment view or framebuffer";
		}
		if (submit)
		{
			views.Clear();
			auto fence = RHIFencePtr::Make();
			if (!driver->SubmitCommandList(recorded[0], fence)) return name + ": submission failed";
			fence->Wait(5000000000ull);
			if (!fence->IsFinished()) return name + ": fence timed out; pending dependencies retained";
		}
		for (size_t i = 0u; i < recorded.Num(); ++i)
		{
			recorded[i]->m_vulkan.m_commandBuffer->Reset();
			const size_t remaining = recorded.Num() - i - 1u;
			for (const auto& view : views)
				if (view.NumRefs() != 1u + (legacy ? (remaining != 0u) : remaining))
					return name + ": command reset did not release its attachment reference";
		}
		for (size_t i = 0u; i < readbacks.Num(); ++i)
		{
			if (outputImages[i]->m_format == VK_FORMAT_D32_SFLOAT)
			{
				const auto* pixels = static_cast<const float*>(readbacks[i]->GetPointer());
				for (size_t pixel = 0u; pixel < Side * Side; ++pixel)
					if (!std::isfinite(pixels[pixel]) || std::abs(pixels[pixel] - 0.375f) > 0.00001f)
						return std::format("{} depth pixel {}: expected 0.375, got {}", name, pixel, pixels[pixel]);
			}
			else if (auto error = CheckPixels(readbacks[i], Solid(ColorC), name.c_str()); !error.empty()) return error;
		}
		return {};
	}

	RHITexturePtr PrivateTextureView(RHITexturePtr source)
	{
		auto result = RHITexturePtr::Make(ETextureFiltration::Nearest, ETextureClamping::Clamp, false, source->GetDefaultLayout());
		result->m_vulkan = source->m_vulkan;
		return result;
	}

	RHIShaderBindingSetPtr SamplerSet(RHITexturePtr texture, bool extraBinding)
	{
		auto& driver = Renderer::GetDriver();
		auto result = driver->CreateShaderBindings();
		if (!driver->AddSamplerToShaderBindings(result, "source"_h, texture, 0u)) return {};
		if (extraBinding)
		{
			auto extra = driver->CreateBuffer(16u, EBufferUsageBit::UniformBuffer_Bit, HostMemory);
			if (!driver->AddBufferToShaderBindings(result, extra, "unused"_h, 1u)) return {};
		}
		return result;
	}

	RHIMeshPtr CreateQuad(RHIVertexDescriptionPtr description, float left, float right)
	{
		auto& driver = Renderer::GetDriver();
		auto mesh = RHIMeshPtr::Make();
		mesh->m_vertexDescription = description;
		const glm::vec2 vertices[] = { {left, -1}, {right, -1}, {right, 1}, {left, 1} };
		const uint32_t indices[] = { 0u, 1u, 2u, 0u, 2u, 3u };
		mesh->m_vertexBuffer = driver->CreateBuffer(sizeof(vertices), EBufferUsageBit::VertexBuffer_Bit, HostMemory);
		mesh->m_indexBuffer = driver->CreateBuffer(sizeof(indices), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
		std::memcpy(mesh->m_vertexBuffer->GetPointer(), vertices, sizeof(vertices));
		std::memcpy(mesh->m_indexBuffer->GetPointer(), indices, sizeof(indices));
		return mesh;
	}

	struct SceneDraw
	{
		TRefPtr<RenderSceneProbe> m_node;
		RHISceneViewSnapshot m_scene;
		TRefPtr<RenderSceneProbe::SubmissionResources> m_resources;
		RHIBufferPtr m_readback;
		DrawCallStats m_stats{};
	};

	void AddInstances(SceneDraw& draw, RHIMaterialPtr material, RHIMeshPtr mesh, RHIShaderBindingSetPtr textures, uint32_t count)
	{
		RHIBatch batch(material, mesh);
		batch.m_textureBindings = textures;
		RenderSceneProbe::PerInstanceData instance{};
		instance.model = glm::mat4(1.0f);
		instance.sphereBounds = glm::vec4(0, 0, 0, 1);
		for (uint32_t i = 0u; i < count; ++i) draw.m_resources->m_packet.Add(batch, mesh, instance);
	}

	template<typename TRecord>
	std::string ValidateFlightUploads(std::array<SceneDraw, 5>& draws, RHIMaterialPtr material,
		RHIMeshPtr mesh, RHIShaderBindingSetPtr textures, TRecord&& recordDraw)
	{
		using Instance = RenderSceneProbe::PerInstanceData;
		using PayloadPtr = TPackedDrawPacketPayloadPtr<Instance>;
		constexpr uint32_t StationaryCount = TPackedDrawArenaPage<Instance>::NumInstances + 1u;
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		for (uint32_t numFlights : { 2u, 3u })
		for (bool paged : { false, true })
		{
			auto scene = RHIScenePtr::Make(numFlights);
			TVector<RenderInstanceHandle> handles;
			for (uint32_t i = 0u; i <= StationaryCount; ++i)
			{
				RHISceneInstanceRecord instance;
				instance.m_producerKey = i;
				instance.m_mobility = i < StationaryCount ? EMobilityType::Stationary : EMobilityType::Dynamic;
				instance.m_worldMatrix[3].x = static_cast<float>(i);
				handles.Add(scene->AddInstance(instance));
			}
			const auto values = [&](RHISceneVersionPtr version)
			{
				TVector<Instance> result;
				for (const auto handle : handles)
				{
					const RHISceneInstanceRecord* source = nullptr;
					if (!version->Resolve(handle, source)) return TVector<Instance>{};
					Instance instance{};
					instance.model = source->m_worldMatrix;
					instance.sphereBounds = glm::vec4(0, 0, 0, 1);
					result.Add(instance);
				}
				return result;
			};
			TPackedDrawPagedArenaCache<Instance> arena;
			const auto payload = [&](RHISceneVersionPtr version) -> PayloadPtr
			{
				if (!paged) return {};
				const auto data = values(version);
				arena.BeginUpdate(1u, version->m_stationaryRevision, version->m_sceneRevision);
				if (!arena.TryReuseRange(10u, 1u))
				{
					TVector<Instance> firstPage;
					TVector<uint64_t> keys;
					for (uint32_t i = 0u; i + 1u < StationaryCount; ++i)
					{
						firstPage.Add(data[i]);
						keys.Add(i);
					}
					if (!arena.ReplaceRange(10u, 1u, firstPage, keys)) return {};
				}
				if (!arena.ReplaceRange(20u, version->m_stationaryRevision,
					{ data[StationaryCount - 1u] }, { StationaryCount - 1u })) return {};
				return arena.EndUpdate();
			};
			const auto checkData = [](RHIBufferPtr readback, const TVector<Instance>& expected) -> std::string
			{
				const auto* actual = static_cast<const Instance*>(readback->GetPointer());
				for (size_t i = 0u; i < expected.Num(); ++i)
					if (!(actual[i] == expected[i])) return std::format("flight upload changed instance {}", i);
				return {};
			};
			TVector<RHICommandListPtr> deferred;
			RHIBufferPtr deferredReadback;
			const auto run = [&](uint32_t slot, RHISceneVersionPtr version, PayloadPtr shared,
				uint32_t uploadedInstances, uint32_t uploadRanges, bool defer = false) -> std::string
			{
				auto flight = scene->PrepareFlight(slot, version);
				if (!flight || flight->m_appliedVersion != version) return "flight did not retain its target version";
				const auto expected = values(flight->m_appliedVersion);
				if (expected.Num() != handles.Num() || (paged && !shared)) return "flight payload preparation failed";
				auto& draw = draws[slot];
				auto& packet = draw.m_resources->m_packet;
				packet.Reset();
				RHIBatch batch(material, mesh);
				batch.m_textureBindings = textures;
				if (paged) packet.UseSharedArenaPayload(EMobilityType::Stationary, shared);
				for (uint32_t i = 0u; i < StationaryCount; ++i)
				{
					if (paged)
					{
						if (!packet.AddArenaView(batch, mesh, i + 1u < StationaryCount ? 10u : 20u,
							i, EMobilityType::Stationary)) return "flight arena view did not resolve";
					}
					else packet.Add(batch, mesh, expected[i], i, EMobilityType::Stationary);
				}
				packet.Add(batch, mesh, expected[StationaryCount], StationaryCount, EMobilityType::Dynamic);
				packet.Finalize();
				auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
				commands->BeginCommandList(upload, true);
				commands->BeginCommandList(graphics, true);
				recordDraw(draw, upload, graphics);
				if (packet.m_metrics.m_instanceUploadBytes != sizeof(Instance) * uploadedInstances ||
					packet.m_metrics.m_dirtyInstanceRanges != uploadRanges ||
					draw.m_stats.m_numInstances != handles.Num()) return "unexpected flight upload bytes, ranges or draw count";
				const auto binding = draw.m_resources->m_perInstanceData->GetOrAddShaderBinding("data"_h);
				auto readback = Tests::RecordBufferReadback(graphics, binding->m_vulkan.m_valueBinding,
					sizeof(Instance) * expected.Num());
				if (defer)
				{
					deferred = { upload, graphics };
					deferredReadback = readback;
					return {};
				}
				if (auto error = Submit(upload, graphics); !error.empty()) return error;
				return checkData(readback, expected);
			};
			for (uint32_t slot = 0u; slot < numFlights; ++slot) draws[slot].m_resources->m_packet.InvalidateUploadedState();
			auto first = scene->PublishVersion();
			auto firstPayload = payload(first);
			for (uint32_t slot = 0u; slot < numFlights; ++slot)
				if (auto error = run(slot, first, firstPayload, StationaryCount + 1u, paged ? 3u : 2u); !error.empty()) return error;
			if (auto error = run(numFlights - 1u, first, firstPayload, 1u, 1u, true); !error.empty()) return error;
			for (const uint32_t i : { StationaryCount - 1u, StationaryCount })
			{
				RHISceneInstanceRecord updated;
				if (!scene->ResolveCurrent(handles[i], updated)) return "flight source disappeared";
				updated.m_worldMatrix[3].x += 1000.0f;
				scene->UpdateInstance(handles[i], updated, ToMask(ESceneChangeBit::Transform));
			}
			auto second = scene->PublishVersion();
			auto secondPayload = payload(second);
			if (paged && (firstPayload->m_arenaPages[0] != secondPayload->m_arenaPages[0] ||
				firstPayload->m_arenaPages[1] == secondPayload->m_arenaPages[1])) return "arena did not preserve the unchanged page";
			if (auto error = run(0u, second, secondPayload, 2u, 2u); !error.empty()) return error;
			if (auto error = Submit(deferred[0], deferred[1]); !error.empty()) return error;
			if (auto error = checkData(deferredReadback, values(first)); !error.empty()) return "retained flight: " + error;
			if (auto error = run(0u, second, secondPayload, 1u, 1u); !error.empty()) return error;
			for (uint32_t slot = 1u; slot < numFlights; ++slot)
				if (auto error = run(slot, second, secondPayload, 2u, 2u); !error.empty()) return error;
		}
		return {};
	}

	std::string ValidateScene(const std::array<ShaderSetPtr, 3>& shaders)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto vertices = RHIVertexDescriptionPtr::Make();
		vertices->SetVertexStride(sizeof(glm::vec2));
		vertices->AddAttribute(0u, 0u, EFormat::R32G32_SFLOAT, 0u);
		const RenderState state(false, false, 0.0f, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0u, false);
		auto material = driver->CreateMaterial(vertices, EPrimitiveTopology::TriangleList, state, shaders[0]);
		auto sibling = driver->CreateMaterial(vertices, EPrimitiveTopology::TriangleList, state, shaders[0]);
		auto emptyMaterial = driver->CreateMaterial(vertices, EPrimitiveTopology::TriangleList, state, shaders[1]);
		if (!material || !sibling || !emptyMaterial)
		{
			return "RenderScene materials could not be created";
		}
		const auto layout = material->m_vulkan.m_pipelines[0]->m_layout;
		if (layout != sibling->m_vulkan.m_pipelines[0]->m_layout || material->GetBindings() == sibling->GetBindings())
		{
			return "materials using the same shaders must share their pipeline layout, not their parameter bindings";
		}
		if (layout->m_descriptionSetLayouts.Num() != 5u ||
			!emptyMaterial->m_vulkan.m_pipelines[0]->m_layout->m_descriptionSetLayouts.IsEmpty())
			return "compiled shader interfaces must have five and zero descriptor sets";
		auto textureA = driver->CreateTexture(&ColorA, sizeof(ColorA), glm::ivec3(1), 1u,
			ETextureType::Texture2D, EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		auto textureC = driver->CreateTexture(&ColorC, sizeof(ColorC), glm::ivec3(1), 1u,
			ETextureType::Texture2D, EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		auto textureB = PrivateTextureView(textureC);
		auto bindingsA = SamplerSet(textureA, false);
		auto bindingsB = SamplerSet(textureB, true);
		if (!bindingsA || !bindingsB || VulkanApi::IsCompatible(layout, bindingsB->m_vulkan.m_descriptorSet, 4u))
			return "fresh B must be compiled but require compatible-set projection";
		const auto nativeA = bindingsA->m_vulkan.m_descriptorSet;
		const auto nativeB = bindingsB->m_vulkan.m_descriptorSet;
		const auto revisionB = bindingsB->GetDescriptorRevision();
		const auto hashB = bindingsB->GetCompatibilityHashCode();
		RestoreView restore(textureB);

		auto setup = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(setup, true);
		auto color = driver->CreateRenderTarget(setup, glm::ivec2(Side), 1u, EFormat::R32G32B32A32_SFLOAT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp,
			ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit);
		auto depth = driver->CreateRenderTarget(setup, glm::ivec2(Side), 1u, EFormat::D32_SFLOAT_S8_UINT,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::DepthStencilAttachment_Bit);
		auto setupUpload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(setupUpload, true);
		if (auto error = Submit(setupUpload, setup); !error.empty()) return error;
		auto surface = RHISurfacePtr::Make(color, color, false);
		if (surface->NeedsResolve() || static_cast<uint32_t>(color->GetMsaaSamples()) != 1u)
			return "fixture target must be a real single-sample no-resolve surface";
		auto graph = RHIFrameGraphPtr::Make();
		graph->SetRenderTarget("DepthBuffer"_h, depth);
		auto lights = driver->CreateShaderBindings();
		const auto lightsRevision = lights->GetDescriptorRevision();
		std::array<SceneDraw, 5> draws;
		for (uint32_t i = 0u; i < draws.size(); ++i)
		{
			auto& draw = draws[i];
			draw.m_node = TRefPtr<RenderSceneProbe>::Make();
			draw.m_node->SetString("Tag"_h, "GraphicsBindingResult");
			draw.m_node->SetString("GPUCulling"_h, "false");
			draw.m_node->SetRHIResource("color"_h, surface);
			draw.m_scene.m_submissionContext = RHIRenderSubmissionContextPtr::Make();
			draw.m_scene.m_submissionContext->BeginSubmission(800u + i, 0u);
			draw.m_scene.m_frameBindings = driver->CreateShaderBindings();
			draw.m_scene.m_rhiLightsData = lights;
			draw.m_resources = draw.m_node->GetResources(draw.m_scene);
			draw.m_readback = driver->CreateBuffer(sizeof(Pixels), EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		}
		auto quad = CreateQuad(vertices, -1.0f, 1.0f);
		for (uint32_t i = 0u; i < 3u; ++i)
		{
			AddInstances(draws[i], material, quad, i == 0u ? bindingsA : bindingsB, 1u);
			draws[i].m_resources->m_packet.Finalize();
		}
		const auto record = [&](SceneDraw& draw, RHICommandListPtr upload, RHICommandListPtr graphics)
		{
			upload->m_vulkan.m_commandBuffer->AddDependency(draw.m_scene.m_submissionContext);
			graphics->m_vulkan.m_commandBuffer->AddDependency(draw.m_scene.m_submissionContext);
			commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
				static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
			commands->ImageMemoryBarrier(graphics, color, EImageLayout::TransferDstOptimal);
			commands->ClearImage(graphics, color, Sentinel);
			draw.m_node->Process(graph, upload, graphics, draw.m_scene);
			draw.m_stats = draw.m_node->GetDrawCallStats();
			commands->ImageMemoryBarrier(graphics, color, EImageLayout::TransferSrcOptimal);
			commands->CopyImageToBuffer(graphics, color, draw.m_readback);
		};
		auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(upload, true);
		commands->BeginCommandList(graphics, true);
		record(draws[0], upload, graphics);
		restore.Invalidate();
		record(draws[1], upload, graphics);
		restore.Restore();
		record(draws[2], upload, graphics);
		if (auto error = Submit(upload, graphics); !error.empty()) return error;
		if (draws[0].m_stats.m_numBatches != 1u || draws[0].m_stats.m_numInstances != 1u)
			return "warm A did not record exactly one real draw";
		if (auto error = CheckPixels(draws[0].m_readback, Solid(ColorA), "A"); !error.empty()) return error;
		const auto rejectedPixels = CheckPixels(draws[1].m_readback, Solid(Sentinel), "B");
		if (draws[1].m_stats.m_numBatches != 0u || draws[1].m_stats.m_numInstances != 0u || !rejectedPixels.empty())
			return std::format("safe primary gate: rejected B recorded {} batches/{} candidates; {}",
				draws[1].m_stats.m_numBatches, draws[1].m_stats.m_numInstances,
				rejectedPixels.empty() ? "sentinel pixels intact" : rejectedPixels);
		if (draws[2].m_stats.m_numBatches != 1u || draws[2].m_stats.m_numInstances != 1u)
			return "same-request C did not record exactly one draw";
		if (auto error = CheckPixels(draws[2].m_readback, Solid(ColorC), "C"); !error.empty()) return error;
		if (bindingsB->m_vulkan.m_descriptorSet != nativeB || bindingsB->GetDescriptorRevision() != revisionB ||
			bindingsB->GetCompatibilityHashCode() != hashB || bindingsA->m_vulkan.m_descriptorSet != nativeA || !nativeA->IsCompiled())
			return "view repair changed request identity/revision or invalidated retained A";
		if (lights->GetDescriptorRevision() != lightsRevision || !lights->GetShaderBindings().IsEmpty())
			return "RenderScene changed the shared lighting bindings";
		TVector<RHIShaderBindingSetPtr> projectedBindings;
		for (uint32_t i = 0u; i < 4u; ++i)
		{
			projectedBindings.Add(driver->CreateShaderBindings());
		}
		projectedBindings.Add(bindingsB);
		auto* nativeDriver = driver.DynamicCast<VulkanGraphicsDriver>();
		const auto firstSets = nativeDriver->GetCompatibleDescriptorSets(layout, projectedBindings);
		const auto siblingSets = nativeDriver->GetCompatibleDescriptorSets(sibling->m_vulkan.m_pipelines[0]->m_layout, projectedBindings);
		if (firstSets.Num() != 5u || firstSets != siblingSets || firstSets[4] == nativeB)
		{
			return "equivalent materials did not reuse the same projected descriptor sets";
		}

		// A fresh rejected set avoids C's now-successful compatible-set cache entry.
		auto mixedTexture = PrivateTextureView(textureC);
		auto mixedBad = SamplerSet(mixedTexture, true);
		if (!mixedBad || VulkanApi::IsCompatible(layout, mixedBad->m_vulkan.m_descriptorSet, 4u))
			return "mixed rejected run must require a fresh projection";
		RestoreView mixedRestore(mixedTexture);
		const std::array<RHIShaderBindingSetPtr, 3> orderedBindings{ bindingsA, mixedBad, bindingsB };
		const std::array<RHIMeshPtr, 3> meshes{
			CreateQuad(vertices, -1.0f, -0.25f), CreateQuad(vertices, -0.25f, 0.25f), CreateQuad(vertices, 0.25f, 1.0f) };
		for (uint32_t i = 0u; i < 3u; ++i)
		{
			AddInstances(draws[3], i == 2u ? sibling : material, meshes[i], orderedBindings[i], i + 2u);
		}
		draws[3].m_resources->m_packet.Finalize(true);
		const auto& groups = draws[3].m_resources->m_packet.GetGroups();
		if (groups.Num() != 9u || GetPackedDrawRunEnd(groups, 0u) != 2u ||
			GetPackedDrawRunEnd(groups, 2u) != 5u || GetPackedDrawRunEnd(groups, 5u) != 9u)
			return "mixed packet does not contain the intended 2/3/4 ordered runs";
		for (uint32_t i = 0u; i < groups.Num(); ++i)
			if (groups[i].m_numInstances != 1u || groups[i].m_batch.m_textureBindings != orderedBindings[i < 2u ? 0u : (i < 5u ? 1u : 2u)])
				return "mixed packet ordering changed during finalization";
		AddInstances(draws[4], emptyMaterial, quad, driver->CreateShaderBindings(), 1u);
		draws[4].m_resources->m_packet.Finalize();
		commands->BeginCommandList(upload, true);
		commands->BeginCommandList(graphics, true);
		mixedRestore.Invalidate();
		record(draws[3], upload, graphics);
		mixedRestore.Restore();
		record(draws[4], upload, graphics);
		if (auto error = Submit(upload, graphics); !error.empty()) return error;
		if (draws[3].m_stats.m_numBatches != 2u || draws[3].m_stats.m_numInstances != 6u)
			return std::format("mixed packet recorded {} runs/{} candidates, expected 2/6", draws[3].m_stats.m_numBatches, draws[3].m_stats.m_numInstances);
		Pixels mixedPixels;
		for (uint32_t i = 0u; i < mixedPixels.size(); ++i)
			mixedPixels[i] = i % Side < 3u ? ColorA : (i % Side < 5u ? Sentinel : ColorC);
		if (auto error = CheckPixels(draws[3].m_readback, mixedPixels, "mixed"); !error.empty()) return error;
		if (draws[4].m_stats.m_numBatches != 1u || draws[4].m_stats.m_numInstances != 1u)
			return "descriptor-free material did not record a draw";
		if (auto error = CheckPixels(draws[4].m_readback, Solid(EmptyColor), "zero descriptors"); !error.empty()) return error;
		return ValidateFlightUploads(draws, material, quad, bindingsA, record);
	}

	void CountCallback(const ImDrawList*, const ImDrawCmd* command)
	{
		static_cast<std::atomic<uint32_t>*>(command->UserCallbackData)->fetch_add(1u, std::memory_order_relaxed);
	}

	TSharedPtr<ImGuiApi::PreparedFrame> PrepareImGui(std::atomic<uint32_t>& callbacks)
	{
		ImDrawList first(ImGui::GetDrawListSharedData());
		ImDrawList second(ImGui::GetDrawListSharedData());
		const auto add = [](ImDrawList& list, float left, float right, ImTextureID texture, uint32_t vertexOffset, uint32_t indexOffset)
		{
			const uint32_t previousVertices = static_cast<uint32_t>(list.VtxBuffer.Size);
			const uint32_t previousIndices = static_cast<uint32_t>(list.IdxBuffer.Size);
			list.VtxBuffer.resize(static_cast<int>(vertexOffset + 4u));
			list.IdxBuffer.resize(static_cast<int>(indexOffset + 6u));
			for (uint32_t i = previousVertices; i < vertexOffset; ++i) list.VtxBuffer[i] = { {-100, -100}, {0, 0}, IM_COL32_WHITE };
			for (uint32_t i = previousIndices; i < indexOffset; ++i) list.IdxBuffer[i] = 0u;
			const ImVec2 positions[] = { {left, 0}, {right, 0}, {right, Side}, {left, Side} };
			const ImDrawIdx indices[] = { 0, 1, 2, 0, 2, 3 };
			for (uint32_t i = 0u; i < 4u; ++i) list.VtxBuffer[vertexOffset + i] = { positions[i], {0, 0}, IM_COL32_WHITE };
			std::memcpy(list.IdxBuffer.Data + indexOffset, indices, sizeof(indices));
			ImDrawCmd command;
			command.ClipRect = { 0, 0, Side, Side };
			command.TextureId = texture;
			command.ElemCount = 6u;
			command.VtxOffset = vertexOffset;
			command.IdxOffset = indexOffset;
			list.CmdBuffer.push_back(command);
		};
		add(first, 0, 3, 1u, 0u, 0u);
		add(first, 3, 5, 2u, 4u, 6u);
		ImDrawCmd callback;
		callback.UserCallback = CountCallback;
		callback.UserCallbackData = &callbacks;
		first.CmdBuffer.push_back(callback);
		callback.UserCallback = ImDrawCallback_ResetRenderState;
		callback.UserCallbackData = nullptr;
		first.CmdBuffer.push_back(callback);
		add(second, 5, 8, 3u, 2u, 3u);
		ImDrawData data;
		data.Valid = true;
		data.DisplaySize = { Side, Side };
		data.FramebufferScale = { 1, 1 };
		data.CmdLists.push_back(&first);
		data.CmdLists.push_back(&second);
		data.CmdListsCount = 2;
		return TSharedPtr<ImGuiApi::PreparedFrame>::Make(&data);
	}

	std::string ValidateImGui(ShaderSetPtr shader, ImGuiApi::PreparedFrame& frame, std::atomic<uint32_t>& callbacks)
	{
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		auto description = RHIVertexDescriptionPtr::Make();
		description->SetVertexStride(sizeof(ImDrawVert));
		description->AddAttribute(0u, 0u, EFormat::R32G32_SFLOAT, offsetof(ImDrawVert, pos));
		description->AddAttribute(2u, 0u, EFormat::R32G32_SFLOAT, offsetof(ImDrawVert, uv));
		description->AddAttribute(3u, 0u, EFormat::R8G8B8A8_UNORM, offsetof(ImDrawVert, col));
		const RenderState state(false, false, 0.0f, false, ECullMode::None, EBlendMode::None, EFillMode::Fill, 0u, false);
		frame.Material = driver->CreateMaterial(description, EPrimitiveTopology::TriangleList, state, shader);
		if (!frame.Material) return "ImGui material could not be created";
		auto textureA = driver->CreateTexture(&ColorA, sizeof(ColorA), glm::ivec3(1), 1u,
			ETextureType::Texture2D, EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		auto textureC = driver->CreateTexture(&ColorC, sizeof(ColorC), glm::ivec3(1), 1u,
			ETextureType::Texture2D, EFormat::R32G32B32A32_SFLOAT, ETextureFiltration::Nearest, ETextureClamping::Clamp);
		auto textureB = PrivateTextureView(textureC);
		frame.ShaderBindings = SamplerSet(textureA, false);
		frame.TextureBindings[1u] = frame.ShaderBindings;
		frame.TextureBindings[2u] = SamplerSet(textureB, true);
		frame.TextureBindings[3u] = SamplerSet(textureC, false);
		for (const auto& binding : frame.TextureBindings)
			if (!binding.second) return "ImGui native input could not be created";
		if (VulkanApi::IsCompatible(frame.Material->m_vulkan.m_pipelines[0]->m_layout, frame.TextureBindings[2u]->m_vulkan.m_descriptorSet, 0u))
			return "ImGui B must require fresh projection";
		const auto& data = frame.DrawData.GetDrawData();
		if (data.CmdListsCount != 2 || data.CmdLists[1]->CmdBuffer[0].VtxOffset != 2u || data.CmdLists[1]->CmdBuffer[0].IdxOffset != 3u)
			return "ImGui snapshot lost the second-list offset case";
		frame.VertexBuffer = driver->CreateBuffer(data.TotalVtxCount * sizeof(ImDrawVert), EBufferUsageBit::VertexBuffer_Bit, HostMemory);
		frame.IndexBuffer = driver->CreateBuffer(data.TotalIdxCount * sizeof(ImDrawIdx), EBufferUsageBit::IndexBuffer_Bit, HostMemory);
		if (frame.VertexBuffer->GetOffset() != 0u || frame.IndexBuffer->GetOffset() != 0u)
			return "ImGui ordinary buffers must start at logical buffer offset zero";
		size_t vertexOffset = 0u, indexOffset = 0u;
		for (const ImDrawList* list : data.CmdLists)
		{
			std::memcpy(static_cast<char*>(frame.VertexBuffer->GetPointer()) + vertexOffset, list->VtxBuffer.Data, list->VtxBuffer.Size * sizeof(ImDrawVert));
			std::memcpy(static_cast<char*>(frame.IndexBuffer->GetPointer()) + indexOffset, list->IdxBuffer.Data, list->IdxBuffer.Size * sizeof(ImDrawIdx));
			vertexOffset += list->VtxBuffer.Size * sizeof(ImDrawVert);
			indexOffset += list->IdxBuffer.Size * sizeof(ImDrawIdx);
		}
		auto upload = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		auto graphics = driver->CreateCommandList(false, ECommandListQueue::Graphics);
		commands->BeginCommandList(upload, true);
		commands->BeginCommandList(graphics, true);
		auto color = driver->CreateRenderTarget(graphics, glm::ivec2(Side), 1u, EFormat::B8G8R8A8_UNORM,
			ETextureFiltration::Nearest, ETextureClamping::Clamp, ETextureUsageBit::ColorAttachment_Bit | ETextureUsageBit::TextureTransferSrc_Bit);
		commands->MemoryBarrier(graphics, static_cast<EAccessFlags>(EAccessBit::HostWrite_Bit),
			static_cast<EAccessFlags>(EAccessBit::VertexAttributeRead_Bit) | static_cast<EAccessFlags>(EAccessBit::IndexRead_Bit));
		commands->ImageMemoryBarrier(graphics, color, EImageLayout::ColorAttachmentOptimal);
		const glm::vec4 sentinel(0, 0, 1, 1);
		commands->BeginRenderPass(graphics, TVector<RHITexturePtr>{ color }, {}, glm::ivec4(0, 0, Side, Side),
			glm::ivec2(0), true, sentinel, 0.0f, false, false);
		RestoreView restore(textureB);
		restore.Invalidate();
		ImGuiProbe::ImGui_RenderDrawData(frame, graphics);
		restore.Restore();
		commands->EndRenderPass(graphics);
		auto readback = driver->CreateBuffer(Side * Side * 4u, EBufferUsageBit::BufferTransferDst_Bit, HostMemory);
		commands->ImageMemoryBarrier(graphics, color, EImageLayout::TransferSrcOptimal);
		commands->CopyImageToBuffer(graphics, color, readback);
		if (auto error = Submit(upload, graphics); !error.empty()) return error;
		if (callbacks.load(std::memory_order_relaxed) != 1u) return "ImGui did not execute the callback following rejected B exactly once";
		const auto* bytes = static_cast<const uint8_t*>(readback->GetPointer());
		constexpr uint32_t BgraChannels[] = { 2u, 1u, 0u, 3u };
		for (uint32_t pixel = 0u; pixel < Side * Side; ++pixel)
		{
			const glm::vec4 expectedColor = pixel % Side < 3u ? ColorA : (pixel % Side < 5u ? sentinel : ColorC);
			for (uint32_t channel = 0u; channel < 4u; ++channel)
			{
				const auto expected = static_cast<uint8_t>(std::lround(expectedColor[BgraChannels[channel]] * 255.0f));
				const uint8_t actual = bytes[pixel * 4u + channel];
				if (actual != expected)
					return std::format("ImGui pixel {} BGRA channel {}: expected byte {}, got {}", pixel, channel, expected, actual);
			}
		}
		return {};
	}
}

GraphicsBindingResultTestComponent::~GraphicsBindingResultTestComponent()
{
	// The worker borrows the snapshot; ImGui allocations always die on Main.
	if (m_validation) m_validation->Wait();
}

void GraphicsBindingResultTestComponent::Tick(float)
{
	if (IsFinished()) return;
	if (m_validation)
	{
		if (!m_validation->IsFinished()) return;
		const auto& error = m_validation->GetResult();
		m_imguiFrame.Clear();
		if (!error.empty()) { MarkFailed(error); return; }
		AddJournalEvent("AttachmentLifetime",
			"Native smart-pointer counts follow two-command recording/reset; external views released before GPU submission; single-sample, MSAA MRT/depth resolves, optional missing resolve and legacy framebuffer pass full 8x8 readbacks");
		AddJournalEvent("GraphicsBindingResultEvidence",
			"Real single-sample RenderScene Process: same-command-buffer A/rejected B/same-request C, all 8x8 pixels and exact recorded counts; ordered 2/3/4 mixed runs record 2 runs/6 candidates; zero-descriptor draw passes; actual ImGui owner preserves callback and later list/index/vertex offsets");
		AddJournalEvent("SceneFlightUploads",
			"Two/three flights, contiguous/paged stationary payloads and dynamic rewrites: exact recorded upload bytes/ranges, all 66 GPU records, unchanged-page identity, and an older recorded flight submitted after its successor");
		AddJournalEvent("GraphicsPipelineLayoutReuse",
			"Independent materials share their compiled layout and projected descriptor sets; mixed-material draws pass all 8x8 pixel readbacks");
		MarkPassed();
		return;
	}
	bool ready = true;
	for (uint32_t i = 0u; i < m_shaders.size(); ++i)
	{
		if (!m_shaders[i])
		{
			const char* filename = i == 2u ? "Shaders/ImGuiUI.shader" : "Tests/Shaders/GraphicsBindingResult.shader";
			if (auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(filename))
				App::GetSubmodule<ShaderCompiler>()->LoadShader(info->GetFileId(), m_shaders[i],
					i == 1u ? TVector<std::string>{ "NO_DESCRIPTORS" } : TVector<std::string>{});
		}
		ready &= m_shaders[i] && m_shaders[i]->IsReady();
	}
	if (ready)
	{
		if (!ImGui::GetCurrentContext() || ImGui::GetCurrentContext() != ImGuiApi::GetCurrentContext())
		{ MarkFailed("fixture requires the actual engine ImGui context on Main"); return; }
		m_imguiFrame = PrepareImGui(m_callbacks);
		m_validation = Tasks::CreateTaskWithResult<std::string>("Graphics binding result validation"_h,
			[shaders = m_shaders, frame = m_imguiFrame.GetRawPtr(), callbacks = &m_callbacks]()
			{
				auto error = ValidateAttachmentLifetime(AttachmentCase::SingleSample, false);
				if (!error.empty()) return error;
				for (const auto testCase : { AttachmentCase::SingleSample, AttachmentCase::Multisample,
					AttachmentCase::PartialResolve, AttachmentCase::Legacy })
				{
					error = ValidateAttachmentLifetime(testCase, true);
					if (!error.empty()) return error;
				}
				error = ValidateScene(shaders);
				if (error.empty()) error = ValidateImGui(shaders[2], *frame, *callbacks);
				return error;
			}, EThreadType::RHI);
		m_validation->Run();
	}
	else if (Utils::GetCurrentTimeMs() - GetStartTimeMs() > 30000)
	{
		MarkFailed("graphics binding shaders did not become ready within 30 seconds");
	}
}
