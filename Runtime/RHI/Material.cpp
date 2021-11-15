#include "Material.h"
#include "Types.h"
#include "Shader.h"
#include "GfxDevice/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GfxDevice::Vulkan;

RHI::ShaderBindingPtr& Material::GetOrCreateShaderBinding(const std::string& parameter)
{
	auto it = m_shaderBindings.find(parameter);
	if (it != m_shaderBindings.end())
	{
		return (*it).second;
	}

	return m_shaderBindings[parameter] = ShaderBindingPtr::Make();
}

void Material::SetLayoutShaderBindings(std::vector<RHI::ShaderLayoutBinding> layoutBindings)
{
	m_layoutBindings = std::move(layoutBindings);
}