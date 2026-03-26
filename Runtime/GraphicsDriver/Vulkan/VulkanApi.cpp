#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include <algorithm>
#include <cstring>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif
#ifdef __APPLE__
#include <vulkan/vulkan_metal.h>
#include <vulkan/vulkan_macos.h>
#include <cstdlib>
#endif
#include "Containers/Map.h"
#include "Core/LogMacros.h"
#include "Containers/Vector.h"
#include "Containers/Set.h"
#include "Sailor.h"
#include "Platform/Win32/Window.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Shader/ShaderCompiler.h"
#include "VulkanDevice.h"
#include "VulkanSwapchain.h"
#include "VulkanRenderPass.h"
#include "VulkanImage.h"
#include "VulkanImageView.h"
#include "VulkanDeviceMemory.h"
#include "VulkanBuffer.h"
#include "VulkanFence.h"
#include "VulkanCommandBuffer.h"
#include "VulkanDescriptors.h"
#include "VulkanPipeline.h"
#include "VulkanShaderModule.h"
#include "RHI/Types.h"
#include "RHI/VertexDescription.h"
#include "Engine/EngineLoop.h"
#include "VulkanMemory.h"
#include "VulkanBufferMemory.h"
#include "VulkanDescriptors.h"

using namespace Sailor;

namespace
{
	static uint32_t GetBytesPerPixel(VkFormat format)
	{
		switch (format)
		{
		case VK_FORMAT_R8G8B8A8_UNORM:
		case VK_FORMAT_R8G8B8A8_SNORM:
		case VK_FORMAT_R8G8B8A8_USCALED:
		case VK_FORMAT_R8G8B8A8_SSCALED:
		case VK_FORMAT_R8G8B8A8_UINT:
		case VK_FORMAT_R8G8B8A8_SINT:
		case VK_FORMAT_R8G8B8A8_SRGB:
		case VK_FORMAT_B8G8R8A8_UNORM:
		case VK_FORMAT_B8G8R8A8_SNORM:
		case VK_FORMAT_B8G8R8A8_USCALED:
		case VK_FORMAT_B8G8R8A8_SSCALED:
		case VK_FORMAT_B8G8R8A8_UINT:
		case VK_FORMAT_B8G8R8A8_SINT:
		case VK_FORMAT_B8G8R8A8_SRGB:
		case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_SNORM_PACK32:
		case VK_FORMAT_A2R10G10B10_USCALED_PACK32:
		case VK_FORMAT_A2R10G10B10_SSCALED_PACK32:
		case VK_FORMAT_A2R10G10B10_UINT_PACK32:
		case VK_FORMAT_A2R10G10B10_SINT_PACK32:
		case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		case VK_FORMAT_A2B10G10R10_SNORM_PACK32:
		case VK_FORMAT_A2B10G10R10_USCALED_PACK32:
		case VK_FORMAT_A2B10G10R10_SSCALED_PACK32:
		case VK_FORMAT_A2B10G10R10_UINT_PACK32:
		case VK_FORMAT_A2B10G10R10_SINT_PACK32:
		case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
		case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
			return 4u;

		case VK_FORMAT_R16G16B16A16_UNORM:
		case VK_FORMAT_R16G16B16A16_SNORM:
		case VK_FORMAT_R16G16B16A16_USCALED:
		case VK_FORMAT_R16G16B16A16_SSCALED:
		case VK_FORMAT_R16G16B16A16_UINT:
		case VK_FORMAT_R16G16B16A16_SINT:
		case VK_FORMAT_R16G16B16A16_SFLOAT:
			return 8u;

		case VK_FORMAT_R32G32B32A32_UINT:
		case VK_FORMAT_R32G32B32A32_SINT:
		case VK_FORMAT_R32G32B32A32_SFLOAT:
			return 16u;

		default:
			return 0u;
		}
	}
}
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
	if (messageSeverity > VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
	{
		SAILOR_LOG_ERROR("Validation layer: %s", pCallbackData->pMessage);
	}
	else
	{
		std::cout << "Validation layer: " << pCallbackData->pMessage << std::endl;
	}

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

void VulkanApi::Initialize(Platform::Window* viewport, RHI::EMsaaSamples msaaSamples, bool bInIsEnabledValidationLayers)
{
	SAILOR_PROFILE_FUNCTION();

	if (s_pInstance != nullptr)
	{
		SAILOR_LOG("Vulkan already initialized!");
		return;
	}

	s_pInstance = new VulkanApi();
	s_pInstance->bIsEnabledValidationLayers = bInIsEnabledValidationLayers;

	SAILOR_LOG("Num supported Vulkan extensions: %d", VulkanApi::GetNumSupportedExtensions());
	PrintSupportedExtensions();

#if defined(__APPLE__)
	// MoltenVK clip-space fixup (historically exposed as shouldFixupClipSpace).
	// Keep this enabled so Y conversion is handled consistently in shader conversion.
	setenv("MVK_CONFIG_SHADER_CONVERSION_FLIP_VERTEX_Y", "1", 1);
	SAILOR_LOG("MoltenVK config: MVK_CONFIG_SHADER_CONVERSION_FLIP_VERTEX_Y=1");

	// Prefer Metal argument buffers on macOS for descriptor-heavy paths.
	setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 1);
	SAILOR_LOG("MoltenVK config: MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS=1");
#endif

	uint32_t availableExtensionCount = 0;
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, nullptr));
	TVector<VkExtensionProperties> availableExtensions(availableExtensionCount);
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &availableExtensionCount, availableExtensions.GetData()));

	const auto hasInstanceExtension = [&](const char* extensionName)
	{
		for (const auto& extension : availableExtensions)
		{
			if (strcmp(extension.extensionName, extensionName) == 0)
			{
				return true;
			}
		}
		return false;
	};

	// Create Vulkan instance
	VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };

	appInfo.pApplicationName = App::GetApplicationName();
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = App::GetEngineName();
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);

#if defined(VK_API_VERSION_1_3)
	uint32_t requestedApiVersion = VK_API_VERSION_1_3;
#elif defined(VK_API_VERSION_1_2)
	uint32_t requestedApiVersion = VK_API_VERSION_1_2;
#else
	uint32_t requestedApiVersion = VK_API_VERSION_1_1;
