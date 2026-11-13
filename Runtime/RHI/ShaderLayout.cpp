#include "RHI/ShaderLayout.h"
#include "YamlExceptionBoundary.h"

#include <algorithm>
#include <limits>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	constexpr size_t MaxSets = 32;
	constexpr size_t MaxBindings = 1024;
	constexpr size_t MaxMembers = 4096;
	constexpr size_t MaxNamesLength = 1024;

	bool ReadName(const YAML::Node& node, StringHash& name)
	{
		if (!node.IsScalar())
		{
			return false;
		}
		const auto text = node.as<std::string>();
		if (text.size() > MaxNamesLength || text.find('\0') != text.npos)
		{
			return false;
		}
		name = StringHash::Runtime(text);
		return true;
	}
}

bool ShaderStageLayout::Validate(std::string& outDiagnostic) const
{
	outDiagnostic.clear();
	if (!m_bValid || (m_stage != EShaderStage::Vertex && m_stage != EShaderStage::Fragment &&
		m_stage != EShaderStage::Compute && m_stage != EShaderStage::Geometry) ||
		m_bindings.Num() > MaxSets || m_pushConstants.Num() > 128 || m_vertexAttributeBindings.Num() > 128)
	{
		outDiagnostic = "Invalid shader stage layout or reflection limits.";
		return false;
	}
	TSet<uint32_t> sets;
	for (const auto& set : m_bindings)
	{
		if (set.IsEmpty() || set.Num() > MaxBindings || sets.Contains(set[0].m_set))
		{
			outDiagnostic = "Invalid or duplicate shader descriptor set.";
			return false;
		}
		sets.Insert(set[0].m_set);
		TSet<uint32_t> bindings;
		for (const auto& binding : set)
		{
			if (binding.m_set != set[0].m_set || binding.m_set >= MaxSets ||
				static_cast<uint32_t>(binding.m_type) > 10 || binding.m_arrayCount == 0 ||
				binding.m_members.Num() > MaxMembers || binding.m_name.ToString().size() > MaxNamesLength ||
				bindings.Contains(binding.m_binding))
			{
				outDiagnostic = "Invalid or duplicate shader descriptor binding.";
				return false;
			}
			bindings.Insert(binding.m_binding);
			for (const auto& member : binding.m_members)
			{
				const auto type = static_cast<uint32_t>(member.m_type);
				if (type < 20 || type > 30 || member.m_name.ToString().size() > MaxNamesLength ||
					member.m_absoluteOffset > (std::numeric_limits<uint32_t>::max)() - member.m_size)
				{
					outDiagnostic = "Invalid shader buffer member.";
					return false;
				}
			}
		}
	}
	for (const auto& range : m_pushConstants)
	{
		if (range.m_size == 0 || range.m_offset % 4 != 0 || range.m_size % 4 != 0 ||
			range.m_offset > (std::numeric_limits<uint32_t>::max)() - range.m_size)
		{
			outDiagnostic = "Invalid shader push-constant range.";
			return false;
		}
	}
	return true;
}

YAML::Node ShaderStageLayout::Serialize() const
{
	if (!m_bValid)
	{
		return YAML::Node(YAML::NodeType::Null);
	}
	YAML::Node result(YAML::NodeType::Map);
	result["schemaVersion"] = 1;
	result["stage"] = static_cast<uint32_t>(m_stage);
	auto sets = result["bindings"] = YAML::Node(YAML::NodeType::Sequence);
	for (const auto& set : m_bindings)
	{
		YAML::Node bindings(YAML::NodeType::Sequence);
		for (const auto& binding : set)
		{
			YAML::Node value;
			value["type"] = static_cast<uint32_t>(binding.m_type);
			value["name"] = binding.m_name.ToString();
			value["binding"] = static_cast<uint32_t>(binding.m_binding);
			value["set"] = binding.m_set;
			value["size"] = binding.m_size;
			value["paddedSize"] = binding.m_paddedSize;
			value["arrayCount"] = binding.m_arrayCount;
			value["variableDescriptorCount"] = binding.m_bVariableDescriptorCount;
			value["textureType"] = static_cast<uint32_t>(binding.m_textureType);
			value["members"] = YAML::Node(YAML::NodeType::Sequence);
			for (const auto& member : binding.m_members)
			{
				YAML::Node field;
				field["name"] = member.m_name.ToString();
				field["type"] = static_cast<uint32_t>(member.m_type);
				field["offset"] = member.m_absoluteOffset;
				field["size"] = member.m_size;
				field["arrayCount"] = member.m_arrayCount;
				field["arrayDimensions"] = member.m_arrayDimensions;
				field["arrayStride"] = member.m_arrayStride;
				value["members"].push_back(field);
			}
			bindings.push_back(value);
		}
		sets.push_back(bindings);
	}
	auto ranges = result["pushConstants"] = YAML::Node(YAML::NodeType::Sequence);
	for (const auto& range : m_pushConstants)
	{
		YAML::Node value;
		value["offset"] = range.m_offset;
		value["size"] = range.m_size;
		ranges.push_back(value);
	}
	auto inputs = result["vertexAttributes"] = YAML::Node(YAML::NodeType::Sequence);
	TVector<uint32_t> sorted;
	for (const auto location : m_vertexAttributeBindings)
	{
		sorted.Add(location);
	}
	std::sort(sorted.begin(), sorted.end());
	for (const auto location : sorted)
	{
		inputs.push_back(location);
	}
	result["fragmentOutputMask"] = m_fragmentOutputMask;
	return result;
}

