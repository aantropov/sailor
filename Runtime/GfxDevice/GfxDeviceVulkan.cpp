#include "GfxDeviceVulkan.h"
#include <wtypes.h>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_win32.h>
#include "LogMacros.h"
#include <assert.h>
#include <iostream>
#include <cstdint> 
#include <map>
#include <vector>
#include <optional>
#include <set>

#include "Sailor.h"

using namespace Sailor;

#define VK_CHECK(call) \
	do { \
		VkResult result_ = call; \
		assert(result_ == VK_SUCCESS); \
	} while (0)

#define NUM_ELEMENTS(array) (sizeof(array) / sizeof(array[0]))

GfxDeviceVulkan* GfxDeviceVulkan::instance = nullptr;

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{

	if (messageSeverity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		std::cerr << "!!! Validation layer: " << pCallbackData->pMessage << std::endl;
	}

	std::cerr << "Validation layer: " << pCallbackData->pMessage << std::endl;
	return VK_FALSE;

}

void PopulateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo)
{
	createInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };

	createInfo.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;

	createInfo.messageType =
		VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = VulkanDebugCallback;
}

void GfxDeviceVulkan::CreateGraphicsPipeline()
{

	
}

void GfxDeviceVulkan::CreateInstance(const Window* viewport, bool bInIsEnabledValidationLayers)
{
	if (instance != nullptr)
	{
		SAILOR_LOG("Vulkan already initialized!");
		return;
	}

	instance = new GfxDeviceVulkan();
	instance->bIsEnabledValidationLayers = bInIsEnabledValidationLayers;

	SAILOR_LOG("Num supported Vulkan extensions: %d", GfxDeviceVulkan::GetNumSupportedExtensions());
	PrintSupportedExtensions();

	// Create Vulkan instance
	VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };

	appInfo.pApplicationName = EngineInstance::ApplicationName.c_str();
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "Sailor";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_2;

	std::vector<const char*> extensions =
	{
		VK_KHR_SURFACE_EXTENSION_NAME,
		"VK_KHR_win32_surface",
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#ifdef VK_USE_PLATFORM_WIN32_KHR
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
	};

	if (instance->bIsEnabledValidationLayers)
	{
		extensions.push_back(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
	}

	VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	createInfo.pApplicationInfo = &appInfo;
	createInfo.ppEnabledExtensionNames = extensions.data();
	createInfo.enabledExtensionCount = extensions.size();

	const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation" };

	VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo;
	if (instance->bIsEnabledValidationLayers)
	{
		createInfo.ppEnabledLayerNames = validationLayers.data();
		createInfo.enabledLayerCount = validationLayers.size();

		PopulateDebugMessengerCreateInfo(debugCreateInfo);
		createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;

		if (!CheckValidationLayerSupport(validationLayers))
		{
			SAILOR_LOG("Not all debug layers are supported");
		}
	}
	else
	{
		createInfo.enabledLayerCount = 0;
	}

	GetVkInstance() = 0;
	VK_CHECK(vkCreateInstance(&createInfo, 0, &GetVkInstance()));

	SetupDebugCallback();

	// Create Win32 surface
	instance->CreateWin32Surface(viewport);

	// Pick & Create device
	instance->mainPhysicalDevice = PickPhysicalDevice();
	instance->CreateLogicalDevice(instance->mainPhysicalDevice);

	// Create swapchain
	instance->CreateSwapchain(viewport);

	SAILOR_LOG("Vulkan initialized");
}