#endif

	uint32_t loaderApiVersion = VK_API_VERSION_1_0;
	const auto pfnEnumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
	if (pfnEnumerateInstanceVersion)
	{
		pfnEnumerateInstanceVersion(&loaderApiVersion);
	}

	const uint32_t clampedRequestedApiVersion = std::min(requestedApiVersion, loaderApiVersion);
	appInfo.apiVersion = clampedRequestedApiVersion;
	SAILOR_LOG("Vulkan API version (requested=%u.%u.%u, loader=%u.%u.%u, initial=%u.%u.%u)",
		VK_VERSION_MAJOR(requestedApiVersion), VK_VERSION_MINOR(requestedApiVersion), VK_VERSION_PATCH(requestedApiVersion),
		VK_VERSION_MAJOR(loaderApiVersion), VK_VERSION_MINOR(loaderApiVersion), VK_VERSION_PATCH(loaderApiVersion),
		VK_VERSION_MAJOR(appInfo.apiVersion), VK_VERSION_MINOR(appInfo.apiVersion), VK_VERSION_PATCH(appInfo.apiVersion));

	TVector<const char*> extensions =
	{
		VK_KHR_SURFACE_EXTENSION_NAME,
#if defined(_WIN32)
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#elif defined(__APPLE__)
		nullptr,
#endif
	};

#if defined(__APPLE__)
	if (hasInstanceExtension(VK_EXT_METAL_SURFACE_EXTENSION_NAME))
	{
		extensions[1] = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
	}
	else if (hasInstanceExtension(VK_MVK_MACOS_SURFACE_EXTENSION_NAME))
	{
		extensions[1] = VK_MVK_MACOS_SURFACE_EXTENSION_NAME;
	}
	else
	{
		SAILOR_LOG_ERROR("No supported Vulkan surface extension found for macOS.");
		extensions[1] = VK_MVK_MACOS_SURFACE_EXTENSION_NAME;
	}
#endif

	if (hasInstanceExtension(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
	{
		extensions.Add(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
	}

	bool bEnablePortabilityEnumeration = false;
#if defined(__APPLE__)
	if (hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
	{
		extensions.Add(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		bEnablePortabilityEnumeration = true;
	}
	else
	{
		SAILOR_LOG("Optional Vulkan extension is not supported: %s", VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
	}
#endif

	VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };

	createInfo.pApplicationInfo = &appInfo;
	createInfo.ppEnabledExtensionNames = extensions.GetData();
	createInfo.enabledExtensionCount = (uint32_t)extensions.Num();
	createInfo.pNext = nullptr;
#if defined(__APPLE__) && defined(VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR)
	if (bEnablePortabilityEnumeration)
	{
		createInfo.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}
#endif

	VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo;
	const TVector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation", "VK_LAYER_KHRONOS_synchronization2" };

	if (s_pInstance->bIsEnabledValidationLayers)
	{
		if (CheckValidationLayerSupport(validationLayers))
		{
			extensions.Add(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#ifndef _SHIPPING
			extensions.Add(VK_EXT_DEBUG_REPORT_EXTENSION_NAME);
#endif

			createInfo.ppEnabledLayerNames = validationLayers.GetData();
			createInfo.enabledLayerCount = (uint32_t)validationLayers.Num();

			PopulateDebugMessengerCreateInfo(debugCreateInfo);
			createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
		}
		else
		{
			SAILOR_LOG("Not all debug layers are supported. Validation layers will be disabled.");
			s_pInstance->bIsEnabledValidationLayers = false;
			createInfo.ppEnabledLayerNames = nullptr;
			createInfo.enabledLayerCount = 0;
		}
	}
	else
	{
		createInfo.ppEnabledLayerNames = nullptr;
		createInfo.enabledLayerCount = 0;
	}

	createInfo.ppEnabledExtensionNames = extensions.GetData();
	createInfo.enabledExtensionCount = (uint32_t)extensions.Num();

	for (const auto& extensionName : extensions)
	{
		SAILOR_LOG("Enabling Vulkan instance extension: %s", extensionName);
	}

	TVector<uint32_t> apiVersionFallbacks;
	apiVersionFallbacks.Add(clampedRequestedApiVersion);
	for (uint32_t minor = VK_VERSION_MINOR(clampedRequestedApiVersion); minor > 0; --minor)
	{
		apiVersionFallbacks.Add(VK_MAKE_API_VERSION(0, 1, minor - 1, 0));
	}

	GetVkInstance() = 0;
	VkResult createInstanceResult = VK_ERROR_INITIALIZATION_FAILED;
	for (const uint32_t fallbackApiVersion : apiVersionFallbacks)
	{
		appInfo.apiVersion = fallbackApiVersion;
		createInstanceResult = vkCreateInstance(&createInfo, 0, &GetVkInstance());
		SAILOR_LOG("vkCreateInstance(api=%u.%u.%u) -> %d",
			VK_VERSION_MAJOR(fallbackApiVersion),
			VK_VERSION_MINOR(fallbackApiVersion),
			VK_VERSION_PATCH(fallbackApiVersion),
			(int32_t)createInstanceResult);
		if (createInstanceResult == VK_SUCCESS)
		{
			SAILOR_LOG("Created Vulkan instance with API version %u.%u.%u",
				VK_VERSION_MAJOR(fallbackApiVersion),
				VK_VERSION_MINOR(fallbackApiVersion),
				VK_VERSION_PATCH(fallbackApiVersion));
			break;
		}

		if (createInstanceResult != VK_ERROR_INCOMPATIBLE_DRIVER)
		{
			break;
		}
	}

	if (createInstanceResult != VK_SUCCESS)
	{
#if defined(__APPLE__) && defined(SAILOR_VULKAN_DIRECT_MOLTENVK_FALLBACK)
		if (createInstanceResult == VK_ERROR_INCOMPATIBLE_DRIVER)
		{
			SAILOR_LOG_ERROR("Vulkan loader is unavailable; Sailor is linked directly against MoltenVK fallback. "
				"On this setup vkCreateInstance() is rejected by the ICD. "
				"Install/link a Vulkan loader (libvulkan / vulkan-loader) and reconfigure CMake.");
		}
#endif
		SAILOR_LOG_ERROR("Failed to create Vulkan instance. vkCreateInstance returned %d", (int32_t)createInstanceResult);
		VulkanApi::Shutdown();
		return;
	}

	SetupDebugCallback();

	s_pInstance->m_device = VulkanDevicePtr::Make(viewport, msaaSamples);

	SAILOR_LOG("Vulkan initialized");
}

void VulkanApi::WaitIdle()
{
	s_pInstance->m_device->WaitIdle();
}

VulkanDevicePtr VulkanApi::GetMainDevice() const
{
	return m_device;
}

uint32_t VulkanApi::GetNumSupportedExtensions()
{
	uint32_t extensionCount;
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr));
	return extensionCount;
}

void VulkanApi::PrintSupportedExtensions()
{
	uint32_t extensionNum = GetNumSupportedExtensions();

	TVector<VkExtensionProperties> extensions(extensionNum);
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionNum,
		extensions.GetData()));

	printf("Vulkan available extensions:\n");
	for (const auto& extension : extensions)
	{
		std::cout << '\t' << extension.extensionName << '\n';
	}
}

bool VulkanApi::CheckValidationLayerSupport(const TVector<const char*>& validationLayers)
{
	uint32_t layerCount;
	VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount, nullptr));

	TVector<VkLayerProperties> availableLayers(layerCount);
	VK_CHECK(vkEnumerateInstanceLayerProperties(&layerCount,
		availableLayers.GetData()));

	for (const std::string& layerName : validationLayers)
	{
		bool bIsLayerFound = false;

		for (const auto& layerProperties : availableLayers)
		{
			if (strcmp(layerName.c_str(), layerProperties.layerName) == 0)
			{
				bIsLayerFound = true;
				break;
			}
		}

		if (!bIsLayerFound)
		{
			return false;
		}
	}
	return true;
}

