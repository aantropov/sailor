#include "AssetRegistry/ShaderAssetInfo.h"
struct IUnknown; // Workaround for "combaseapi.h(229): error C2187: syntax error: 'identifier' was unexpected here" when using /permissive-

#include <set>
#include <assert.h>
#include <map>
#include <vector>
#include <optional>
#include <wtypes.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include "VulkanDevice.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/ShaderCompiler.h"
#include "Platform/Win/Window.h"
#include "VulkanApi.h"
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

using namespace Sailor;
using namespace Sailor::Win32;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

TRefPtr<VulkanShaderStage> g_testFragShader;
TRefPtr<VulkanShaderStage> g_testVertShader;

const std::vector<RHIVertex> g_testVertices =
{
	{{-0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}},
	{{0.5f, -0.5f}, {0.3f, 1.0f, 0.0f}},
	{{0.5f, 0.5f}, {0.0f, 0.0f, 0.5f}},
	{{-0.5f, 0.5f}, {0.0f, 1.0f, 1.0f}}
};

const std::vector<uint32_t> g_testIndices =
{
	0, 1, 2, 2, 3, 0
};

VulkanDevice::VulkanDevice(const Window* pViewport)
{
	// Create Win32 surface
	CreateWin32Surface(pViewport);

	// Pick & Create device
	m_physicalDevice = VulkanApi::PickPhysicalDevice(m_surface);
	CreateLogicalDevice(m_physicalDevice);

	// Create swapchain
	CreateSwapchain(pViewport);

	// Create graphics
	CreateRenderPass();
	CreateGraphicsPipeline();
	CreateFramebuffers();
	CreateCommandPool();
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

	g_testFragShader.Clear();
	g_testVertShader.Clear();

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
	VkFormat depthFormat = VK_FORMAT_D32_SFLOAT; // VK_FORMAT_D24_UNORM_S8_UINT or VK_FORMAT_D32_SFLOAT_S8_UINT or VK_FORMAT_D24_SFLOAT_S8_UINT
	m_renderPass = VulkanApi::CreateRenderPass(TRefPtr<VulkanDevice>(this), m_swapchain->GetImageFormat(), depthFormat);
}