void GfxDeviceVulkan::CreateLogicalDevice(VkPhysicalDevice physicalDevice)
{
	queueFamilies = FindQueueFamilies(physicalDevice);

	std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
	std::set<uint32_t> uniqueQueueFamilies = { queueFamilies.graphicsFamily.value(), queueFamilies.presentFamily.value() };

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
	GetRequiredDeviceExtensions(deviceExtensions);

	createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	createInfo.ppEnabledExtensionNames = deviceExtensions.data();

	createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
	createInfo.pQueueCreateInfos = queueCreateInfos.data();
	createInfo.pEnabledFeatures = &deviceFeatures;

	// Compatibility with older Vulkan drivers
	const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation" };
	if (instance->bIsEnabledValidationLayers)
	{
		createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
		createInfo.ppEnabledLayerNames = validationLayers.data();
	}
	else
	{
		createInfo.enabledLayerCount = 0;
	}

	VK_CHECK(vkCreateDevice(physicalDevice, &createInfo, nullptr, &instance->device));

	// Create queues
	vkGetDeviceQueue(device, queueFamilies.graphicsFamily.value(), 0, &graphicsQueue);
	vkGetDeviceQueue(device, queueFamilies.presentFamily.value(), 0, &presentQueue);
}

void GfxDeviceVulkan::CreateWin32Surface(const Window* viewport)
{
	VkWin32SurfaceCreateInfoKHR createInfoWin32{ VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
	createInfoWin32.hwnd = viewport->GetHWND();
	createInfoWin32.hinstance = viewport->GetHINSTANCE();
	VK_CHECK(vkCreateWin32SurfaceKHR(GetVkInstance(), &createInfoWin32, nullptr, &surface));
}

void GfxDeviceVulkan::CreateSwapchain(const Window* viewport)
{
	SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(mainPhysicalDevice);

	surfaceFormat = ChooseSwapSurfaceFormat(swapChainSupport.formats);
	presentMode = ÑhooseSwapPresentMode(swapChainSupport.presentModes);
	extent = ChooseSwapExtent(swapChainSupport.capabilities, viewport);

	uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;

	if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount)
	{
		imageCount = swapChainSupport.capabilities.maxImageCount;
	}

	VkSwapchainCreateInfoKHR createSwapChainInfo{ VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
	createSwapChainInfo.surface = surface;
	createSwapChainInfo.minImageCount = imageCount;
	createSwapChainInfo.imageFormat = surfaceFormat.format;
	createSwapChainInfo.imageColorSpace = surfaceFormat.colorSpace;
	createSwapChainInfo.imageExtent = extent;
	createSwapChainInfo.imageArrayLayers = 1;
	createSwapChainInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT; //TODO: Use VK_IMAGE_USAGE_TRANSFER_DST_BIT for Post Processing

	QueueFamilyIndices indices = FindQueueFamilies(mainPhysicalDevice);
	uint32_t queueFamilyIndices[] = { indices.graphicsFamily.value(), indices.presentFamily.value() };

	if (indices.graphicsFamily != indices.presentFamily)
	{
		createSwapChainInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createSwapChainInfo.queueFamilyIndexCount = 2;
		createSwapChainInfo.pQueueFamilyIndices = queueFamilyIndices;
	}
	else
	{
		createSwapChainInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		createSwapChainInfo.queueFamilyIndexCount = 0; // Optional
		createSwapChainInfo.pQueueFamilyIndices = nullptr; // Optional
	}

	createSwapChainInfo.preTransform = swapChainSupport.capabilities.currentTransform;
	createSwapChainInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;

	createSwapChainInfo.presentMode = presentMode;
	createSwapChainInfo.clipped = VK_TRUE;
	createSwapChainInfo.oldSwapchain = VK_NULL_HANDLE;

	VK_CHECK(vkCreateSwapchainKHR(device, &createSwapChainInfo, nullptr, &swapChain));

	// Create Swapchain images & image views

	VK_CHECK(vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr));
	swapChainImages.resize(imageCount);
	VK_CHECK(vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data()));

	swapChainImageViews.resize(swapChainImages.size());

	for (size_t i = 0; i < swapChainImages.size(); i++)
	{
		VkImageViewCreateInfo createInfo{ VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
		createInfo.image = swapChainImages[i];
		createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		createInfo.format = surfaceFormat.format;

		createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
		createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;

		createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		createInfo.subresourceRange.baseMipLevel = 0;
		createInfo.subresourceRange.levelCount = 1;
		createInfo.subresourceRange.baseArrayLayer = 0;
		createInfo.subresourceRange.layerCount = 1;

		VK_CHECK(vkCreateImageView(device, &createInfo, nullptr, &swapChainImageViews[i]));
	}
}