VulkanApi::~VulkanApi()
{
	if (bIsEnabledValidationLayers)
	{
		DestroyDebugUtilsMessengerEXT(GetVkInstance(), m_debugMessenger, nullptr);
	}

	m_device.Clear();
	vkDestroyInstance(GetVkInstance(), nullptr);
}

bool VulkanApi::SetupDebugCallback()
{
	if (!s_pInstance->bIsEnabledValidationLayers)
	{
		return false;
	}

	VkDebugUtilsMessengerCreateInfoEXT createInfo;
	PopulateDebugMessengerCreateInfo(createInfo);

	VK_CHECK(CreateDebugUtilsMessengerEXT(GetVkInstance(), &createInfo, nullptr, &s_pInstance->m_debugMessenger));

	return true;
}

VkPhysicalDevice VulkanApi::PickPhysicalDevice(VulkanSurfacePtr surface)
{
	VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;

	if (GetVkInstance() == VK_NULL_HANDLE)
	{
		SAILOR_LOG_ERROR("Failed to enumerate physical devices: Vulkan instance is null.");
		return VK_NULL_HANDLE;
	}

	uint32_t deviceCount = 0;
	const VkResult enumResult = vkEnumeratePhysicalDevices(GetVkInstance(), &deviceCount, nullptr);
	if (enumResult != VK_SUCCESS)
	{
		SAILOR_LOG_ERROR("Failed to enumerate physical devices: vkEnumeratePhysicalDevices returned %d", (int32_t)enumResult);
		return VK_NULL_HANDLE;
	}

	if (deviceCount == 0)
	{
		SAILOR_LOG("Failed to find GPUs with Vulkan support!");
		return VK_NULL_HANDLE;
	}

	TVector<VkPhysicalDevice> devices(deviceCount);
	std::multimap<int, VkPhysicalDevice> candidates;

	if (vkEnumeratePhysicalDevices(GetVkInstance(), &deviceCount, devices.GetData()) != VK_SUCCESS)
	{
		SAILOR_LOG_ERROR("Failed to enumerate physical devices list.");
		return VK_NULL_HANDLE;
	}

	for (const auto& device : devices)
	{
		if (IsDeviceSuitable(device, surface))
		{
			int score = GetDeviceScore(device);
			candidates.insert(std::make_pair(score, device));
		}
	}

	if (!candidates.empty() && candidates.rbegin()->first > 0)
	{
		physicalDevice = candidates.rbegin()->second;
	}
	else
	{
		SAILOR_LOG("Failed to find a suitable GPU!");
	}

	return physicalDevice;
}

VulkanQueueFamilyIndices VulkanApi::FindQueueFamilies(VkPhysicalDevice device, VulkanSurfacePtr surface)
{
	VulkanQueueFamilyIndices indices;

	uint32_t queueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

	TVector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.GetData());

	int i = 0;
	for (const auto& queueFamily : queueFamilies)
	{
		if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT)
		{
			indices.m_graphicsFamily = i;
		}

		// Find first a queue that has just the transfer bit set
		if ((queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
			(queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT) == 0 &&
			(queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT))
		{
			indices.m_transferFamily = i;
		}

		// Find first a queue that has just the compute bit set
		if ((queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
			(queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT))
		{
			indices.m_computeFamily = i;
		}

		// Get queue that is shared 
		if (!indices.m_computeFamily.has_value() && (queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT))
		{
			indices.m_computeFamily = i;
		}

		// Get queue that is shared 
		if (!indices.m_transferFamily.has_value() && (queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT))
		{
			indices.m_transferFamily = i;
		}

		VkBool32 presentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, (VkSurfaceKHR)*surface, &presentSupport);

		if (presentSupport)
		{
			indices.m_presentFamily = i;
		}

		/*SAILOR_LOG("VkQueueFamily:%d, Graphics: %d, Compute:%d, Transfer:%d, Present:%d", i,
			queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT ? 1 : 0,
			queueFamily.queueFlags & VK_QUEUE_COMPUTE_BIT ? 1 : 0,
			queueFamily.queueFlags & VK_QUEUE_TRANSFER_BIT ? 1 : 0,
			presentSupport ? 1 : 0);*/

		i++;
	}

	return indices;
}

