struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <chrono>
#include <set>
#include <assert.h>
#include <map>
#include <vector>
#include <optional>
#include <wtypes.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>

#include "glm/glm/glm.hpp"
#include "glm/glm/gtc/matrix_transform.hpp"

#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "AssetRegistry/ShaderAssetInfo.h"
#include "AssetRegistry/TextureImporter.h"
#include "AssetRegistry/TextureAssetInfo.h"

#include "VulkanDevice.h"
#include "VulkanApi.h"
#include "Platform/Win/Window.h"
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
#include "RHI/RHIResource.h"
#include "VulkanDeviceMemory.h"
#include "VulkanBuffer.h"
#include "VulkanShaderModule.h"
#include "VulkanPipeline.h"
#include "VulkanDescriptors.h"
#include "VulkanPipileneStates.h"

using namespace Sailor;
using namespace Sailor::Win32;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

TRefPtr<VulkanShaderStage> g_testFragShader;
TRefPtr<VulkanShaderStage> g_testVertShader;

const std::vector<RHIVertex> g_testVertices =
{
	{{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
	{{0.5f, -0.5f, 0.0f}, {0.0f, 1.0f}, {0.3f, 1.0f, 0.0f, 1.0f}},
	{{0.5f, 0.5f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 0.5f, 1.0f}},
	{{-0.5f, 0.5f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}},

	{{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f, 1.0f}},
	{{0.5f, -0.5f, -0.5f}, {0.0f, 1.0f}, {0.3f, 1.0f, 0.0f, 1.0f}},
	{{0.5f, 0.5f, -0.5f}, {1.0f, 1.0f}, {0.0f, 0.0f, 0.5f, 1.0f}},
	{{-0.5f, 0.5f, -0.5f}, {1.0f, 0.0f}, {0.0f, 1.0f, 1.0f, 1.0f}}
};

const std::vector<uint32_t> g_testIndices =
{
	0, 1, 2, 2, 3, 0,
	4, 5, 6, 6, 7, 4
};

VulkanDevice::VulkanDevice(const Window* pViewport)
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

	// Cache samplers
	m_samplers.Initialize(TRefPtr<VulkanDevice>(this));

	CreateCommandPool();

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

	m_descriptorPool.Clear();
	m_descriptorSet.Clear();
	m_descriptorSetLayout.Clear();
	m_image.Clear();
	m_imageView.Clear();
	m_graphicsPipeline.Clear();
	m_pipelineLayout.Clear();

	m_commandBuffers.clear();
	m_commandPool.Clear();
	m_transferCommandPool.Clear();
	m_renderFinishedSemaphores.clear();
	m_imageAvailableSemaphores.clear();
	m_syncImages.clear();
	m_syncFences.clear();

	m_graphicsQueue.Clear();
	m_presentQueue.Clear();
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

TRefPtr<VulkanCommandBuffer> VulkanDevice::CreateCommandBuffer(bool bOnlyTransferQueue)
{
	return TRefPtr<VulkanCommandBuffer>::Make(TRefPtr<VulkanDevice>(this), bOnlyTransferQueue ? m_transferCommandPool : m_commandPool);
}

void VulkanDevice::SubmitCommandBuffer(TRefPtr<VulkanCommandBuffer> commandBuffer, TRefPtr<VulkanFence> fence) const
{
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = { commandBuffer->GetHandle() };

	if (*commandBuffer->GetCommandPool() == *m_transferCommandPool)
	{
		VK_CHECK(m_transferQueue->Submit(submitInfo, fence));
	}
	else
	{
		VK_CHECK(m_graphicsQueue->Submit(submitInfo, fence));
	}
}

void VulkanDevice::CreateRenderPass()
{
	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT; // VK_FORMAT_D24_UNORM_S8_UINT or VK_FORMAT_D32_SFLOAT_S8_UINT or VK_FORMAT_D24_SFLOAT_S8_UINT
	m_renderPass = VulkanApi::CreateRenderPass(TRefPtr<VulkanDevice>(this), m_swapchain->GetImageFormat(), depthFormat);
}

void VulkanDevice::CreateCommandPool()
{
	VulkanQueueFamilyIndices queueFamilyIndices = VulkanApi::FindQueueFamilies(m_physicalDevice, m_surface);
	m_commandPool = TRefPtr<VulkanCommandPool>::Make(TRefPtr<VulkanDevice>(this), queueFamilyIndices.m_graphicsFamily.value(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	m_transferCommandPool = TRefPtr<VulkanCommandPool>::Make(TRefPtr<VulkanDevice>(this), queueFamilyIndices.m_transferFamily.value(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
}

void VulkanDevice::CreateFrameSyncSemaphores()
{
	m_syncImages.resize(m_swapchain->GetImageViews().size());

	for (size_t i = 0; i < VulkanApi::MaxFramesInFlight; i++)
	{
		m_imageAvailableSemaphores.push_back(TRefPtr<VulkanSemaphore>::Make(m_device));
		m_renderFinishedSemaphores.push_back(TRefPtr<VulkanSemaphore>::Make(m_device));
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
		const VkDeviceSize uniformBufferSize = sizeof(RHI::UBOTransform);

		m_uniformBuffer = VulkanApi::CreateBuffer(TRefPtr<VulkanDevice>(this),
			uniformBufferSize,
			VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);

		std::vector<uint32_t> vertCode;
		std::vector<uint32_t> fragCode;

		ShaderCompiler::GetInstance()->GetSpirvCode(shaderUID->GetUID(), {}, vertCode, fragCode);

		g_testVertShader = TRefPtr<VulkanShaderStage>::Make(VK_SHADER_STAGE_VERTEX_BIT, "main", TRefPtr<VulkanDevice>(this), vertCode);
		g_testFragShader = TRefPtr<VulkanShaderStage>::Make(VK_SHADER_STAGE_FRAGMENT_BIT, "main", TRefPtr<VulkanDevice>(this), fragCode);

		const TRefPtr<VulkanStateVertexDescription> pVertexDescription = new VulkanStateVertexDescription(
			RHIVertexFactory<RHI::RHIVertex>::GetBindingDescription(),
			RHIVertexFactory<RHI::RHIVertex>::GetAttributeDescriptions());

		const TRefPtr<VulkanStateInputAssembly> pInputAssembly = new VulkanStateInputAssembly();
		const TRefPtr<VulkanStateDynamicViewport> pStateViewport = new VulkanStateDynamicViewport();
		const TRefPtr<VulkanStateRasterization> pStateRasterizer = new VulkanStateRasterization();

		const TRefPtr<VulkanStateDynamic> pDynamicState = new VulkanStateDynamic();
		const TRefPtr<VulkanStateDepthStencil> pDepthStencil = new VulkanStateDepthStencil();
		const TRefPtr<VulkanStateColorBlending> pColorBlending = new VulkanStateColorBlending(false,
			VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO,
			VK_BLEND_OP_ADD,
			VK_BLEND_FACTOR_ONE,
			VK_BLEND_FACTOR_ZERO,
			VK_BLEND_OP_ADD,
			VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);

		const TRefPtr<VulkanStateMultisample> pMultisample = new VulkanStateMultisample();

		m_descriptorSetLayout = TRefPtr<VulkanDescriptorSetLayout>::Make(
			TRefPtr<VulkanDevice>(this),
			std::vector
			{
				VulkanApi::CreateDescriptorSetLayoutBinding(0, VkDescriptorType::VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL),
				VulkanApi::CreateDescriptorSetLayoutBinding(1, VkDescriptorType::VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_ALL)
			});

		m_pipelineLayout = TRefPtr<VulkanPipelineLayout>::Make(TRefPtr<VulkanDevice>(this),
			std::vector{ m_descriptorSetLayout },
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

		TextureImporter::GetInstance()->LoadTexture(textureUID->GetUID(), data, width, height);
		m_image = VulkanApi::CreateImage_Immediate(
			TRefPtr<VulkanDevice>(this),
			data.data(),
			data.size() * sizeof(uint8_t),
			VkExtent3D{ (uint32_t)width, (uint32_t)height, 1 });

		data.clear();

		m_imageView = new VulkanImageView(TRefPtr<VulkanDevice>(this), m_image);
		m_imageView->Compile();

		auto descriptorSizes = vector
		{
			VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1),
			VulkanApi::CreateDescriptorPoolSize(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1)
		};

		auto descriptors = std::vector<TRefPtr<VulkanDescriptor>>
		{
			TRefPtr<VulkanDescriptorBuffer>::Make(0, 0, m_uniformBuffer, 0, sizeof(UBOTransform)),
			TRefPtr<VulkanDescriptorImage>::Make(1, 0, m_samplers.GetSampler(textureUID->GetFiltration(), textureUID->GetClamping()), m_imageView)
		};

		m_descriptorPool = TRefPtr<VulkanDescriptorPool>::Make(TRefPtr<VulkanDevice>(this), 1, descriptorSizes);
		m_descriptorSet = TRefPtr<VulkanDescriptorSet>::Make(TRefPtr<VulkanDevice>(this), m_descriptorPool, m_descriptorSetLayout, descriptors);
		m_descriptorSet->Compile();
	}

}

void VulkanDevice::CreateFramebuffers()
{
	std::vector<TRefPtr<VulkanImageView>>& swapChainImageViews = m_swapchain->GetImageViews();

	m_swapChainFramebuffers.resize(swapChainImageViews.size());

	for (size_t i = 0; i < swapChainImageViews.size(); i++)
	{
		m_swapChainFramebuffers[i] = TRefPtr<VulkanFramebuffer>::Make(
			m_renderPass,
			std::vector<TRefPtr<VulkanImageView>>{ swapChainImageViews[i], m_swapchain->GetDepthBufferView() },
			m_swapchain->GetExtent().width,
			m_swapchain->GetExtent().height,
			1);
	}
}

void VulkanDevice::CreateCommandBuffers()
{
	for (int i = 0; i < m_swapChainFramebuffers.size(); i++)
	{
		m_commandBuffers.push_back(TRefPtr<VulkanCommandBuffer>::Make(TRefPtr<VulkanDevice>(this), m_commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY));
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

bool VulkanDevice::PresentFrame(const std::vector<TRefPtr<VulkanCommandBuffer>>* primaryCommandBuffers,
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

	static auto startTime = std::chrono::high_resolution_clock::now();
	const auto currentTime = std::chrono::high_resolution_clock::now();
	const float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();

	UBOTransform ubo{};
	ubo.m_model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
	ubo.m_view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));

	float aspect = m_swapchain->GetExtent().width / (float)m_swapchain->GetExtent().height;
	ubo.m_projection = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 10.0f);
	ubo.m_projection[1][1] *= -1;

	m_uniformBuffer->GetMemoryDevice()->Copy(0, sizeof(ubo), &ubo);

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