uint32_t GfxDeviceVulkan::GetNumSupportedExtensions()
{
	uint32_t extensionCount;
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
	return extensionCount;
}

void GfxDeviceVulkan::PrintSupportedExtensions()
{
	uint32_t extensionNum = GetNumSupportedExtensions();

	std::vector<VkExtensionProperties> extensions(extensionNum);
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionNum,
		extensions.data()));

	printf("Vulkan available extensions:\n");
	for (const auto& extension : extensions)
	{
		std::cout << '\t' << extension.extensionName << '\n';
	}
}

bool GfxDeviceVulkan::CheckValidationLayerSupport(const std::vector<const char*>& validationLayers)
{
	uint32_t layerCount;
	VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));

	std::vector<VkLayerProperties> availableLayers(layerCount);
	VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount,
		availableLayers.data()));

	for (const std::string& layerName : validationLayers)
	{
		bool layerFound = false;

		for (const auto& layerProperties : availableLayers)
		{
			if (strcmp(layerName.c_str(), layerProperties.layerName) == 0)
			{
				layerFound = true;
				break;
			}
		}

		if (!layerFound)
		{
			return false;
		}
	}
	return true;
}

void GfxDeviceVulkan::Shutdown()
{
	if (instance->bIsEnabledValidationLayers)
	{
		DestroyDebugUtilsMessengerEXT(GetVkInstance(), instance->debugMessenger, nullptr);
	}

	for (const auto& imageView : instance->swapChainImageViews)
	{
		vkDestroyImageView(instance->device, imageView, nullptr);
	}
	
	vkDestroySwapchainKHR(instance->device, instance->swapChain, nullptr);
	vkDestroyDevice(instance->device, nullptr);
	vkDestroySurfaceKHR(GetVkInstance(), instance->surface, nullptr);
	vkDestroyInstance(GetVkInstance(), nullptr);

	delete(instance);
}

bool GfxDeviceVulkan::SetupDebugCallback()
{
	if (!instance->bIsEnabledValidationLayers)
	{
		return false;
	}

	VkDebugUtilsMessengerCreateInfoEXT createInfo;
	PopulateDebugMessengerCreateInfo(createInfo);

	VK_CHECK(CreateDebugUtilsMessengerEXT(GetVkInstance(), &createInfo, nullptr, &instance->debugMessenger));

	return true;
}

VkPhysicalDevice GfxDeviceVulkan::PickPhysicalDevice()
{
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

	uint32_t deviceCount = 0;
	VK_CHECK(vkEnumeratePhysicalDevices(GetVkInstance(), &deviceCount, nullptr));

	if (deviceCount == 0)
	{
		SAILOR_LOG("Failed to find GPUs with Vulkan support!");
		return VK_NULL_HANDLE;
	}

	std::vector<VkPhysicalDevice> devices(deviceCount);
	std::multimap<int, VkPhysicalDevice> candidates;

	VK_CHECK(vkEnumeratePhysicalDevices(GetVkInstance(), &deviceCount, devices.data()));

	for (const auto& device : devices)
	{
		if (IsDeviceSuitable(device))
		{
			int score = GetDeviceScore(device);
			candidates.insert(std::make_pair(score, device));
		}
	}

	if (candidates.rbegin()->first > 0)
	{
		physicalDevice = candidates.rbegin()->second;
	}
	else
	{
		SAILOR_LOG("Failed to find a suitable GPU!");
	}

	return physicalDevice;
}

bool GfxDeviceVulkan::CheckDeviceExtensionSupport(VkPhysicalDevice device)
{
	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

	std::vector<VkExtensionProperties> availableExtensions(extensionCount);
	VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data()));

	std::vector<const char*> deviceExtensions;
	GetRequiredDeviceExtensions(deviceExtensions);

	std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

	for (const auto& extension : availableExtensions)
	{
		requiredExtensions.erase(extension.extensionName);
	}

	return requiredExtensions.empty();
}

