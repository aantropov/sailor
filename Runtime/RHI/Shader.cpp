#include "Shader.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "Memory/WeakPtr.hpp"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

bool RHIShaderBinding::IsBind() const
{
#if defined(SAILOR_BUILD_WITH_VULKAN)
	return (bool)(m_textureBinding) || (bool)(m_vulkan.m_valueBinding);
#endif

	return false;
}

size_t RHIShaderBinding::GetCompatibilityHash() const
{
	static std::hash<RHIResourcePtr> p;

	size_t hash = 0;
	if (m_textureBinding)
	{
		HashCombine(hash, p(m_textureBinding));
	}
	else if (m_vulkan.m_valueBinding)
	{
		if (m_vulkan.m_valueBinding->Get().m_ptr.m_buffer->m_usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)
		{
			HashCombine(hash, p(m_vulkan.m_valueBinding->Get().m_ptr.m_buffer));
		}
		else
		{
			HashCombine(hash, p(m_vulkan.m_valueBinding->Get().m_ptr.m_buffer), m_vulkan.m_valueBinding->Get().m_offset);
		}
	}

	return hash;
}

bool RHIShaderBinding::FindVariableInUniformBuffer(const std::string& variable, ShaderLayoutBindingMember& outVariable) const
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