SwapChainSupportDetails VulkanApi::QuerySwapChainSupport(VkPhysicalDevice device, VulkanSurfacePtr surface)
{
	SAILOR_PROFILE_FUNCTION();

	SwapChainSupportDetails details;

	VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, *surface, &details.m_capabilities));

	uint32_t formatCount;
	VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, *surface, &formatCount, nullptr));

	if (formatCount != 0)
	{
		details.m_formats.AddDefault(formatCount);
		VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(device, *surface, &formatCount, details.m_formats.GetData()));
	}

	uint32_t presentModeCount;
	VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, *surface, &presentModeCount, nullptr));

	if (presentModeCount != 0)
	{
		details.m_presentModes.AddDefault(presentModeCount);
		VK_CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(device, *surface, &presentModeCount, details.m_presentModes.GetData()));
	}

	return details;
}

VkSurfaceFormatKHR VulkanApi::ChooseSwapSurfaceFormat(const TVector<VkSurfaceFormatKHR>& availableFormats)
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

VkPresentModeKHR VulkanApi::ChooseSwapPresentMode(const TVector<VkPresentModeKHR>& availablePresentModes, bool bVSync)
{
	if (bVSync)
	{
		return VK_PRESENT_MODE_FIFO_KHR;
	}

	for (const auto& availablePresentMode : availablePresentModes)
	{
		if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR ||
			availablePresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR)
		{
			return availablePresentMode;
		}
	}

	return VK_PRESENT_MODE_FIFO_KHR;
}

VkExtent2D VulkanApi::ChooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities, uint32_t width, uint32_t height)
{
	SAILOR_PROFILE_FUNCTION();

	if (capabilities.currentExtent.width != UINT32_MAX)
	{
		return capabilities.currentExtent;
	}

	VkExtent2D actualExtent = { width, height };

	actualExtent.width = std::max(capabilities.minImageExtent.width, std::min(capabilities.maxImageExtent.width, actualExtent.width));
	actualExtent.height = std::max(capabilities.minImageExtent.height, std::min(capabilities.maxImageExtent.height, actualExtent.height));

	return actualExtent;
}

TSet<string> VulkanApi::GetSupportedDeviceExtensions(VkPhysicalDevice device)
{
	TSet<string> supportedDeviceExtensions{};

	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

	TVector<VkExtensionProperties> availableExtensions(extensionCount);
	VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.GetData()));

	for (const auto& extension : availableExtensions)
	{
		supportedDeviceExtensions.Insert(std::string(extension.extensionName));
	}

	return supportedDeviceExtensions;
}

bool VulkanApi::CheckDeviceExtensionSupport(VkPhysicalDevice device)
{
	uint32_t extensionCount;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

	TVector<VkExtensionProperties> availableExtensions(extensionCount);
	VK_CHECK(vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.GetData()));

	TVector<const char*> deviceExtensions;
	TVector<const char*> instanceExtensions;
	GetRequiredExtensions(deviceExtensions, instanceExtensions);

	TSet<std::string> requiredExtensions;
	for (const auto& extension : deviceExtensions)
	{
		requiredExtensions.Insert(extension);
	}

	for (const auto& extension : availableExtensions)
	{
		requiredExtensions.Remove(extension.extensionName);
	}

	VkPhysicalDeviceProperties deviceProperties;
	vkGetPhysicalDeviceProperties(device, &deviceProperties);

	for (const auto& extension : requiredExtensions)
	{
		SAILOR_LOG_ERROR("Physical device %s, doesn't support required device extension: %s", deviceProperties.deviceName, extension.c_str());
	}

	return requiredExtensions.IsEmpty();
}

bool VulkanApi::IsDeviceSuitable(VkPhysicalDevice device, VulkanSurfacePtr surface)
{
	VulkanQueueFamilyIndices indices = FindQueueFamilies(device, surface);

	const bool extensionsSupported = CheckDeviceExtensionSupport(device);

	bool swapChainFits = false;
	if (extensionsSupported)
	{
		SwapChainSupportDetails swapChainSupport = QuerySwapChainSupport(device, surface);
		swapChainFits = !swapChainSupport.m_formats.IsEmpty() && !swapChainSupport.m_presentModes.IsEmpty();
	}

	VkPhysicalDeviceFeatures supportedFeatures;
	vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

	return indices.IsComplete() && extensionsSupported && swapChainFits && supportedFeatures.samplerAnisotropy;
}

int VulkanApi::GetDeviceScore(VkPhysicalDevice device)
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

	// Reward geometry shader support, but don't reject portability GPUs that don't expose it.
	if (deviceFeatures.geometryShader)
	{
		score += 128;
	}

	return score;
}

VkResult VulkanApi::CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger)
{
	auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
	if (func != nullptr)
	{
		return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
	}
	return VK_ERROR_EXTENSION_NOT_PRESENT;
}

void VulkanApi::DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const	VkAllocationCallbacks* pAllocator)
{
	auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
	if (func != nullptr)
	{
		func(instance, debugMessenger, pAllocator);
	}
}

VkAttachmentDescription VulkanApi::GetDefaultColorAttachment(VkFormat imageFormat)
{
	VkAttachmentDescription colorAttachment = {};
	colorAttachment.format = imageFormat;
	colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	return colorAttachment;
}

VkAttachmentDescription VulkanApi::GetDefaultDepthAttachment(VkFormat depthFormat)
{
	VkAttachmentDescription depthAttachment = {};
	depthAttachment.format = depthFormat;
	depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	return depthAttachment;
}

VulkanRenderPassPtr VulkanApi::CreateDefaultRenderPass(VulkanDevicePtr device, VkFormat imageFormat, VkFormat depthFormat)
{
	VkAttachmentReference colorAttachmentRef = {};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentRef = {};
	depthAttachmentRef.attachment = 1;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VulkanSubpassDescription subpass = {};
	subpass.m_pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.m_colorAttachments.Emplace(colorAttachmentRef);
	subpass.m_depthStencilAttachments.Emplace(depthAttachmentRef);

	// image layout transition
	VkSubpassDependency colorDependency = {};
	colorDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	colorDependency.dstSubpass = 0;
	colorDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	colorDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	colorDependency.srcAccessMask = 0;
	colorDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	colorDependency.dependencyFlags = 0;

	// depth buffer is shared between swap chain images
	VkSubpassDependency depthDependency = {};
	depthDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	depthDependency.dstSubpass = 0;
	depthDependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	depthDependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
	depthDependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	depthDependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	depthDependency.dependencyFlags = 0;

	return VulkanRenderPassPtr::Make(device,
		TVector<VkAttachmentDescription> {	GetDefaultColorAttachment(imageFormat), GetDefaultDepthAttachment(depthFormat) },
		TVector<VulkanSubpassDescription> { subpass },
		TVector<VkSubpassDependency> { colorDependency, depthDependency });
}