void ShaderStageLayout::Deserialize(const YAML::Node& data)
{
	*this = {};
	ShaderStageLayout candidate;
	std::string diagnostic;
	const bool parsed = External::GuardYamlExceptions([&]()
		{
			if (!data.IsMap() || data.size() != 6 || data["schemaVersion"].as<uint32_t>() != 1 ||
				!data["bindings"].IsSequence() || data["bindings"].size() > MaxSets ||
				!data["pushConstants"].IsSequence() || data["pushConstants"].size() > 128 ||
				!data["vertexAttributes"].IsSequence() || data["vertexAttributes"].size() > 128)
			{
				return;
			}
			const auto stage = data["stage"].as<uint32_t>();
			if (stage > 255)
			{
				return;
			}
			candidate.m_stage = static_cast<EShaderStage>(stage);
			for (const auto& set : data["bindings"])
			{
				if (!set.IsSequence() || set.size() > MaxBindings)
				{
					return;
				}
				TVector<ShaderLayoutBinding> bindings;
				for (const auto& value : set)
				{
					ShaderLayoutBinding binding;
					if (!value.IsMap() || value.size() != 10 || !ReadName(value["name"], binding.m_name) ||
						!value["members"].IsSequence() || value["members"].size() > MaxMembers ||
						value["binding"].as<uint32_t>() > 255 || value["type"].as<uint32_t>() > 10 ||
						value["textureType"].as<uint32_t>() > 255)
					{
						return;
					}
					binding.m_binding = value["binding"].as<uint8_t>();
					binding.m_type = static_cast<EShaderBindingType>(value["type"].as<uint32_t>());
					binding.m_textureType = static_cast<ETextureType>(value["textureType"].as<uint32_t>());
					binding.m_set = value["set"].as<uint32_t>();
					binding.m_size = value["size"].as<uint32_t>();
					binding.m_paddedSize = value["paddedSize"].as<uint32_t>();
					binding.m_arrayCount = value["arrayCount"].as<uint32_t>();
					binding.m_bVariableDescriptorCount = value["variableDescriptorCount"].as<bool>();
					for (const auto& field : value["members"])
					{
						ShaderLayoutBindingMember member;
						if (!field.IsMap() || field.size() != 7 || !ReadName(field["name"], member.m_name) ||
							field["type"].as<uint32_t>() < 20 || field["type"].as<uint32_t>() > 30)
						{
							return;
						}
						member.m_type = static_cast<EShaderBindingMemberType>(field["type"].as<uint32_t>());
						member.m_absoluteOffset = field["offset"].as<uint32_t>();
						member.m_size = field["size"].as<uint32_t>();
						member.m_arrayCount = field["arrayCount"].as<uint32_t>();
						member.m_arrayDimensions = field["arrayDimensions"].as<uint32_t>();
						member.m_arrayStride = field["arrayStride"].as<uint32_t>();
						binding.m_members.Add(std::move(member));
					}
					bindings.Add(std::move(binding));
				}
				candidate.m_bindings.Add(std::move(bindings));
			}
			for (const auto& value : data["pushConstants"])
			{
				if (!value.IsMap() || value.size() != 2)
				{
					return;
				}
				candidate.m_pushConstants.Add({ value["offset"].as<uint32_t>(), value["size"].as<uint32_t>() });
			}
			for (const auto& location : data["vertexAttributes"])
			{
				candidate.m_vertexAttributeBindings.Insert(location.as<uint32_t>());
			}
			candidate.m_fragmentOutputMask = data["fragmentOutputMask"].as<uint32_t>();
			candidate.m_bValid = true;
		}, diagnostic);
	if (parsed && candidate.Validate(diagnostic))
	{
		*this = std::move(candidate);
	}
}