void VulkanDevice::CreateCommandPool()
{
	VulkanQueueFamilyIndices queueFamilyIndices = VulkanApi::FindQueueFamilies(m_physicalDevice, m_surface);
	m_commandPool = TRefPtr<VulkanCommandPool>::Make(m_device, queueFamilyIndices.m_graphicsFamily.value(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
	m_transferCommandPool = TRefPtr<VulkanCommandPool>::Make(m_device, queueFamilyIndices.m_transferFamily.value(), VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);
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
	CreateGraphicsPipeline();
	CreateFramebuffers();
	CreateCommandBuffers();

	m_bIsSwapChainOutdated = false;
	return true;
}

void VulkanDevice::CreateVertexBuffer()
{
	VkDeviceSize bufferSize = sizeof(g_testVertices[0]) * g_testVertices.size();
	VkDeviceSize indexBufferSize = sizeof(g_testIndices[0]) * g_testIndices.size();

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
		std::vector<uint32_t> vertCode;
		std::vector<uint32_t> fragCode;

		ShaderCompiler::GetInstance()->GetSpirvCode(shaderUID->GetUID(), {}, vertCode, fragCode);

		g_testVertShader = TRefPtr<VulkanShaderStage>::Make(VK_SHADER_STAGE_VERTEX_BIT, "main", TRefPtr<VulkanDevice>(this), vertCode);
		g_testVertShader->Compile();
		
		g_testFragShader = TRefPtr<VulkanShaderStage>::Make(VK_SHADER_STAGE_FRAGMENT_BIT, "main", TRefPtr<VulkanDevice>(this), fragCode);
		g_testFragShader->Compile();
		
		VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
		g_testVertShader->Apply(vertShaderStageInfo);
		
		VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
		g_testFragShader->Apply(fragShaderStageInfo);
		
		VkPipelineShaderStageCreateInfo shaderStages[] = { fragShaderStageInfo, vertShaderStageInfo };

		auto bindingDescription = RHIVertexFactoryPositionColor::GetBindingDescription();
		auto attributeDescriptions = RHIVertexFactoryPositionColor::GetAttributeDescriptions();

		VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
		vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

		vertexInputInfo.vertexBindingDescriptionCount = 1;
		vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
		vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
		vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

		VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
		inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
		inputAssembly.primitiveRestartEnable = VK_FALSE;

		VkViewport viewport{};
		viewport.x = 0.0f;
		viewport.y = 0.0f;
		viewport.width = (float)m_swapchain->GetExtent().width;
		viewport.height = (float)m_swapchain->GetExtent().height;
		viewport.minDepth = 0.0f;
		viewport.maxDepth = 1.0f;

		VkRect2D scissor{};
		scissor.offset = { 0, 0 };
		scissor.extent = m_swapchain->GetExtent();

		VkPipelineViewportStateCreateInfo viewportState{};
		viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportState.viewportCount = 1;
		viewportState.pViewports = &viewport;
		viewportState.scissorCount = 1;
		viewportState.pScissors = &scissor;

		VkPipelineRasterizationStateCreateInfo rasterizer{};
		rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizer.depthClampEnable = VK_FALSE;
		rasterizer.rasterizerDiscardEnable = VK_FALSE;
		rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
		rasterizer.lineWidth = 1.0f;
		rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
		rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

		rasterizer.depthBiasEnable = VK_FALSE;
		rasterizer.depthBiasConstantFactor = 0.0f; // Optional
		rasterizer.depthBiasClamp = 0.0f; // Optional
		rasterizer.depthBiasSlopeFactor = 0.0f; // Optional

		VkPipelineMultisampleStateCreateInfo multisampling{};
		multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampling.sampleShadingEnable = VK_FALSE;
		multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		multisampling.minSampleShading = 1.0f; // Optional
		multisampling.pSampleMask = nullptr; // Optional
		multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
		multisampling.alphaToOneEnable = VK_FALSE; // Optional

		VkPipelineColorBlendAttachmentState colorBlendAttachment{};
		colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
		colorBlendAttachment.blendEnable = VK_FALSE;
		colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
		colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
		colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD; // Optional
		colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
		colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
		colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD; // Optional

		VkPipelineColorBlendStateCreateInfo colorBlending{};
		colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlending.logicOpEnable = VK_FALSE;
		colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
		colorBlending.attachmentCount = 1;
		colorBlending.pAttachments = &colorBlendAttachment;
		colorBlending.blendConstants[0] = 0.0f; // Optional
		colorBlending.blendConstants[1] = 0.0f; // Optional
		colorBlending.blendConstants[2] = 0.0f; // Optional
		colorBlending.blendConstants[3] = 0.0f; // Optional

		VkDynamicState dynamicStates[] = {
			VK_DYNAMIC_STATE_VIEWPORT,
			VK_DYNAMIC_STATE_LINE_WIDTH
		};

		VkPipelineDynamicStateCreateInfo dynamicState{};
		dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicState.dynamicStateCount = 2;
		dynamicState.pDynamicStates = dynamicStates;

		VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
		pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutInfo.setLayoutCount = 0; // Optional
		pipelineLayoutInfo.pSetLayouts = nullptr; // Optional
		pipelineLayoutInfo.pushConstantRangeCount = 0; // Optional
		pipelineLayoutInfo.pPushConstantRanges = nullptr; // Optional

		VK_CHECK(vkCreatePipelineLayout(m_device, &pipelineLayoutInfo, nullptr, &m_pipelineLayout));

		VkGraphicsPipelineCreateInfo pipelineInfo{};
		pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineInfo.stageCount = 2;
		pipelineInfo.pStages = shaderStages;

		pipelineInfo.pVertexInputState = &vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &rasterizer;
		pipelineInfo.pMultisampleState = &multisampling;
		pipelineInfo.pDepthStencilState = nullptr; // Optional
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDynamicState = nullptr; // Optional

		pipelineInfo.layout = m_pipelineLayout;

		pipelineInfo.renderPass = *m_renderPass;
		pipelineInfo.subpass = 0;

		pipelineInfo.basePipelineHandle = VK_NULL_HANDLE; // Optional
		pipelineInfo.basePipelineIndex = -1; // Optional		

		VK_CHECK(vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_graphicsPipeline));
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
			std::vector<TRefPtr<VulkanImageView>>{ swapChainImageViews[i] },
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

	/*for (size_t i = 0; i < m_commandBuffers.size(); i++)
	{
		m_commandBuffers[i]->BeginCommandList();
		{
			m_commandBuffers[i]->BeginRenderPass(m_renderPass, m_swapChainFramebuffers[i], m_swapchain->GetExtent());

			vkCmdBindPipeline(*m_commandBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);

			m_commandBuffers[i]->BindVertexBuffers({ m_vertexBuffer });
			m_commandBuffers[i]->BindIndexBuffer(m_indexBuffer);
			m_commandBuffers[i]->DrawIndexed(m_indexBuffer);
			m_commandBuffers[i]->EndRenderPass();
		}
		m_commandBuffers[i]->EndCommandList();
	}*/
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

	m_swapchain = TRefPtr<VulkanSwapchain>::Make(m_physicalDevice,
		TRefPtr<VulkanDevice>(this),
		m_surface,
		viewport->GetWidth(),
		viewport->GetHeight(),
		viewport->IsVsyncRequested(),
		oldSwapchain);
}

void VulkanDevice::CleanupSwapChain()
{
	m_swapChainFramebuffers.clear();
	m_commandBuffers.clear();

	vkDestroyPipeline(m_device, m_graphicsPipeline, nullptr);
	vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr);

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

void VulkanDevice::FixLostDevice(const Win32::Window* pViewport)
{
	if (IsSwapChainOutdated())
	{
		RecreateSwapchain(pViewport);
	}
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

		vkCmdBindPipeline(*m_commandBuffers[imageIndex], VK_PIPELINE_BIND_POINT_GRAPHICS, m_graphicsPipeline);
		m_commandBuffers[imageIndex]->BindVertexBuffers({ m_vertexBuffer });
		m_commandBuffers[imageIndex]->BindIndexBuffer(m_indexBuffer);
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