VulkanRenderPassPtr VulkanApi::CreateMSSRenderPass(VulkanDevicePtr device, VkFormat imageFormat, VkFormat depthFormat, VkSampleCountFlagBits samples)
{
	if (samples & VK_SAMPLE_COUNT_1_BIT)
	{
		return CreateDefaultRenderPass(device, imageFormat, depthFormat);
	}

	// First attachment is multisampled target.
	VkAttachmentDescription colorAttachment = {};
	colorAttachment.format = imageFormat;
	colorAttachment.samples = samples;
	colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	// Second attachment is the resolved image which will be presented.
	VkAttachmentDescription resolveAttachment = {};
	resolveAttachment.format = imageFormat;
	resolveAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
	resolveAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	resolveAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	resolveAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	resolveAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	resolveAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	resolveAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

	// Multisampled depth attachment. It won't be resolved.
	VkAttachmentDescription depthAttachment = {};
	depthAttachment.format = depthFormat;
	depthAttachment.samples = samples;
	depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	TVector<VkAttachmentDescription> attachments{ colorAttachment, resolveAttachment, depthAttachment };

	VkAttachmentReference colorAttachmentRef = {};
	colorAttachmentRef.attachment = 0;
	colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference resolveAttachmentRef = {};
	resolveAttachmentRef.attachment = 1;
	resolveAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

	VkAttachmentReference depthAttachmentRef = {};
	depthAttachmentRef.attachment = 2;
	depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

	VulkanSubpassDescription subpass;
	subpass.m_pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.m_colorAttachments.Emplace(colorAttachmentRef);
	subpass.m_resolveAttachments.Emplace(resolveAttachmentRef);
	subpass.m_depthStencilAttachments.Emplace(depthAttachmentRef);

	TVector<VulkanSubpassDescription> subpasses{ subpass };

	VkSubpassDependency dependency = {};
	dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	dependency.dstSubpass = 0;
	dependency.srcStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependency.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependency.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	VkSubpassDependency dependency2 = {};
	dependency2.srcSubpass = 0;
	dependency2.dstSubpass = VK_SUBPASS_EXTERNAL;
	dependency2.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	dependency2.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	dependency2.dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
	dependency2.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
	dependency2.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

	TVector<VkSubpassDependency> dependencies{ dependency, dependency2 };

	return VulkanRenderPassPtr::Make(device, attachments, subpasses, dependencies);
}

bool VulkanApi::HasStencilComponent(VkFormat format)
{
	return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

VkFormat VulkanApi::SelectFormatByFeatures(VkPhysicalDevice physicalDevice, const TVector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features)
{
	for (VkFormat format : candidates)
	{
		VkFormatProperties props;
		vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

		if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
			return format;
		}
		else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
			return format;
		}
	}

	SAILOR_LOG("Failed to find supported format!");
	return VkFormat::VK_FORMAT_UNDEFINED;
}

VkAccessFlags VulkanApi::ComputeAccessFlagsForFormat(VkFormat format)
{
	if (format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT)
	{
		return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}
	else if (format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_X8_D24_UNORM_PACK32)
	{
		return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}
	else
	{
		return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	}
}

VkImageAspectFlags VulkanApi::ComputeAspectFlagsForFormat(VkFormat format)
{
	if (format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT)
	{
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	}
	else if (format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_X8_D24_UNORM_PACK32)
	{
		return VK_IMAGE_ASPECT_DEPTH_BIT;
	}
	else
	{
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

VulkanImageViewPtr VulkanApi::CreateImageView(VulkanDevicePtr device, VulkanImagePtr image, VkImageAspectFlags aspectFlags)
{
	image->Compile();
	VulkanImageViewPtr imageView = VulkanImageViewPtr::Make(device, image, aspectFlags);
	imageView->Compile();

	return imageView;
}

VkVertexInputBindingDescription VulkanApi::GetBindingDescription(const RHI::RHIVertexDescriptionPtr& vertexDescription)
{
	VkVertexInputBindingDescription bindingDescription{};
	bindingDescription.binding = 0;
	bindingDescription.stride = (uint32_t)vertexDescription->GetVertexStride();
	bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	return bindingDescription;
}

TVector<VkVertexInputAttributeDescription> VulkanApi::GetAttributeDescriptions(const RHI::RHIVertexDescriptionPtr& vertexDescription, const TSet<uint32_t>& vertexAttributeBindings)
{
	const auto& attributes = vertexDescription->GetAttributeDescriptions();
	TVector<VkVertexInputAttributeDescription> attributeDescriptions;
	attributeDescriptions.Reserve(attributes.Num());

	for (uint32_t i = 0; i < attributes.Num(); i++)
	{
		// If the set is empty then we're going to bind all the attributes
		if (vertexAttributeBindings.Num() == 0 || vertexAttributeBindings.Contains(attributes[i].m_location))
		{
			VkVertexInputAttributeDescription attribute;

			attribute.binding = attributes[i].m_binding;
			attribute.location = attributes[i].m_location;
			attribute.format = (VkFormat)attributes[i].m_format;
			attribute.offset = attributes[i].m_offset;

			attributeDescriptions.Emplace(std::move(attribute));
		}
	}

	return attributeDescriptions;
}

uint32_t VulkanApi::FindMemoryByType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties)
{
	static bool s_bIsInited = false;
	static VkPhysicalDeviceMemoryProperties s_memProperties{};

	if (!s_bIsInited)
	{
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &s_memProperties);
		s_bIsInited = true;
	}

	for (uint32_t i = 0; i < s_memProperties.memoryTypeCount; i++)
	{
		if (typeFilter & (1 << i) && (s_memProperties.memoryTypes[i].propertyFlags & properties) == properties)
		{
			return i;
		}
	}

	SAILOR_LOG("Cannot find GPU memory!");
	check(false);

	return 0;
}

