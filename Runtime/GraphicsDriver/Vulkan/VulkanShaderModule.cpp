#include "VulkanShaderModule.h"
#include "RHI/Types.h"
#include <spirv_reflect.h>
#include <memory>

using namespace Sailor;
using namespace Sailor::GraphicsDriver::Vulkan;

VulkanShaderStage::VulkanShaderStage(VkShaderStageFlagBits stage, StringHash entryPointName, VulkanShaderModulePtr shaderModule) :
	m_stage(stage),
	m_module(shaderModule),
	m_entryPointName(entryPointName)
{
}

VulkanShaderStage::VulkanShaderStage(VkShaderStageFlagBits stage, StringHash entryPointName, VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv, const RHI::ShaderStageLayout* layout) :
	m_stage(stage),
	m_entryPointName(entryPointName)
{
	m_module = VulkanShaderModulePtr::Make(pDevice, spirv);
	if (layout)
	{
		ApplyShaderLayout(*layout);
		m_bHasCachedLayout = true;
	}
}

bool VulkanShaderStage::IsRuntimeArrayBinding(const SpvReflectDescriptorBinding& reflBinding)
{
	if (reflBinding.count == 0)
	{
		return true;
	}

	if (reflBinding.array.dims_count > 0 &&
		reflBinding.array.dims[reflBinding.array.dims_count - 1] == SPV_REFLECT_ARRAY_DIM_RUNTIME)
	{
		return true;
	}

	if (reflBinding.type_description &&
		reflBinding.type_description->traits.array.dims_count > 0 &&
		reflBinding.type_description->traits.array.dims[reflBinding.type_description->traits.array.dims_count - 1] == SPV_REFLECT_ARRAY_DIM_RUNTIME)
	{
		return true;
	}

	return false;
}

void VulkanShaderStage::Apply(VkPipelineShaderStageCreateInfo& stageInfo) const
{
	stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.stage = m_stage;
	stageInfo.module = *m_module;
	stageInfo.pName = m_entryPointName.ToString().c_str();
}

void VulkanShaderStage::Compile()
{
	if ((VkShaderModule)*m_module == nullptr)
	{
		m_module->Compile();
		if (!m_bHasCachedLayout)
		{
			ReflectDescriptorSetBindings(m_module->m_byteCode);
		}
	}
}

void VulkanShaderStage::ApplyShaderLayout(const RHI::ShaderStageLayout& layout)
{
	m_bindings = layout.m_bindings;
	m_vertexAttributeBindings = layout.m_vertexAttributeBindings;
	m_fragmentOutputMask = layout.m_fragmentOutputMask;
	m_layoutBindings.Clear();
	m_layoutBindings.Resize(m_bindings.Num());
	for (size_t index = 0; index < m_bindings.Num(); ++index)
	{
		for (const auto& binding : m_bindings[index])
		{
			auto value = VulkanApi::CreateDescriptorSetLayoutBinding(binding.m_binding,
				static_cast<VkDescriptorType>(binding.m_type), binding.m_arrayCount);
			value.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT;
			m_layoutBindings[index].Add(value);
		}
	}
	m_pushConstants.Clear();
	for (const auto& range : layout.m_pushConstants)
	{
		m_pushConstants.Add({ static_cast<VkShaderStageFlags>(m_stage), range.m_offset, range.m_size });
	}
}

void VulkanShaderStage::ReflectDescriptorSetBindings(const RHI::ShaderByteCode& code)
{
	RHI::ShaderStageLayout layout;
	std::string diagnostic;
	if (ReflectShaderLayout(static_cast<RHI::EShaderStage>(m_stage), code, layout, diagnostic))
	{
		ApplyShaderLayout(layout);
	}
	else
	{
		SAILOR_LOG_ERROR("%s", diagnostic.c_str());
	}
}

