#include "Shader.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "Memory/WeakPtr.hpp"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

bool ShaderBinding::IsBind() const
{
#if defined(VULKAN)
	return (bool)(m_textureBinding) || (bool)(m_vulkan.m_valueBinding);
#endif

	return false;
}

bool ShaderBinding::FindVariableInUniformBuffer(const std::string& variable, ShaderLayoutBindingMember& outVariable) const
{
	auto it = std::find_if(m_bindingLayout.m_members.begin(), m_bindingLayout.m_members.end(), [&variable](const RHI::ShaderLayoutBindingMember& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == variable;
		});

	if (it != m_bindingLayout.m_members.end())
	{
		outVariable = (*it);
		return true;
	}

	return false;
}

ShaderBinding::~ShaderBinding()
{
#if defined(VULKAN)
	if (m_vulkan.m_valueBinding)
	{
		if (m_vulkan.m_bufferAllocator)
		{
			auto allocator = m_vulkan.m_bufferAllocator.Lock();
			allocator->Free(m_vulkan.m_valueBinding);
		}
	}
#endif
}