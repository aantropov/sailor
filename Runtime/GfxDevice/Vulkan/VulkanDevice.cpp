struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <chrono>
#include <set>
#include <assert.h>
#include <map>
#include <vector>
#include <optional>
#include <unordered_map>
#include <wtypes.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "AssetRegistry/ShaderAssetInfo.h"
#include "AssetRegistry/TextureImporter.h"
#include "AssetRegistry/TextureAssetInfo.h"
#include "AssetRegistry/ModelImporter.h"
#include "AssetRegistry/ModelAssetInfo.h"
#include "Math/Math.h"
#include "RHI/RHIResource.h"
#include "VulkanDevice.h"
#include "VulkanApi.h"
#include "Platform/Win32/Window.h"
#include "VulkanRenderPass.h"
#include "VulkanSwapchain.h"
#include "VulkanCommandBuffer.h"
#include "VulkanCommandPool.h"
#include "VulkanSemaphore.h"
#include "VulkanFence.h"
#include "VulkanQueue.h"
#include "VulkanImage.h"
#include "VulkanImageView.h"
#include "VulkanFrameBuffer.h"
#include "VulkanDeviceMemory.h"
#include "VulkanBuffer.h"
#include "VulkanShaderModule.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptors.h"
#include "VulkanPipileneStates.h"

#include "Platform/Win32/Input.h"
#include "Winuser.h"
#include "Framework/Framework.h"
#include "Memory/MemoryBlockAllocator.hpp"

using namespace glm;
using namespace Sailor;
using namespace Sailor::Win32;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

TRefPtr<VulkanShaderStage> g_testFragShader;
TRefPtr<VulkanShaderStage> g_testVertShader;

