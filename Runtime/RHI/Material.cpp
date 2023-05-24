#include "Material.h"
#include "Types.h"
#include "Shader.h"
#include "RHI/Texture.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

GraphicsDriver::Vulkan::VulkanGraphicsPipelinePtr RHIMaterial::Vulkan::GetOrAddPipeline(const TVector<VkFormat>& colorAttachments, VkFormat depthStencilAttachment)
{
	SAILOR_PROFILE_FUNCTION();

	uint32_t stateIndex = 0;

	for (auto& p : m_pipelines)
	{
		stateIndex = 0;
		for (auto& state : p->m_pipelineStates)
		{
			if (const auto pDynamicState = state.DynamicCast<GraphicsDriver::Vulkan::VulkanStateDynamicRendering>())
			{
				if (pDynamicState->Equals(colorAttachments, depthStencilAttachment, depthStencilAttachment))
				{
					return p;
				}
				
				break;
			}

			stateIndex++;
		}
	}
	
	auto device = VulkanApi::GetInstance()->GetMainDevice();

	TVector<VulkanPipelineStatePtr> states = m_pipelines[0]->m_pipelineStates;

	const VkFormat stencilAttachmentFormat = (VulkanApi::ComputeAspectFlagsForFormat(depthStencilAttachment) & VK_IMAGE_ASPECT_STENCIL_BIT) ? depthStencilAttachment : VK_FORMAT_UNDEFINED;
	
	states[stateIndex] = GraphicsDriver::Vulkan::VulkanStateDynamicRenderingPtr::Make(colorAttachments, depthStencilAttachment, stencilAttachmentFormat);

	GraphicsDriver::Vulkan::VulkanGraphicsPipelinePtr pipeline = VulkanGraphicsPipelinePtr::Make(device,
		m_pipelines[0]->m_layout,
		m_pipelines[0]->m_stages,
		states,
		0);

	pipeline->m_renderPass = device->GetRenderPass();
	pipeline->Compile();

	m_pipelines.Emplace(std::move(pipeline));

	return *m_pipelines.Last();
}

void RHIShaderBindingSet::RecalculateCompatibility()
{
	SAILOR_PROFILE_FUNCTION();

	m_compatibilityHashCode = 0;

	for (auto& shaderBinding : m_shaderBindings)
	{
		HashCombine(m_compatibilityHashCode, shaderBinding.Second()->GetCompatibilityHash());
	}
}

RHI::RHIShaderBindingPtr& RHIShaderBindingSet::GetOrAddShaderBinding(const std::string& binding)
{
	auto& pBinding = m_shaderBindings.At_Lock(binding);
	if (!pBinding)
	{
		pBinding = RHIShaderBindingPtr::Make();
	}
	m_shaderBindings.Unlock(binding);

	return pBinding;
}

void RHIShaderBindingSet::RemoveShaderBinding(const std::string& binding)
{
	m_shaderBindings.Remove(binding);
}

void RHIShaderBindingSet::UpdateLayoutShaderBinding(const ShaderLayoutBinding& layout)
{
	// We are able to rewrite m_binding and m_set
	const size_t index = m_layoutBindings.FindIf([&](const auto& lhs)
		{
			return lhs.m_name == layout.m_name && lhs.m_type == layout.m_type && lhs.m_arrayCount == layout.m_arrayCount;
		});

	if (index != -1)
	{
		m_layoutBindings[index] = layout;
		return;
	}

	m_layoutBindings.Add(layout);
}

bool RHIShaderBindingSet::PerInstanceDataStoredInSsbo() const
{
	return std::find_if(m_layoutBindings.begin(), m_layoutBindings.end(), [](const auto& binding) { return binding.m_type == EShaderBindingType::StorageBuffer; }) != m_layoutBindings.end();
}

uint32_t RHIShaderBindingSet::GetStorageInstanceIndex(const std::string& bindingName) const
{
	if (m_shaderBindings.ContainsKey(bindingName))
	{
		return m_shaderBindings[bindingName]->GetStorageInstanceIndex();
	}

	return 0;
}

void RHIShaderBindingSet::SetLayoutShaderBindings(TVector<RHI::ShaderLayoutBinding> layoutBindings)
{
	m_layoutBindings = std::move(layoutBindings);
	m_bNeedsStorageBuffer = PerInstanceDataStoredInSsbo();
}

void RHIShaderBindingSet::ParseParameter(const std::string& parameter, std::string& outBinding, std::string& outVariable)
{
	TVector<std::string> splittedString = Utils::SplitString(parameter, ".");
	outBinding = splittedString[0];
	outVariable = splittedString[1];
}

bool RHIShaderBindingSet::HasBinding(const std::string& binding) const
{
	auto it = std::find_if(m_layoutBindings.begin(), m_layoutBindings.end(), [&binding](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == binding;
		});

	return it != m_layoutBindings.end();
}

bool RHIShaderBindingSet::HasParameter(const std::string& parameter) const
{
	TVector<std::string> splittedString = Utils::SplitString(parameter, ".");
	const std::string& binding = splittedString[0];
	const std::string& variable = splittedString[1];

	auto index = m_layoutBindings.FindIf([&binding](const RHI::ShaderLayoutBinding& shaderLayoutBinding)
		{
			return shaderLayoutBinding.m_name == binding;
		});

	if (index != -1)
	{
		if (m_layoutBindings[index].m_members.FindIf([&variable](const RHI::ShaderLayoutBindingMember& shaderLayoutBinding)
			{
				return shaderLayoutBinding.m_name == variable;
			}) != -1)
		{
			return true;
		}
	}

	return false;
}
