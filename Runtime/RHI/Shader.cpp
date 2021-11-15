#include "Shader.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

bool ShaderBinding::IsBind() const
{
#if defined(VULKAN)
	return (bool)(m_vulkan.m_textureBinding) || (bool)(m_vulkan.m_valueBinding);
#endif

	return false;
}