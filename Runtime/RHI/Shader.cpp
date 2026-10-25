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
	size_t hash = 0;
	HashCombine(hash, m_bindingLayout.m_type, m_bindingLayout.m_binding,
		m_bindingLayout.m_set, m_bindingLayout.m_arrayCount);
	if (m_textureBinding.Num() > 0)
	{
		for (const auto& binding : m_textureBinding)
		{
			HashCombine(hash, binding);
		}
	}
	else if (m_vulkan.m_valueBinding)
	{
		const auto allocation = *m_vulkan.m_valueBinding->Get();
		HashCombine(hash, allocation.m_buffer);
		const bool withoutOffset = m_bindingLayout.m_type == EShaderBindingType::StorageBuffer &&
			!m_vulkan.m_bBindSsboWithOffset;
		HashCombine(hash, withoutOffset ? size_t(0u) : allocation.m_offset);
		if (m_bindingLayout.m_type != EShaderBindingType::StorageBuffer)
		{
			HashCombine(hash, allocation.m_size);
		}
	}

	return hash;
}

bool RHIShaderBinding::IsCompatibleWith(const RHIShaderBinding& rhs) const
{
	if (!(m_bindingLayout == rhs.m_bindingLayout) || m_textureBinding != rhs.m_textureBinding)
	{
		return false;
	}
	if (!m_textureBinding.IsEmpty())
	{
		return true;
	}
	if (!m_vulkan.m_valueBinding || !rhs.m_vulkan.m_valueBinding)
	{
		return m_vulkan.m_valueBinding == rhs.m_vulkan.m_valueBinding;
	}
	const auto lhsAllocation = *m_vulkan.m_valueBinding->Get();
	const auto rhsAllocation = *rhs.m_vulkan.m_valueBinding->Get();
	const bool storage = m_bindingLayout.m_type == EShaderBindingType::StorageBuffer;
	const size_t lhsOffset = storage && !m_vulkan.m_bBindSsboWithOffset ? 0u : lhsAllocation.m_offset;
	const size_t rhsOffset = storage && !rhs.m_vulkan.m_bBindSsboWithOffset ? 0u : rhsAllocation.m_offset;
	return lhsAllocation.m_buffer == rhsAllocation.m_buffer && lhsOffset == rhsOffset &&
		(storage || lhsAllocation.m_size == rhsAllocation.m_size);
}

bool RHIShaderBinding::FindVariableInUniformBuffer(StringHash variable, ShaderLayoutBindingMember& outVariable) const
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