bool VulkanShaderStage::ReflectShaderLayout(RHI::EShaderStage stage, const RHI::ShaderByteCode& code,
	RHI::ShaderStageLayout& outLayout, std::string& outDiagnostic)
{
	SAILOR_PROFILE_FUNCTION();

	outLayout = {};
	outDiagnostic.clear();
	if (code.IsEmpty())
	{
		outDiagnostic = "Cannot reflect an empty shader.";
		return false;
	}
	SpvReflectShaderModule module{};
	SpvReflectResult result = spvReflectCreateShaderModule(code.Num() * sizeof(code[0]), &code[0], &module);

	if (result != SPV_REFLECT_RESULT_SUCCESS)
	{
		outDiagnostic = "Cannot reflect SPIR-V shader layout.";
		return false;
	}
	std::unique_ptr<SpvReflectShaderModule, decltype(&spvReflectDestroyShaderModule)>
		owner(&module, &spvReflectDestroyShaderModule);
	if (module.entry_point_count != 1 || module.shader_stage != static_cast<SpvReflectShaderStageFlagBits>(stage))
	{
		outDiagnostic = "Shader reflection entry point does not match the requested stage.";
		return false;
	}
	outLayout.m_stage = stage;

	const uint32_t numBoundVertexAttributes = module.entry_points[0].input_variable_count;
	outLayout.m_fragmentOutputMask = 0u;
	if (stage == RHI::EShaderStage::Fragment)
	{
		for (uint32_t i = 0u; i < module.entry_points[0].output_variable_count; ++i)
		{
			const auto location = module.entry_points[0].output_variables[i]->location;
			if (location < 32u)
			{
				outLayout.m_fragmentOutputMask |= 1u << location;
			}
		}
	}
	for (uint32_t i = 0; i < numBoundVertexAttributes; i++)
	{
		outLayout.m_vertexAttributeBindings.Insert(module.entry_points[0].input_variables[i]->location);
	}

	uint32_t count = 0;
	result = spvReflectEnumerateDescriptorSets(&module, &count, NULL);
	if (result != SPV_REFLECT_RESULT_SUCCESS)
	{
		outDiagnostic = "Cannot enumerate shader descriptor sets.";
		return false;
	}

	TVector<TMap<uint32_t, RHI::ShaderLayoutBinding>> bindings;
	bindings.Resize(count);

	TVector<SpvReflectDescriptorSet*> sets(count);
	result = spvReflectEnumerateDescriptorSets(&module, &count, sets.GetData());
	if (result != SPV_REFLECT_RESULT_SUCCESS)
	{
		outDiagnostic = "Cannot enumerate shader descriptor sets.";
		return false;
	}

	for (size_t i_set = 0; i_set < sets.Num(); ++i_set)
	{
		const SpvReflectDescriptorSet& reflSet = *(sets[i_set]);
		auto& binding = bindings[i_set];

		for (uint32_t i_binding = 0; i_binding < reflSet.binding_count; ++i_binding)
		{
			const SpvReflectDescriptorBinding& reflBinding = *(reflSet.bindings[i_binding]);
			if (reflBinding.binding > 255 || static_cast<uint32_t>(reflBinding.descriptor_type) > 10)
			{
				outDiagnostic = "Shader descriptor binding exceeds the engine layout range.";
				return false;
			}

			RHI::ShaderLayoutBinding& rhiBinding = binding[reflBinding.binding] = {};
			const bool bVariableDescriptorCount = VulkanShaderStage::IsRuntimeArrayBinding(reflBinding);
			const uint32_t descriptorCount = std::max(1u, reflBinding.count);

			// Fill data
			rhiBinding.m_name = StringHash::Runtime(reflBinding.name ? reflBinding.name : "");
			rhiBinding.m_type = (RHI::EShaderBindingType)reflBinding.descriptor_type;
			rhiBinding.m_binding = reflBinding.binding;
			rhiBinding.m_size = reflBinding.block.size;
			rhiBinding.m_set = reflBinding.set;
			rhiBinding.m_arrayCount = descriptorCount;
			rhiBinding.m_paddedSize = reflBinding.block.padded_size;
			rhiBinding.m_bVariableDescriptorCount = bVariableDescriptorCount;
			rhiBinding.m_textureType = (RHI::ETextureType)reflBinding.image.dim;

			uint32_t membersSize = 0;

			// We handle UBO as POD
			SpvReflectBlockVariable* blockContent = reflBinding.block.members;
			uint32_t blockCount = reflBinding.block.member_count;

			// We handle SSBO as instance's POD
			if (rhiBinding.m_type == RHI::EShaderBindingType::StorageBuffer && reflBinding.block.member_count != 0)
			{
				blockContent = reflBinding.block.members[0].members;
				blockCount = reflBinding.block.members[0].member_count;
			}

			for (uint32_t i = 0; i < blockCount; i++)
			{
				RHI::ShaderLayoutBindingMember member;

				member.m_name = StringHash::Runtime(blockContent[i].name ? blockContent[i].name : "");
				member.m_absoluteOffset = blockContent[i].offset;
				member.m_size = blockContent[i].size;
				member.m_type = (RHI::EShaderBindingMemberType)(blockContent[i].type_description->op);
				member.m_arrayDimensions = blockContent[i].array.dims_count;
				// TODO: implement multiple arrays
				member.m_arrayCount = blockContent[i].array.dims[0];
				member.m_arrayStride = blockContent[i].array.stride;

				membersSize = std::max(
					membersSize,
					blockContent[i].offset + blockContent[i].size);
				if (member.m_type == RHI::EShaderBindingMemberType::Array && blockContent[i].type_description)
				{
					const auto& typeFlags = blockContent[i].type_description->type_flags;
					if (typeFlags & SPV_REFLECT_TYPE_FLAG_FLOAT)
					{
						member.m_type = RHI::EShaderBindingMemberType::Float;
					}
					else if (typeFlags & SPV_REFLECT_TYPE_FLAG_BOOL)
					{
						member.m_type = RHI::EShaderBindingMemberType::Bool;
					}
					else if (typeFlags & SPV_REFLECT_TYPE_FLAG_INT)
					{
						member.m_type = RHI::EShaderBindingMemberType::Int;
					}

					if (typeFlags & SPV_REFLECT_TYPE_FLAG_MATRIX)
					{
						member.m_type = RHI::EShaderBindingMemberType::Matrix;
					}
					else if (typeFlags & SPV_REFLECT_TYPE_FLAG_VECTOR)
					{
						member.m_type = RHI::EShaderBindingMemberType::Vector;
					}
				}

				rhiBinding.m_members.Emplace(std::move(member));
			}

			if (rhiBinding.m_type == RHI::EShaderBindingType::StorageBuffer && reflBinding.block.member_count != 0)
			{
				//We store 1 instance size in binding
				rhiBinding.m_size = membersSize;
				const SpvReflectBlockVariable& reflectedArray =
					reflBinding.block.members[0];
				rhiBinding.m_paddedSize =
					SsboLayout::ResolveSsboArrayStride(reflectedArray);
			}
		}
	}

	outLayout.m_pushConstants.Resize(module.push_constant_block_count);
	for (uint32_t i = 0; i < module.push_constant_block_count; ++i)
	{
		const auto& block = module.push_constant_blocks[i];
		// SPIRV-Reflect reports the absolute end, including any leading offset.
		outLayout.m_pushConstants[i] = { block.offset, block.size - block.offset };
	}

	outLayout.m_bindings.Clear();
	outLayout.m_bindings.Resize(count);

	for (uint32_t i = 0; i < count; i++)
	{
		for (const auto& binding : bindings[i])
		{
			outLayout.m_bindings[i].Add(*binding.m_second);
		}
	}
	outLayout.m_bValid = true;
	return outLayout.Validate(outDiagnostic);
}

VulkanShaderModule::VulkanShaderModule(VulkanDevicePtr pDevice, const RHI::ShaderByteCode& spirv) : m_byteCode(spirv), m_pDevice(pDevice) {}

VulkanShaderModule::~VulkanShaderModule()
{
	VulkanShaderModule::Release();
}

void VulkanShaderModule::Compile()
{
	if (m_shaderModule)
	{
		return;
	}

	VkShaderModuleCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	createInfo.codeSize = m_byteCode.Num() * 4;
	createInfo.pCode = reinterpret_cast<const uint32_t*>(m_byteCode.GetData());

	VK_CHECK(vkCreateShaderModule(*m_pDevice, &createInfo, nullptr, &m_shaderModule));
}

void VulkanShaderModule::Release()
{
	vkDestroyShaderModule(*m_pDevice, m_shaderModule, nullptr);
	m_shaderModule = nullptr;
}
