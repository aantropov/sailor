#include "Shader.h"
#include "Types.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

bool ShaderBinding::IsBind() const
{
#if defined(VULKAN)
	return (bool)(m_textureBinding) || (bool)(m_vulkan.m_valueBinding);
#endif

	return false;
}

bool ShaderBinding::FindVariableInUniformBuffer(const std::string& variable, ShaderLayoutBindingMember& outVariable) const
{
	auto it = std::find_if(m_membersInfo.m_members.begin(), m_membersInfo.m_members.end(), [&variable](const RHI::ShaderLayoutBindingMember& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == variable;
		});

	if (it != m_membersInfo.m_members.end())
	{
		outVariable = (*it);
		return true;
	}

	return false;
}
