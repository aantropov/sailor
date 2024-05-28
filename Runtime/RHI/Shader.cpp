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
	return m_textureBinding.Num() > 0 || (bool)(m_vulkan.m_valueBinding);
#endif

	return false;
}

size_t RHIShaderBinding::GetCompatibilityHash() const
{
	static std::hash<RHIResourcePtr> p;
	static std::hash<size_t> pSize;

	size_t hash = 0;
	if (m_textureBinding.Num() > 0)
	{
		// For arrays of textures we handle only the first texture and the size of array
		// TODO: Should we handle 4-12 textures instead? 
		// That should cover all VFX programmer's needs
		HashCombine(hash, p(m_textureBinding[0]), pSize(m_textureBinding.Num()));

		/*for (const auto& binding : m_textureBinding)
		{
			HashCombine(hash, p(binding));
		}*/
	}
	else if (m_vulkan.m_valueBinding)
	{
		if ((m_vulkan.m_valueBinding->Get().m_ptr.m_buffer->m_usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) && !m_vulkan.m_bBindSsboWithOffset)
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