std::vector<Vertex> g_testVertices =
{
	{{-0.5f, 0.5f, -0.5f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
	{{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f}, {0.3f, 1.0f, 0.0f, 1.0f}},
	{{-0.5f, -0.5f, -0.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, 0.5f, 1.0f}},
	{{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}},

	{{-0.5f, 0.5f, 0.5f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
	{{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f}, {0.3f, 1.0f, 0.0f, 1.0f}},
	{{-0.5f, -0.5f, 0.5f}, {1.0f, 0.0f}, {0.0f, 0.0f, 0.5f, 1.0f}},
	{{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}}
};

std::vector<uint32_t> g_testIndices =
{
   0, 1, 2,    // side 1
	2, 1, 3,
	4, 0, 6,    // side 2
	6, 0, 2,
	7, 5, 6,    // side 3
	6, 5, 4,
	3, 1, 7,    // side 4
	7, 1, 5,
	4, 5, 0,    // side 5
	0, 5, 1,
	3, 7, 2,    // side 6
	2, 7, 6,
};

VkSampleCountFlagBits CalculateMaxAllowedMSAASamples(VkSampleCountFlags counts)
{
	if (counts & VK_SAMPLE_COUNT_64_BIT) { return VK_SAMPLE_COUNT_64_BIT; }
	if (counts & VK_SAMPLE_COUNT_32_BIT) { return VK_SAMPLE_COUNT_32_BIT; }
	if (counts & VK_SAMPLE_COUNT_16_BIT) { return VK_SAMPLE_COUNT_16_BIT; }
	if (counts & VK_SAMPLE_COUNT_8_BIT) { return VK_SAMPLE_COUNT_8_BIT; }
	if (counts & VK_SAMPLE_COUNT_4_BIT) { return VK_SAMPLE_COUNT_4_BIT; }
	if (counts & VK_SAMPLE_COUNT_2_BIT) { return VK_SAMPLE_COUNT_2_BIT; }

	return VK_SAMPLE_COUNT_1_BIT;
}

VulkanDevice::VulkanDevice(const Window* pViewport, RHI::EMsaaSamples requestMsaa)
{
	// Create Win32 surface
	CreateWin32Surface(pViewport);

	// Pick & Create device
	m_physicalDevice = VulkanApi::PickPhysicalDevice(m_surface);
	CreateLogicalDevice(m_physicalDevice);

	// Calculate max anisotropy
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(m_physicalDevice, &properties);
	m_maxAllowedAnisotropy = properties.limits.maxSamplerAnisotropy;
	m_maxAllowedMsaaSamples = CalculateMaxAllowedMSAASamples(properties.limits.framebufferColorSampleCounts & properties.limits.framebufferDepthSampleCounts);
	m_currentMsaaSamples = (VkSampleCountFlagBits)(std::min((uint8_t)requestMsaa, (uint8_t)m_maxAllowedMsaaSamples));

	SAILOR_LOG("m_maxAllowedAnisotropy = %.2f", m_maxAllowedAnisotropy);
	SAILOR_LOG("m_maxAllowedMSAASamples = %d, requestedMSAASamples = %d", m_maxAllowedMsaaSamples, m_currentMsaaSamples);

	// Cache samplers
	m_samplers.Initialize(TRefPtr<VulkanDevice>(this));

	// Cache memory requirements
	{
		TRefPtr<VulkanBuffer> stagingBuffer = TRefPtr<VulkanBuffer>::Make(TRefPtr<VulkanDevice>(this),
			1024,
			VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_SHARING_MODE_CONCURRENT);

		stagingBuffer->Compile();
		m_memoryRequirements_StagingBuffer = stagingBuffer->GetMemoryRequirements();
		stagingBuffer.Clear();
	}

	// Create swapchain	CreateCommandPool();
	CreateSwapchain(pViewport);

	// Create graphics
	CreateRenderPass();
	CreateFramebuffers();
	CreateGraphicsPipeline();
	CreateVertexBuffer();
	CreateCommandBuffers();
	CreateFrameSyncSemaphores();

	m_bIsSwapChainOutdated = false;
}

VulkanDevice::~VulkanDevice()
{
	vkDestroyDevice(m_device, nullptr);

	m_surface.Clear();
}

void VulkanDevice::Shutdown()
{
	//Clear dependencies
	WaitIdle();

	CleanupSwapChain();

	m_swapchain.Clear();

	m_vertexBuffer.Clear();
	m_indexBuffer.Clear();
	m_uniformBuffer.Clear();

	g_testFragShader.Clear();
	g_testVertShader.Clear();

	m_descriptorSet.Clear();
	m_image.Clear();
	m_imageView.Clear();
	m_graphicsPipeline.Clear();
	m_pipelineLayout.Clear();

	m_commandBuffers.clear();

	for (auto& pair : m_threadContext)
	{
		pair.second.Clear();
	}

	m_renderFinishedSemaphores.clear();
	m_imageAvailableSemaphores.clear();
	m_syncImages.clear();
	m_syncFences.clear();

	m_graphicsQueue.Clear();
	m_presentQueue.Clear();
}

ThreadContext& VulkanDevice::GetThreadContext()
{
	const auto threadId = GetCurrentThreadId();
	auto res = m_threadContext.find(threadId);
	if (res != m_threadContext.end())
	{
		return *(*res).second;
	}

	return *(m_threadContext[threadId] = CreateThreadContext());
}

TRefPtr<VulkanSurface> VulkanDevice::GetSurface() const
{
	return m_surface;
}

VkFormat VulkanDevice::GetDepthFormat() const
{
	return VulkanApi::SelectFormatByFeatures(
		m_physicalDevice,
		{ VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT },
		VK_IMAGE_TILING_OPTIMAL,
		VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
	);
}

bool VulkanDevice::IsMipsSupported(VkFormat format) const
{
	VkFormatProperties formatProperties;
	vkGetPhysicalDeviceFormatProperties(m_physicalDevice, format, &formatProperties);

	if (!(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
	{
		return false;
	}

	return true;
}

TRefPtr<VulkanCommandBuffer> VulkanDevice::CreateCommandBuffer(bool bOnlyTransferQueue)
{
	return TRefPtr<VulkanCommandBuffer>::Make(TRefPtr<VulkanDevice>(this),
		bOnlyTransferQueue ? GetThreadContext().m_transferCommandPool : GetThreadContext().m_commandPool);
}

void VulkanDevice::SubmitCommandBuffer(TRefPtr<VulkanCommandBuffer> commandBuffer,
	TRefPtr<VulkanFence> fence,
	std::vector<TRefPtr<VulkanSemaphore>> signalSemaphores,
	std::vector<TRefPtr<VulkanSemaphore>> waitSemaphores)
{
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = { commandBuffer->GetHandle() };

	VkSemaphore* waits = reinterpret_cast<VkSemaphore*>(_malloca(waitSemaphores.size() * sizeof(VkSemaphore)));
	VkPipelineStageFlags* waitStages = reinterpret_cast<VkPipelineStageFlags*>(_malloca(waitSemaphores.size() * sizeof(VkPipelineStageFlags)));
	VkSemaphore* signals = reinterpret_cast<VkSemaphore*>(_malloca(signalSemaphores.size() * sizeof(VkSemaphore)));

	for (uint32_t i = 0; i < waitSemaphores.size(); i++)
	{
		waits[i] = *waitSemaphores[i];
		waitStages[i] = waitSemaphores[i]->PipelineStageFlags();
	}

	for (uint32_t i = 0; i < signalSemaphores.size(); i++)
	{
		signals[i] = *signalSemaphores[i];
	}

	submitInfo.signalSemaphoreCount = static_cast<uint32_t>(signalSemaphores.size());
	submitInfo.pSignalSemaphores = &signals[0];

	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
	submitInfo.pWaitSemaphores = &waits[0];
	submitInfo.pWaitDstStageMask = &waitStages[0];

	if (*commandBuffer->GetCommandPool() == *GetThreadContext().m_transferCommandPool)
	{
		VK_CHECK(m_transferQueue->Submit(submitInfo, fence));
	}
	else
	{
		VK_CHECK(m_graphicsQueue->Submit(submitInfo, fence));
	}

	_freea(waits);
	_freea(signals);
	_freea(waitStages);
}

void VulkanDevice::CreateRenderPass()
{
	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // VK_FORMAT_D24_UNORM_S8_UINT or VK_FORMAT_D32_SFLOAT_S8_UINT or VK_FORMAT_D24_SFLOAT_S8_UINT
	m_renderPass = VulkanApi::CreateMSSRenderPass(TRefPtr<VulkanDevice>(this), m_swapchain->GetImageFormat(), depthFormat, (VkSampleCountFlagBits)m_currentMsaaSamples);
}

TUniquePtr<ThreadContext> VulkanDevice::CreateThreadContext()
{
	TUniquePtr<ThreadContext> context = TUniquePtr<ThreadContext>::Make();

	VulkanQueueFamilyIndices queueFamilyIndices = VulkanApi::FindQueueFamilies(m_physicalDevice, m_surface);
	context->m_commandPool = TRefPtr<VulkanCommandPool>::Make(TRefPtr<VulkanDevice>(this), queueFamilyIndices.m_graphicsFamily.value(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	context->m_transferCommandPool = TRefPtr<VulkanCommandPool>::Make(TRefPtr<VulkanDevice>(this), queueFamilyIndices.m_transferFamily.value(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);

	auto descriptorSizes = vector
	{
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1),
		VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)
	};

	context->m_descriptorPool = TRefPtr<VulkanDescriptorPool>::Make(TRefPtr<VulkanDevice>(this), 1, descriptorSizes);

	return context;
}

void VulkanDevice::CreateFrameSyncSemaphores()
{
	m_syncImages.resize(m_swapchain->GetImageViews().size());

	for (size_t i = 0; i < VulkanApi::MaxFramesInFlight; i++)
	{
		m_imageAvailableSemaphores.push_back(TRefPtr<VulkanSemaphore>::Make(TRefPtr<VulkanDevice>(this)));
		m_renderFinishedSemaphores.push_back(TRefPtr<VulkanSemaphore>::Make(TRefPtr<VulkanDevice>(this)));
		m_syncFences.push_back(TRefPtr<VulkanFence>::Make(TRefPtr<VulkanDevice>(this), VK_FENCE_CREATE_SIGNALED_BIT));
	}
}

bool VulkanDevice::RecreateSwapchain(const Window* pViewport)
{
	if (pViewport->GetWidth() == 0 || pViewport->GetHeight() == 0)
	{
		return false;
	}

	WaitIdle();

	CleanupSwapChain();

	CreateSwapchain(pViewport);
	CreateRenderPass();
	CreateFramebuffers();

	m_bIsSwapChainOutdated = false;
	return true;
}

void VulkanDevice::CreateVertexBuffer()
{
	if (auto modelUID = AssetRegistry::GetInstance()->GetAssetInfo<ModelAssetInfo>("Models\\Sponza\\sponza.obj"))
	{
		//ModelImporter::GetInstance()->LoadModel(modelUID->GetUID(), g_testVertices, g_testIndices);
	}

	const VkDeviceSize bufferSize = sizeof(g_testVertices[0]) * g_testVertices.size();
	const VkDeviceSize indexBufferSize = sizeof(g_testIndices[0]) * g_testIndices.size();

	m_vertexBuffer = VulkanApi::CreateBuffer_Immediate(TRefPtr<VulkanDevice>(this),
		reinterpret_cast<void const*>(&g_testVertices[0]),
		bufferSize,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

	m_indexBuffer = VulkanApi::CreateBuffer_Immediate(TRefPtr<VulkanDevice>(this),
		reinterpret_cast<void const*>(&g_testIndices[0]),
		indexBufferSize,
		VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void VulkanDevice::CreateGraphicsPipeline()
{
	if (auto shaderUID = AssetRegistry::GetInstance()->GetAssetInfo<ShaderAssetInfo>("Shaders\\Simple.shader"))
	{
		const VkDeviceSize uniformBufferSize = sizeof(RHI::UboTransform);

		m_uniformBuffer = VulkanApi::CreateBuffer(TRefPtr<VulkanDevice>(this),
			uniformBufferSize,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

		std::vector<uint32_t> vertCode;
		std::vector<uint32_t> fragCode;

		ShaderCompiler::GetInstance()->GetSpirvCode(shaderUID->GetUID(), {}, vertCode, fragCode, true);

		g_testVertShader = TRefPtr<VulkanShaderStage>::Make(VK_SHADER_STAGE_VERTEX_BIT, "main", TRefPtr<VulkanDevice>(this), vertCode);
		g_testFragShader = TRefPtr<VulkanShaderStage>::Make(VK_SHADER_STAGE_FRAGMENT_BIT, "main", TRefPtr<VulkanDevice>(this), fragCode);

		g_testVertShader->Compile();
		g_testFragShader->Compile();

		const TRefPtr<VulkanStateVertexDescription> pVertexDescription = TRefPtr<VulkanStateVertexDescription>::Make(
			VertexFactory<RHI::Vertex>::GetBindingDescription(),
			VertexFactory<RHI::Vertex>::GetAttributeDescriptions());

		const TRefPtr<VulkanStateInputAssembly> pInputAssembly = TRefPtr<VulkanStateInputAssembly>::Make();
		const TRefPtr<VulkanStateDynamicViewport> pStateViewport = TRefPtr<VulkanStateDynamicViewport>::Make();
		const TRefPtr<VulkanStateRasterization> pStateRasterizer = TRefPtr<VulkanStateRasterization>::Make();

		const TRefPtr<VulkanStateDynamic> pDynamicState = TRefPtr<VulkanStateDynamic>::Make();
		const TRefPtr<VulkanStateDepthStencil> pDepthStencil = TRefPtr<VulkanStateDepthStencil>::Make();
		const TRefPtr<VulkanStateColorBlending> pColorBlending = TRefPtr<VulkanStateColorBlending>::Make(false,
			VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO,
			VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO,
			VK_BLEND_OP_ADD,
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

		const TRefPtr<VulkanStateMultisample> pMultisample = TRefPtr<VulkanStateMultisample>::Make(GetCurrentMsaaSamples());

		auto descriptorSetLayouts = VulkanApi::CreateDescriptorSetLayouts(TRefPtr<VulkanDevice>(this), { g_testVertShader , g_testFragShader });

		m_pipelineLayout = TRefPtr<VulkanPipelineLayout>::Make(TRefPtr<VulkanDevice>(this),
			descriptorSetLayouts,
			std::vector<VkPushConstantRange>(),
			0);

		std::vector<TRefPtr<VulkanPipelineState>> states = { pStateViewport, pVertexDescription, pInputAssembly, pStateRasterizer, pDynamicState, pDepthStencil, pColorBlending, pMultisample };

		m_graphicsPipeline = TRefPtr<VulkanPipeline>::Make(TRefPtr<VulkanDevice>(this),
			m_pipelineLayout,
			std::vector{ g_testVertShader, g_testFragShader },
			states,
			0);

		m_graphicsPipeline->m_renderPass = m_renderPass;
		m_graphicsPipeline->Compile();
	}

	if (auto textureUID = AssetRegistry::GetInstance()->GetAssetInfo<TextureAssetInfo>("Textures\\VulkanLogo.png"))
	{
		TextureImporter::ByteCode data;
		int32_t width;
		int32_t height;
		uint32_t mipLevels;

		TextureImporter::GetInstance()->LoadTexture(textureUID->GetUID(), data, width, height, mipLevels);
		m_image = VulkanApi::CreateImage_Immediate(
			TRefPtr<VulkanDevice>(this),
			data.data(),
			data.size() * sizeof(uint8_t),
			VkExtent3D{ (uint32_t)width, (uint32_t)height, 1 },
			mipLevels);

		data.clear();

		m_imageView = new VulkanImageView(TRefPtr<VulkanDevice>(this), m_image);
		m_imageView->Compile();

		auto descriptors = std::vector<TRefPtr<VulkanDescriptor>>
		{
			TRefPtr<VulkanDescriptorBuffer>::Make(0, 0, m_uniformBuffer, 0, sizeof(UboTransform)),
			TRefPtr<VulkanDescriptorImage>::Make(1, 0,
				m_samplers.GetSampler(textureUID->GetFiltration(),
				textureUID->GetClamping(),
				m_image->m_mipLevels),
				m_imageView)
		};

		m_descriptorSet = TRefPtr<VulkanDescriptorSet>::Make(TRefPtr<VulkanDevice>(this), 
			GetThreadContext().m_descriptorPool, 
			m_pipelineLayout->m_descriptionSetLayouts[0], 
			descriptors);

		m_descriptorSet->Compile();
	}
}

void VulkanDevice::CreateFramebuffers()
{
	std::vector<TRefPtr<VulkanImageView>>& swapChainImageViews = m_swapchain->GetImageViews();

	m_swapChainFramebuffers.resize(swapChainImageViews.size());

	std::vector<TRefPtr<VulkanImageView>> attachments;

	uint8_t swapchainIndex = 1;
	if ((VkSampleCountFlagBits)m_currentMsaaSamples != VK_SAMPLE_COUNT_1_BIT)
	{
		attachments = { m_swapchain->GetColorBufferView(), nullptr, m_swapchain->GetDepthBufferView() };
	}
	else
	{
		swapchainIndex = 0;
		attachments = { nullptr, m_swapchain->GetDepthBufferView() };
	}

	for (size_t i = 0; i < swapChainImageViews.size(); i++)
	{
		attachments[swapchainIndex] = swapChainImageViews[i];
		m_swapChainFramebuffers[i] = TRefPtr<VulkanFramebuffer>::Make(
			m_renderPass,
			attachments,
			m_swapchain->GetExtent().width,
			m_swapchain->GetExtent().height,
			1);
	}
}

void VulkanDevice::CreateCommandBuffers()
{
	for (int i = 0; i < m_swapChainFramebuffers.size(); i++)
	{
		m_commandBuffers.push_back(TRefPtr<VulkanCommandBuffer>::Make(TRefPtr<VulkanDevice>(this), GetThreadContext().m_commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY));
	}
}

void VulkanDevice::CreateLogicalDevice(VkPhysicalDevice physicalDevice)
{
	m_queueFamilies = VulkanApi::FindQueueFamilies(physicalDevice, m_surface);

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<uint32_t> uniqueQueueFamilies = { m_queueFamilies.m_graphicsFamily.value(),
		m_queueFamilies.m_presentFamily.value(),
		m_queueFamilies.m_transferFamily.value() };

	float queuePriority = 1.0f;

	for (uint32_t queueFamily : uniqueQueueFamilies)
	{
		VkDeviceQueueCreateInfo queueCreateInfo{ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
		queueCreateInfo.queueFamilyIndex = queueFamily;
		queueCreateInfo.queueCount = 1;
		queueCreateInfo.pQueuePriorities = &queuePriority;
		queueCreateInfos.push_back(queueCreateInfo);
	}

	VkPhysicalDeviceFeatures deviceFeatures{};
	deviceFeatures.samplerAnisotropy = VK_TRUE;

#ifdef SAILOR_MSAA_IMPACTS_TEXTURE_SAMPLING
	deviceFeatures.sampleRateShading = VK_TRUE;
#endif

	VkDeviceCreateInfo createInfo{ VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };

	std::vector<const char*> deviceExtensions;
	std::vector<const char*> instanceExtensions;
	VulkanApi::GetRequiredExtensions(deviceExtensions, instanceExtensions);

	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	createInfo.ppEnabledExtensionNames = deviceExtensions.data();

	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.pEnabledFeatures = &deviceFeatures;

	// Compatibility with older Vulkan drivers
	const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation" };
	if (VulkanApi::GetInstance()->IsEnabledValidationLayers())
	{
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();
	}
	else
	{
		createInfo.enabledLayerCount = 0;
	}

	VK_CHECK(vkCreateDevice(physicalDevice, &createInfo, nullptr, &m_device));

	// Create queues
	VkQueue graphicsQueue;
	vkGetDeviceQueue(m_device, m_queueFamilies.m_graphicsFamily.value(), 0, &graphicsQueue);
	m_graphicsQueue = TRefPtr<VulkanQueue>::Make(graphicsQueue, m_queueFamilies.m_graphicsFamily.value(), 0);

	VkQueue presentQueue;
	vkGetDeviceQueue(m_device, m_queueFamilies.m_presentFamily.value(), 0, &presentQueue);
	m_presentQueue = TRefPtr<VulkanQueue>::Make(presentQueue, m_queueFamilies.m_presentFamily.value(), 0);

	VkQueue transferQueue;
	vkGetDeviceQueue(m_device, m_queueFamilies.m_transferFamily.value(), 0, &transferQueue);
	m_transferQueue = TRefPtr<VulkanQueue>::Make(transferQueue, m_queueFamilies.m_transferFamily.value(), 0);
}

void VulkanDevice::CreateWin32Surface(const Window* viewport)
{
	VkWin32SurfaceCreateInfoKHR createInfoWin32{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	createInfoWin32.hwnd = viewport->GetHWND();
	createInfoWin32.hinstance = viewport->GetHINSTANCE();
	VkSurfaceKHR surface;
	VK_CHECK(vkCreateWin32SurfaceKHR(VulkanApi::GetVkInstance(), &createInfoWin32, nullptr, &surface));

	m_surface = TRefPtr<VulkanSurface>::Make(surface, VulkanApi::GetVkInstance());
}

void VulkanDevice::CreateSwapchain(const Window* viewport)
{
	TRefPtr<VulkanSwapchain> oldSwapchain = m_swapchain;
	m_swapchain.Clear();

	m_swapchain = TRefPtr<VulkanSwapchain>::Make(
		TRefPtr<VulkanDevice>(this),
		viewport->GetWidth(),
		viewport->GetHeight(),
		viewport->IsVsyncRequested(),
		oldSwapchain);
}

void VulkanDevice::CleanupSwapChain()
{
	m_swapChainFramebuffers.clear();
	m_renderPass.Clear();
}

void VulkanDevice::WaitIdlePresentQueue()
{
	m_presentQueue->WaitIdle();
}

void VulkanDevice::WaitIdle()
{
	vkDeviceWaitIdle(m_device);
}

bool VulkanDevice::ShouldFixLostDevice(const Win32::Window* pViewport)
{
	return IsSwapChainOutdated() || (m_swapchain && (pViewport->GetWidth() != m_swapchain->GetExtent().width ||
		pViewport->GetHeight() != m_swapchain->GetExtent().height));
}

void VulkanDevice::FixLostDevice(const Win32::Window* pViewport)
{
	RecreateSwapchain(pViewport);
}

bool VulkanDevice::PresentFrame(const FrameState& state, const std::vector<TRefPtr<VulkanCommandBuffer>>* primaryCommandBuffers,
	const std::vector<TRefPtr<VulkanCommandBuffer>>* secondaryCommandBuffers,
	const std::vector<TRefPtr<VulkanSemaphore>>* semaphoresToWait)
{
	// Wait while we recreate swapchain from main thread to sync with Win32Api
	if (m_bIsSwapChainOutdated)
	{
		return false;
	}

	// Wait while GPU is finishing frame
	m_syncFences[m_currentFrame]->Wait();

	///////////////////////////////////
	static glm::vec3 cameraPosition = Math::vec3_Forward * -10.0f;
	static glm::vec3 cameraViewDir = Math::vec3_Forward;

	const float sensitivity = 500;

	glm::vec3 delta = glm::vec3(0.0f, 0.0f, 0.0f);
	if (state.GetInputState().IsKeyDown('A'))
		delta += -cross(cameraViewDir, Math::vec3_Up);

	if (state.GetInputState().IsKeyDown('D'))
		delta += cross(cameraViewDir, Math::vec3_Up);

	if (state.GetInputState().IsKeyDown('W'))
		delta += cameraViewDir;

	if (state.GetInputState().IsKeyDown('S'))
		delta += -cameraViewDir;

	if (glm::length(delta) > 0)
		cameraPosition += glm::normalize(delta) * sensitivity * state.GetDeltaTime();

	const float speed = 50.0f;

	vec2 shift{};
	shift.x += (state.GetMouseDeltaToCenterViewport().x) * state.GetDeltaTime() * speed;
	shift.y += (state.GetMouseDeltaToCenterViewport().y) * state.GetDeltaTime() * speed;

	if (glm::length(shift) > 0.0f && state.GetInputState().IsKeyDown(VK_LBUTTON))
	{
		glm::quat hRotation = angleAxis(-glm::radians(shift.x), Math::vec3_Up);
		glm::quat vRotation = angleAxis(glm::radians(shift.y), cross(Math::vec3_Up, cameraViewDir));

		cameraViewDir = vRotation * cameraViewDir;
		cameraViewDir = hRotation * cameraViewDir;
	}

	UboTransform ubo{};
	ubo.m_model = glm::rotate(glm::mat4(1.0f), state.GetTime() * glm::radians(90.0f), Math::vec3_Up);
	ubo.m_view = glm::lookAt(cameraPosition, cameraPosition + cameraViewDir, Math::vec3_Up);

	float aspect = m_swapchain->GetExtent().width / (float)m_swapchain->GetExtent().height;
	ubo.m_projection = glm::perspective(glm::radians(90.0f), aspect, 0.1f, 10000.0f);
	ubo.m_projection[1][1] *= -1;

	m_uniformBuffer->GetMemoryDevice()->Copy(0, sizeof(ubo), &ubo);

	/////////////////////////////////////////

	uint32_t imageIndex;
	VkResult result = m_swapchain->AcquireNextImage(UINT64_MAX, m_imageAvailableSemaphores[m_currentFrame], TRefPtr<VulkanFence>(), imageIndex);

	if (result == VK_ERROR_OUT_OF_DATE_KHR)
	{
		m_bIsSwapChainOutdated = true;
		return false;
	}
	else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR)
	{
		SAILOR_LOG("Failed to acquire swap chain image!");
		return false;
	}

	// Check if a previous frame is using this image (i.e. there is its fence to wait on)
	if (m_syncImages[imageIndex] && *m_syncImages[imageIndex] != VK_NULL_HANDLE)
	{
		// Wait while previous frame frees the image
		m_syncImages[imageIndex]->Wait();
	}
	// Mark the image as now being in use by this frame
	m_syncImages[imageIndex] = m_syncFences[m_currentFrame];

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

	//////////////////////////////////////////////////
	TRefPtr<VulkanStateViewport> pStateViewport = new VulkanStateViewport((float)m_swapchain->GetExtent().width, (float)m_swapchain->GetExtent().height);

	m_commandBuffers[imageIndex]->BeginCommandList();
	{
		m_commandBuffers[imageIndex]->BeginRenderPass(m_renderPass, m_swapChainFramebuffers[imageIndex], m_swapchain->GetExtent());

		if (secondaryCommandBuffers)
		{
			for (auto cmdBuffer : *secondaryCommandBuffers)
			{
				m_commandBuffers[imageIndex]->Execute(cmdBuffer);
			}
		}

		m_commandBuffers[imageIndex]->BindPipeline(m_graphicsPipeline);
		m_commandBuffers[imageIndex]->SetViewport(pStateViewport);
		m_commandBuffers[imageIndex]->SetScissor(pStateViewport);
		m_commandBuffers[imageIndex]->BindVertexBuffers({ m_vertexBuffer });
		m_commandBuffers[imageIndex]->BindIndexBuffer(m_indexBuffer);
		m_commandBuffers[imageIndex]->BindDescriptorSet(m_pipelineLayout, m_descriptorSet);
		m_commandBuffers[imageIndex]->DrawIndexed(m_indexBuffer);

		m_commandBuffers[imageIndex]->EndRenderPass();
	}
	m_commandBuffers[imageIndex]->EndCommandList();

	std::vector<VkSemaphore> waitSemaphores;
	if (semaphoresToWait)
	{
		waitSemaphores.reserve(semaphoresToWait->size() + 1);
		for (auto semaphore : *semaphoresToWait)
		{
			waitSemaphores.push_back(*semaphore);
		}
	}

	waitSemaphores.push_back(*m_imageAvailableSemaphores[m_currentFrame]);

	std::vector<VkCommandBuffer> commandBuffers;
	if (primaryCommandBuffers)
	{
		commandBuffers.reserve(primaryCommandBuffers->size() + 1);
		for (auto cmdBuffer : *primaryCommandBuffers)
		{
			commandBuffers.push_back(*cmdBuffer);
		}
	}

	commandBuffers.push_back(*m_commandBuffers[imageIndex]->GetHandle());
	///////////////////////////////////////////////////

	VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
	submitInfo.pWaitSemaphores = &waitSemaphores[0];
	submitInfo.pWaitDstStageMask = waitStages;

	submitInfo.commandBufferCount = static_cast<uint32_t>(commandBuffers.size());
	submitInfo.pCommandBuffers = &commandBuffers[0];

	VkSemaphore signalSemaphores[] = { *m_renderFinishedSemaphores[m_currentFrame] };
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = signalSemaphores;

	m_syncFences[m_currentFrame]->Reset();
	VK_CHECK(m_graphicsQueue->Submit(submitInfo, m_syncFences[m_currentFrame]));

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = signalSemaphores;

	VkSwapchainKHR swapChains[] = { *m_swapchain };
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = swapChains;
	presentInfo.pImageIndices = &imageIndex;
	presentInfo.pResults = nullptr; // Optional

	result = m_presentQueue->Present(presentInfo);

	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
	{
		m_bIsSwapChainOutdated = true;
	}
	else if (result != VK_SUCCESS)
	{
		SAILOR_LOG("Failed to present swap chain image!");
		return false;
	}

	m_currentFrame = (m_currentFrame + 1) % VulkanApi::MaxFramesInFlight;

	return true;
}