bool GfxDeviceVulkan::IsDeviceSuitable(VkPhysicalDevice device)
{
	QueueFamilyIndices indices = FindQueueFamilies(device);

	bool extensionsSupported = CheckDeviceExtensionSupport(device);

	bool swapChainFits = false;
	if (extensionsSupported)
	{
		SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device);
		swapChainFits = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
	}

	return indices.IsComplete() && extensionsSupported && swapChainFits;
}

int GfxDeviceVulkan::GetDeviceScore(VkPhysicalDevice device)
{
	VkPhysicalDeviceProperties deviceProperties;
	VkPhysicalDeviceFeatures deviceFeatures;

	vkGetPhysicalDeviceProperties(device, &deviceProperties);
	vkGetPhysicalDeviceFeatures(device, &deviceFeatures);

	int score = 0;
	// Discrete GPUs have a significant performance advantage
	if (deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
	{
		score += 1000;
	}

	// Maximum possible size of textures affects graphics quality
	score += deviceProperties.limits.maxImageDimension2D;

	// Application can't function without geometry shaders
	if (!deviceFeatures.geometryShader)
	{
		return 0;
	}

	return score;
}

GfxDeviceVulkan::QueueFamilyIndices GfxDeviceVulkan::FindQueueFamilies(VkPhysicalDevice device)
{
	QueueFamilyIndices indices;

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

	std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

	int i = 0;
	for (const auto& queueFamily : queueFamilies)
	{
		if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			indices.graphicsFamily = i;
		}

		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, instance->surface, &presentSupport);

		if (presentSupport)
		{
			indices.presentFamily = i;
		}

		i++;
	}

	return indices;
}

GfxDeviceVulkan::SwapChainSupportDetails GfxDeviceVulkan::QuerySwapChainSupport(VkPhysicalDevice device)
{
	SwapChainSupportDetails details;

	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, instance->surface, &details.capabilities));

	uint32_t formatCount;
	VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, instance->surface, &formatCount, nullptr));

	if (formatCount != 0)
	{
		details.formats.resize(formatCount);
		VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, instance->surface, &formatCount, details.formats.data()));
	}

	uint32_t presentModeCount;
	VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, instance->surface, &presentModeCount, nullptr));

	if (presentModeCount != 0)
	{
		details.presentModes.resize(presentModeCount);
		VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, instance->surface, &presentModeCount, details.presentModes.data()));
	}

	return details;
}

VkSurfaceFormatKHR GfxDeviceVulkan::ChooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats)
{
	for (const auto& availableFormat : availableFormats)
	{
		if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			return availableFormat;
		}
	}

	return availableFormats[0];
}

VkPresentModeKHR GfxDeviceVulkan::ÑhooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes)
{
	for (const auto& availablePresentMode : availablePresentModes)
	{
		if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR)
		{
			return availablePresentMode;
		}
	}

	return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D GfxDeviceVulkan::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, const Window* viewport)
{
	if (capabilities.currentExtent.width != UINT32_MAX)
	{
		return capabilities.currentExtent;
	}

	VkExtent2D actualExtent = { static_cast<uint32_t>(viewport->GetWidth()), static_cast<uint32_t>(viewport->GetHeight()) };

	actualExtent.width = max(capabilities.minImageExtent.width, min(capabilities.maxImageExtent.width, actualExtent.width));
	actualExtent.height = max(capabilities.minImageExtent.height, min(capabilities.maxImageExtent.height, actualExtent.height));

	return actualExtent;
}

VkResult GfxDeviceVulkan::CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger)
{
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if (func != nullptr)
	{
		return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	}
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void GfxDeviceVulkan::DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator)
{
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	if (func != nullptr)
	{
		func(instance, debugMessenger, pAllocator);
	}
}