VulkanBufferPtr VulkanApi::CreateBuffer(VulkanDevicePtr device, VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkSharingMode sharingMode)
{
	VulkanBufferPtr outBuffer = VulkanBufferPtr::Make(device, size, usage, sharingMode);
	outBuffer->Compile();

	// TODO: Pass into Allocate the correct allignment for memory device allocation
	auto requirements = outBuffer->GetMemoryRequirements();

	//requirements.size += device->GetBufferImageGranuality();
	//requirements.alignment = std::max(requirements.alignment, device->GetBufferImageGranuality());

	auto data = device->GetMemoryAllocator(properties, requirements).Allocate(requirements.size, requirements.alignment);
	outBuffer->Bind(data);

	return outBuffer;
}

VulkanBufferPtr VulkanApi::CreateBuffer(VulkanCommandBufferPtr& cmdBuffer, VulkanDevicePtr device, const void* pData, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode)
{
	VulkanBufferPtr outbuffer = VulkanApi::CreateBuffer(
		device,
		size,
		VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		VK_SHARING_MODE_CONCURRENT);

	const auto requirements = outbuffer->GetMemoryRequirements();

	auto stagingBufferManagedPtr = device->GetStagingBufferAllocator()->Allocate(size, requirements.alignment);
	(*stagingBufferManagedPtr).m_buffer->GetMemoryDevice()->Copy((**stagingBufferManagedPtr).m_offset, size, pData);

	cmdBuffer->CopyBuffer(*stagingBufferManagedPtr, outbuffer->GetBufferMemoryPtr(), size);
	cmdBuffer->AddDependency(stagingBufferManagedPtr, device->GetStagingBufferAllocator());

	return outbuffer;
}

VulkanCommandBufferPtr VulkanApi::UpdateBuffer(VulkanDevicePtr device, const Memory::VulkanBufferMemoryPtr& dst, const void* pData, VkDeviceSize size)
{
	const auto requirements = dst.m_buffer->GetMemoryRequirements();

	auto stagingBufferManagedPtr = device->GetStagingBufferAllocator()->Allocate(size, requirements.alignment);
	(*stagingBufferManagedPtr).m_buffer->GetMemoryDevice()->Copy((**stagingBufferManagedPtr).m_offset, size, pData);

	auto cmdBuffer = device->CreateCommandBuffer(RHI::ECommandListQueue::Transfer);
	device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(VkCommandBuffer)*cmdBuffer, "VulkanApi::UpdateBuffer");
	cmdBuffer->BeginCommandList(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	cmdBuffer->CopyBuffer(*stagingBufferManagedPtr, dst, size, 0, 0);
	cmdBuffer->EndCommandList();
	cmdBuffer->AddDependency(stagingBufferManagedPtr, device->GetStagingBufferAllocator());

	return cmdBuffer;
}

VulkanBufferPtr VulkanApi::CreateBuffer_Immediate(VulkanDevicePtr device, const void* pData, VkDeviceSize size, VkBufferUsageFlags usage, VkSharingMode sharingMode)
{
	auto cmdBuffer = device->CreateCommandBuffer(RHI::ECommandListQueue::Transfer);
	device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(VkCommandBuffer)*cmdBuffer, "VulkanApi::CreateBuffer_Immediate");
	cmdBuffer->BeginCommandList(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	VulkanBufferPtr resBuffer = CreateBuffer(cmdBuffer, device, pData, size, usage, sharingMode);
	cmdBuffer->EndCommandList();

	auto fence = VulkanFencePtr::Make(device);
	device->SubmitCommandBuffer(cmdBuffer, fence);
	fence->Wait();

	return resBuffer;
}

void VulkanApi::CopyBuffer_Immediate(VulkanDevicePtr device, VulkanBufferMemoryPtr src, VulkanBufferMemoryPtr dst, VkDeviceSize size, VkDeviceSize srcOffset, VkDeviceSize dstOffset)
{
	auto fence = VulkanFencePtr::Make(device);

	auto cmdBuffer = device->CreateCommandBuffer(RHI::ECommandListQueue::Transfer);
	device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(VkCommandBuffer)*cmdBuffer, "VulkanApi::CopyBuffer_Immediate");
	cmdBuffer->BeginCommandList(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	cmdBuffer->CopyBuffer(src, dst, size, srcOffset, dstOffset);
	cmdBuffer->EndCommandList();
	device->SubmitCommandBuffer(cmdBuffer, fence);

	fence->Wait();
}

