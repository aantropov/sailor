#include "Material.h"
#include "Types.h"
#include "Shader.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

RHI::ShaderBindingPtr& ShaderBindingSet::GetOrCreateShaderBinding(const std::string& parameter)
{
	auto it = m_shaderBindings.find(parameter);
	if (it != m_shaderBindings.end())
	{
		return (*it).second;
	}

	return m_shaderBindings[parameter] = ShaderBindingPtr::Make();
}

void ShaderBindingSet::SetLayoutShaderBindings(std::vector<RHI::ShaderLayoutBinding> layoutBindings)
{
	m_layoutBindings = std::move(layoutBindings);
}

bool ShaderBindingSet::HasBinding(const std::string& binding) const
{
	auto it = std::find_if(m_layoutBindings.begin(), m_layoutBindings.end(), [&binding](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == binding;
		});

	return it != m_layoutBindings.end();
}

bool ShaderBindingSet::HasVariable(const std::string& parameter) const
{
	std::vector<std::string> splittedString = Utils::SplitString(parameter, ".");
	const std::string& binding = splittedString[0];
	const std::string& variable = splittedString[1];

	auto it = std::find_if(m_layoutBindings.begin(), m_layoutBindings.end(), [&binding](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == binding;
		});

	if (it != m_layoutBindings.end())
	{

		if (it->m_members.end() != std::find_if(it->m_members.begin(), it->m_members.end(), [&variable](const RHI::ShaderLayoutBindingMember& shaderLayoutBinding)
			{
				return shaderLayoutBinding.m_name == variable;
			}))
		{
			return true;
		}
	}

	return false;
}
