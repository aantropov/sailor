#include "GfxDeviceVulkan.h"
#include <vulkan/vulkan.h>
#include "LogMacros.h"
#include <assert.h>
#include <iostream>
#include <vector>

#include "Sailor.h"

using namespace Sailor;

#define VK_CHECK(call) \
	do { \
		VkResult result_ = call; \
		assert(result_ == VK_SUCCESS); \
	} while (0)

#define NUM_ELEMENTS(array) (sizeof(array) / sizeof(array[0]))

GfxDeviceVulkan* GfxDeviceVulkan::instance = nullptr;
VkInstance GfxDeviceVulkan::vkInstance = 0;

static VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDebugCallback(
	VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
	VkDebugUtilsMessageTypeFlagsEXT messageType,
	const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
	void* pUserData)
{
	std::cerr << "Validation layer: " << pCallbackData->pMessage <<
		std::endl;
	return VK_FALSE;

}

void GfxDeviceVulkan::CreateInstance(const Window* viewport)
{
	if (instance != nullptr)
	{
		SAILOR_LOG("Vulkan already initialized!");
		return;
	}

	SAILOR_LOG("Num supported Vulkan extensions: %d", GfxDeviceVulkan::GetNumSupportedExtensions());
	PrintSupportedExtensions();

	VkApplicationInfo appInfo{ VK_STRUCTURE_TYPE_APPLICATION_INFO };

	appInfo.pApplicationName = EngineInstance::ApplicationName.c_str();
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "Sailor";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_2;

	const char* extensions[] =
	{
		VK_KHR_SURFACE_EXTENSION_NAME,
		"VK_KHR_win32_surface",
		VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#ifdef VK_USE_PLATFORM_WIN32_KHR
		VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
#ifdef _DEBUG
		VK_EXT_DEBUG_REPORT_EXTENSION_NAME,
#endif
	};

	VkInstanceCreateInfo createInfo{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
	createInfo.pApplicationInfo = &appInfo;
	createInfo.ppEnabledExtensionNames = extensions;
	createInfo.enabledExtensionCount = NUM_ELEMENTS(extensions);

#ifdef _DEBUG
	const std::vector<const char*> debugLayers =
	{
		"VK_LAYER_KHRONOS_validation"
	};

	createInfo.ppEnabledLayerNames = debugLayers.data();
	createInfo.enabledLayerCount = debugLayers.size();

	if (!CheckValidationLayerSupport(debugLayers))
	{
		SAILOR_LOG("Not all debug layers are supported");
	}

#endif

	vkInstance = 0;
	VK_CHECK(vkCreateInstance(&createInfo, 0, &vkInstance));
	
	SetupDebugCallback();
	SAILOR_LOG("Vulkan initialized");
	
	// 
	//vkCreateWin32SurfaceKHR
}

uint32_t GfxDeviceVulkan::GetNumSupportedExtensions()
{
	uint32_t extensionCount;
	VK_CHECK(vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount,
		nullptr));
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

void GfxDeviceVulkan::SetupDebugCallback()
{
	VkDebugUtilsMessengerCreateInfoEXT createInfo{ VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
	createInfo.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
		| VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	createInfo.pfnUserCallback = VulkanDebugCallback;
	createInfo.pUserData = nullptr; // Optional

	//TODO: Get proc addresses and init debug callbacks properly
	//VK_CHECK(vkCreateDebugUtilsMessengerEXT(vkInstance, &createInfo, nullptr, nullptr));
}

void GfxDeviceVulkan::Shutdown()
{
	vkDestroyInstance(vkInstance, nullptr);
}