VulkanImagePtr VulkanApi::CreateImageUpload(
	VulkanCommandBufferPtr& cmdBuffer,
	VulkanDevicePtr device,
	const void* pData,
	VkDeviceSize size,
	VkExtent3D extent,
	uint32_t mipLevels,
	VkImageType type,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkSharingMode sharingMode,
	VkImageLayout defaultLayout,
	VkImageCreateFlags flags,
	uint32_t arrayLayers)
{
	auto stagingBufferManagedPtr = device->GetStagingBufferAllocator()->Allocate(size, device->GetMemoryRequirements_StagingBuffer().alignment);
	(*stagingBufferManagedPtr).m_buffer->GetMemoryDevice()->Copy((**stagingBufferManagedPtr).m_offset, size, pData);

	VulkanImagePtr outImage = new VulkanImage(device);

	outImage->m_extent = extent;
	outImage->m_imageType = type;
	outImage->m_format = format;
	outImage->m_tiling = tiling;
	outImage->m_usage = usage;
	outImage->m_mipLevels = mipLevels;
	outImage->m_samples = VK_SAMPLE_COUNT_1_BIT;
	outImage->m_arrayLayers = arrayLayers;
	outImage->m_sharingMode = sharingMode;
	outImage->m_defaultLayout = defaultLayout;
	outImage->m_initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	outImage->m_flags = flags;

	outImage->Compile();

	auto requirements = outImage->GetMemoryRequirements();

	// We must respect bufferImageGranuality
	//requirements.size += device->GetBufferImageGranuality();
	//requirements.alignment = std::max(requirements.alignment, device->GetBufferImageGranuality());

	auto& imageMemoryAllocator = device->GetMemoryAllocator((VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), requirements);
	auto data = imageMemoryAllocator.Allocate(requirements.size, requirements.alignment);

	outImage->Bind(data);

	cmdBuffer->ImageMemoryBarrier(outImage, outImage->m_format, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	cmdBuffer->CopyBufferToImage((*stagingBufferManagedPtr).m_buffer->GetBufferMemoryPtr(),
		outImage,
		static_cast<uint32_t>(extent.width),
		static_cast<uint32_t>(extent.height),
		static_cast<uint32_t>(extent.depth),
		(*stagingBufferManagedPtr).m_offset);

	cmdBuffer->AddDependency(stagingBufferManagedPtr, device->GetStagingBufferAllocator());

	if (outImage->m_mipLevels == 1)
	{
		cmdBuffer->ImageMemoryBarrier(outImage, outImage->m_format, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, defaultLayout);
	}
	else
	{
		cmdBuffer->GenerateMipMaps(outImage);
	}

	return outImage;
}

VulkanImagePtr VulkanApi::CreateImage(
	VulkanDevicePtr device,
	VkExtent3D extent,
	uint32_t mipLevels,
	VkImageType type,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkSharingMode sharingMode,
	VkSampleCountFlagBits sampleCount,
	VkImageLayout defaultLayout,
	VkImageCreateFlags flags,
	uint32_t arrayLayers)
{
	VulkanImagePtr outImage = new VulkanImage(device);

	outImage->m_extent = extent;
	outImage->m_imageType = type;
	outImage->m_format = format;
	outImage->m_tiling = tiling;
	outImage->m_usage = usage;
	outImage->m_mipLevels = mipLevels;
	outImage->m_samples = sampleCount;
	outImage->m_arrayLayers = arrayLayers;
	outImage->m_sharingMode = sharingMode;
	outImage->m_defaultLayout = defaultLayout;
	outImage->m_initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
	outImage->m_flags = flags;

	outImage->Compile();

	auto requirements = outImage->GetMemoryRequirements();

	// We must respect bufferImageGranuality
	//requirements.size += device->GetBufferImageGranuality();
	//requirements.alignment = std::max(requirements.alignment, device->GetBufferImageGranuality());

	auto& imageMemoryAllocator = device->GetMemoryAllocator((VkMemoryPropertyFlags)(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT), requirements);
	auto data = imageMemoryAllocator.Allocate(requirements.size, requirements.alignment);

	outImage->Bind(data);

	return outImage;
}

VulkanImagePtr VulkanApi::CreateImage_Immediate(
        VulkanDevicePtr device,
        const void* pData,
        VkDeviceSize size,
        VkExtent3D extent,
	uint32_t mipLevels,
	VkImageType type,
	VkFormat format,
	VkImageTiling tiling,
	VkImageUsageFlags usage,
	VkSharingMode sharingMode,
	VkImageLayout defaultLayout,
	VkImageCreateFlags flags,
	uint32_t arrayLayers)
{

	auto cmdBuffer = device->CreateCommandBuffer();
	device->SetDebugName(VkObjectType::VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)(VkCommandBuffer)*cmdBuffer, "VulkanApi::CreateImage_Immediate");
	cmdBuffer->BeginCommandList(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	VulkanImagePtr res = CreateImageUpload(cmdBuffer, device, pData, size, extent, mipLevels, type, format, tiling, usage, sharingMode, defaultLayout, flags, arrayLayers);
	cmdBuffer->EndCommandList();

	auto fence = VulkanFencePtr::Make(device);
	device->SubmitCommandBuffer(cmdBuffer, fence);
        fence->Wait();

        return res;
}

#ifdef _WIN32
void* VulkanApi::ExportImage(VulkanDevicePtr device, VulkanImagePtr image, VkExternalMemoryHandleTypeFlagBits handleType)
{
        auto vkGetMemoryWin32HandleKHR = (PFN_vkGetMemoryWin32HandleKHR)vkGetDeviceProcAddr(*device, "vkGetMemoryWin32HandleKHR");
        if (!vkGetMemoryWin32HandleKHR)
        {
                return nullptr;
        }

        VkMemoryGetWin32HandleInfoKHR handleInfo{};
        handleInfo.sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR;
        handleInfo.handleType = handleType;
        handleInfo.memory = *image->GetMemoryDevice();

        HANDLE handle = nullptr;
        //if (vkGetMemoryWin32HandleKHR(*device, &handleInfo, &handle) != VK_SUCCESS)
        //{
        //        return nullptr;
        //}

        return handle;
}
#else
void* VulkanApi::ExportImage(VulkanDevicePtr /*device*/, VulkanImagePtr /*image*/, VkExternalMemoryHandleTypeFlagBits /*handleType*/)
{
    return nullptr;
}
#endif

#ifdef _WIN32
VulkanImagePtr VulkanApi::ImportImage(VulkanDevicePtr device,
        void* handle,
        VkExtent3D extent,
        VkFormat format,
        VkImageUsageFlags usage,
        VkImageLayout defaultLayout,
        VkImageCreateFlags flags,
        uint32_t arrayLayers,
        VkSampleCountFlagBits sampleCount)
{
        VulkanImagePtr outImage = new VulkanImage(device);
        outImage->EnableExternalMemory(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT);
        outImage->m_extent = extent;
        outImage->m_imageType = VK_IMAGE_TYPE_2D;
        outImage->m_format = format;
        outImage->m_tiling = VK_IMAGE_TILING_OPTIMAL;
        outImage->m_usage = usage;
        outImage->m_mipLevels = 1;
        outImage->m_samples = sampleCount;
        outImage->m_arrayLayers = arrayLayers;
        outImage->m_sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        outImage->m_defaultLayout = defaultLayout;
        outImage->m_initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        outImage->m_flags = flags;

        outImage->Compile();

        VkMemoryRequirements requirements = outImage->GetMemoryRequirements();

		VkImportMemoryWin32HandleInfoKHR importInfo{};
        importInfo.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR;
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        importInfo.handle = (HANDLE)handle;

        auto memory = VulkanDeviceMemoryPtr::Make(device, requirements,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &importInfo);

        outImage->Bind(memory, 0);

        return outImage;
}
#else
VulkanImagePtr VulkanApi::ImportImage(VulkanDevicePtr /*device*/, void* /*handle*/, VkExtent3D /*extent*/, VkFormat /*format*/, VkImageUsageFlags /*usage*/, VkImageLayout /*defaultLayout*/, VkImageCreateFlags /*flags*/, uint32_t /*arrayLayers*/, VkSampleCountFlagBits /*sampleCount*/)
{
        return nullptr;
}
#endif

VkDescriptorSetLayoutBinding VulkanApi::CreateDescriptorSetLayoutBinding(uint32_t binding, VkDescriptorType descriptorType, uint32_t descriptorCount, VkShaderStageFlags stageFlags, const VkSampler* pImmutableSamplers)
{
	VkDescriptorSetLayoutBinding layoutBinding{};
	layoutBinding.binding = binding;
	layoutBinding.descriptorType = descriptorType;
	layoutBinding.descriptorCount = descriptorCount;
	layoutBinding.stageFlags = stageFlags;
	layoutBinding.pImmutableSamplers = pImmutableSamplers;

	return layoutBinding;
}

VkDescriptorPoolSize VulkanApi::CreateDescriptorPoolSize(VkDescriptorType type, uint32_t count)
{
	VkDescriptorPoolSize poolSize{};
	poolSize.type = type;
	poolSize.descriptorCount = static_cast<uint32_t>(count);
	return poolSize;
}

bool VulkanApi::IsCompatible(const VulkanPipelineLayoutPtr& pipelineLayout, const VulkanDescriptorSetPtr& descriptorSet, uint32_t binding)
{
	SAILOR_PROFILE_FUNCTION();

	if (!descriptorSet)
	{
		return false;
	}

	if (pipelineLayout->m_descriptionSetLayouts.Num() <= binding)
	{
		return false;
	}

	const VulkanDescriptorSetLayoutPtr& layout = pipelineLayout->m_descriptionSetLayouts[binding];


	size_t numDescriptorsInLayout = 0;
	for (const auto& layoutBinding : layout->m_descriptorSetLayoutBindings)
	{
		if (!descriptorSet->LikelyContains(layoutBinding) || numDescriptorsInLayout > descriptorSet->m_descriptors.Num())
		{
			return false;
		}

		numDescriptorsInLayout += layoutBinding.descriptorCount;

		// Heavy and slow, TODO: should we more carefully check the compatibility of descriptor sets and layouts?
		/*if (!descriptorSet->m_descriptors.ContainsIf([&](const auto& lhs) { return lhs->GetBinding() == layoutBinding.binding && lhs->GetType() == layoutBinding.descriptorType; }))
		{
			return false;
		}*/
	}
	return descriptorSet->m_descriptors.Num() == numDescriptorsInLayout;
}

TVector<bool> VulkanApi::IsCompatible(const VulkanPipelineLayoutPtr& pipelineLayout, const TVector<VulkanDescriptorSetPtr>& descriptorSets)
{
	SAILOR_PROFILE_FUNCTION();
	TVector<bool> res(descriptorSets.Num());

	const uint32_t numSets = (uint32_t)std::min(descriptorSets.Num(), pipelineLayout->m_descriptionSetLayouts.Num());
	for (uint32_t i = 0; i < numSets; i++)
	{
		res[i] = IsCompatible(pipelineLayout, descriptorSets[i], i);
	}

	return res;
}

bool VulkanApi::CreateDescriptorSetLayouts(VulkanDevicePtr device,
	const TVector<VulkanShaderStagePtr>& shaders,
	TVector<VulkanDescriptorSetLayoutPtr>& outVulkanLayouts,
	TVector<RHI::ShaderLayoutBinding>& outRhiLayout)
{
	SAILOR_PROFILE_FUNCTION();

	TVector<TVector<VkDescriptorSetLayoutBinding>> vulkanLayouts;
	TVector<TVector<RHI::ShaderLayoutBinding>> rhiLayouts;

	TSet<uint32_t> uniqueDescriptorSets;
	size_t countDescriptorSets = 0;

	for (const auto& shader : shaders)
	{
		for (const auto& binding : shader->GetBindings())
		{
			uniqueDescriptorSets.Insert(binding[0].m_set);
			countDescriptorSets = std::max(countDescriptorSets, (size_t)binding[0].m_set + 1);
		}
	}

	vulkanLayouts.Resize(countDescriptorSets);
	rhiLayouts.Resize(countDescriptorSets);

	for (uint32_t i = 0; i < shaders.Num(); i++)
	{
		for (uint32_t j = 0; j < shaders[i]->GetDescriptorSetLayoutBindings().Num(); j++)
		{
			const auto& rhiBindings = shaders[i]->GetBindings()[j];
			const auto& bindings = shaders[i]->GetDescriptorSetLayoutBindings()[j];

			for (uint32_t k = 0; k < bindings.Num(); k++)
			{
				const auto& rhiBinding = rhiBindings[k];
				const auto& binding = bindings[k];

				// Handle duplicated bindings
				auto& binds = vulkanLayouts[rhiBinding.m_set];
				if (binds.FindIf([&rhiBinding](auto& b) { return b.binding == rhiBinding.m_binding; }) == -1)
				{
					vulkanLayouts[rhiBinding.m_set].Add(binding);
					rhiLayouts[rhiBinding.m_set].Add(rhiBinding);
				}
			}
		}
	}

	outVulkanLayouts.Clear();
	outRhiLayout.Clear();

	outVulkanLayouts.Resize(vulkanLayouts.Num());
	outRhiLayout.Reserve(rhiLayouts.Num());

	for (uint32_t i = 0; i < vulkanLayouts.Num(); i++)
	{
		outVulkanLayouts[i] = VulkanDescriptorSetLayoutPtr::Make(device, std::move(vulkanLayouts[i]));
	}

	for (uint32_t i = 0; i < rhiLayouts.Num(); i++)
	{
		for (auto& rhi : rhiLayouts[i])
		{
			outRhiLayout.Emplace(std::move(rhi));
		}
	}

	return countDescriptorSets > 0;
}

bool operator==(const VkDescriptorSetLayoutBinding& lhs, const VkDescriptorSetLayoutBinding& rhs)
{
	return	lhs.binding == rhs.binding &&
		lhs.descriptorType == rhs.descriptorType &&
		lhs.descriptorCount == rhs.descriptorCount &&
		lhs.stageFlags == rhs.stageFlags &&
		lhs.pImmutableSamplers == rhs.pImmutableSamplers;
}
