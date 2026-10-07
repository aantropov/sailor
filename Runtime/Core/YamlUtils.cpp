#include "Core/YamlUtils.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

using namespace Sailor;
using namespace Sailor::Utils;

namespace
{
	constexpr size_t MaxCanonicalYamlDepth = 64;
	constexpr size_t MaxCanonicalYamlNodes = 262144;
	constexpr size_t MaxCanonicalYamlBytes = 64 * 1024 * 1024;

	bool AppendCanonicalText(
		std::string& destination,
		const std::string& value,
		size_t& remainingBytes)
	{
		if (value.size() > remainingBytes)
		{
			return false;
		}

		destination.append(value);
		remainingBytes -= value.size();
		return true;
	}

	bool AppendCanonicalCharacter(
		std::string& destination,
		char value,
		size_t& remainingBytes)
	{
		if (remainingBytes == 0)
		{
			return false;
		}

		destination.push_back(value);
		--remainingBytes;
		return true;
	}

	bool AppendCanonicalField(
		std::string& destination,
		const std::string& value,
		size_t& remainingBytes)
	{
		const std::string length = std::to_string(value.size());
		return AppendCanonicalText(destination, length, remainingBytes) &&
			AppendCanonicalCharacter(destination, ':', remainingBytes) &&
			AppendCanonicalText(destination, value, remainingBytes);
	}

	bool AppendCanonicalTag(
		const YAML::Node& node,
		std::string& destination,
		EYamlCanonicalizationMode mode,
		size_t& remainingBytes)
	{
		return mode == EYamlCanonicalizationMode::SemanticValue ||
			AppendCanonicalField(destination, node.Tag(), remainingBytes);
	}

	bool AppendCanonicalYaml(
		const YAML::Node& node,
		std::string& destination,
		EYamlCanonicalizationMode mode,
		size_t depth,
		size_t& remainingNodes,
		size_t& remainingBytes)
	{
		if ((mode == EYamlCanonicalizationMode::StrictDocument &&
				depth > MaxCanonicalYamlDepth) ||
			remainingNodes == 0)
		{
			return false;
		}
		--remainingNodes;

		if (!node.IsDefined())
		{
			return AppendCanonicalCharacter(destination, 'U', remainingBytes);
		}

		switch (node.Type())
		{
		case YAML::NodeType::Undefined:
			return AppendCanonicalCharacter(destination, 'U', remainingBytes);
		case YAML::NodeType::Null:
			return AppendCanonicalCharacter(destination, 'N', remainingBytes) &&
				AppendCanonicalTag(node, destination, mode, remainingBytes);
		case YAML::NodeType::Scalar:
			return AppendCanonicalCharacter(destination, 'S', remainingBytes) &&
				AppendCanonicalTag(node, destination, mode, remainingBytes) &&
				AppendCanonicalField(destination, node.Scalar(), remainingBytes);
		case YAML::NodeType::Sequence:
		{
			if (mode == EYamlCanonicalizationMode::StrictDocument &&
				node.size() > remainingNodes)
			{
				return false;
			}

			const std::string numElements = std::to_string(node.size());
			if (!AppendCanonicalCharacter(destination, 'Q', remainingBytes) ||
				!AppendCanonicalTag(node, destination, mode, remainingBytes) ||
				!AppendCanonicalText(destination, numElements, remainingBytes) ||
				!AppendCanonicalCharacter(destination, ':', remainingBytes))
			{
				return false;
			}
			for (const YAML::Node& element : node)
			{
				std::string canonicalElement;
				if (!AppendCanonicalYaml(
						element,
						canonicalElement,
						mode,
						depth + 1,
						remainingNodes,
						remainingBytes) ||
					!AppendCanonicalField(
						destination,
						canonicalElement,
						remainingBytes))
				{
					return false;
				}
			}
			return true;
		}
		case YAML::NodeType::Map:
		{
			if (mode == EYamlCanonicalizationMode::StrictDocument &&
				node.size() > remainingNodes / 2)
			{
				return false;
			}

			TVector<std::pair<std::string, std::string>> entries;
			entries.Reserve(node.size());
			for (const auto& entry : node)
			{
				std::string canonicalKey;
				std::string canonicalValue;
				if (!AppendCanonicalYaml(
						entry.first,
						canonicalKey,
						mode,
						depth + 1,
						remainingNodes,
						remainingBytes) ||
					!AppendCanonicalYaml(
						entry.second,
						canonicalValue,
						mode,
						depth + 1,
						remainingNodes,
						remainingBytes))
				{
					return false;
				}
				entries.Add(std::make_pair(
					std::move(canonicalKey),
					std::move(canonicalValue)));
			}

			std::sort(
				entries.begin(),
				entries.end(),
				[](const auto& lhs, const auto& rhs)
				{
					return lhs < rhs;
				});
			if (mode == EYamlCanonicalizationMode::StrictDocument)
			{
				for (size_t index = 1; index < entries.Num(); ++index)
				{
					if (entries[index - 1].first == entries[index].first)
					{
						return false;
					}
				}
			}

			const std::string numEntries = std::to_string(entries.Num());
			if (!AppendCanonicalCharacter(destination, 'M', remainingBytes) ||
				!AppendCanonicalTag(node, destination, mode, remainingBytes) ||
				!AppendCanonicalText(destination, numEntries, remainingBytes) ||
				!AppendCanonicalCharacter(destination, ':', remainingBytes))
			{
				return false;
			}
			for (const auto& entry : entries)
			{
				if (!AppendCanonicalField(
						destination,
						entry.first,
						remainingBytes) ||
					!AppendCanonicalField(
						destination,
						entry.second,
						remainingBytes))
				{
					return false;
				}
			}
			return true;
		}
		}

		return false;
	}
}

bool Utils::CanonicalizeYaml(
	const YAML::Node& node,
	std::string& destination,
	EYamlCanonicalizationMode mode)
{
	destination.clear();
	const bool bBounded =
		mode == EYamlCanonicalizationMode::StrictDocument;
	size_t remainingNodes = bBounded
		? MaxCanonicalYamlNodes
		: std::numeric_limits<size_t>::max();
	size_t remainingBytes = bBounded
		? MaxCanonicalYamlBytes
		: std::numeric_limits<size_t>::max();
	return AppendCanonicalYaml(
		node,
		destination,
		mode,
		0,
		remainingNodes,
		remainingBytes);
}

bool Utils::AreYamlNodesEqual(const YAML::Node& lhs, const YAML::Node& rhs)
{
	if (lhs.IsDefined() != rhs.IsDefined())
	{
		return false;
	}

	if (!lhs.IsDefined() || lhs.is(rhs))
	{
		return true;
	}

	std::string canonicalLhs;
	std::string canonicalRhs;
	return CanonicalizeYaml(
			lhs,
			canonicalLhs,
			EYamlCanonicalizationMode::SemanticValue) &&
		CanonicalizeYaml(
			rhs,
			canonicalRhs,
			EYamlCanonicalizationMode::SemanticValue) &&
		canonicalLhs == canonicalRhs;
}

bool Utils::TryLoadSingleYamlDocument(
	const std::string& payload,
	YAML::Node& outDocument,
	std::string& outDiagnostic) noexcept
{
	outDocument = YAML::Node(YAML::NodeType::Undefined);
	std::vector<YAML::Node> documents;
	if (!External::GuardYamlExceptions(
			[&]()
			{
				documents = YAML::LoadAll(payload);
			},
			outDiagnostic))
	{
		return false;
	}

	if (documents.size() != 1u)
	{
		outDiagnostic = "expected exactly one YAML document, found " +
			std::to_string(documents.size());
		return false;
	}

	outDocument = documents.front();
	outDiagnostic.clear();
	return true;
}

size_t Utils::CountYamlMapField(
	const YAML::Node& map,
	std::string_view fieldName,
	YAML::Node* outField)
{
	if (outField != nullptr)
	{
		*outField = YAML::Node(YAML::NodeType::Undefined);
	}

	if (!map.IsMap())
	{
		return 0u;
	}

	size_t count = 0u;
	for (const auto& field : map)
	{
		if (field.first.IsScalar() && field.first.Scalar() == fieldName)
		{
			if (outField != nullptr && count == 0u)
			{
				*outField = field.second;
			}
			++count;
		}
	}
	return count;
}

YAML::Node Utils::FindYamlMapField(
	const YAML::Node& map,
	std::string_view fieldName)
{
	YAML::Node field(YAML::NodeType::Undefined);
	CountYamlMapField(map, fieldName, &field);
	return field;
}

Utils::YamlMapValidationResult Utils::ValidateYamlMap(
	const YAML::Node& map)
{
	if (!map.IsMap())
	{
		return { EYamlMapValidationError::ExpectedMap, {} };
	}

	TSet<std::string_view> fields;
	for (const auto& field : map)
	{
		if (!field.first.IsScalar())
		{
			return { EYamlMapValidationError::NonScalarKey, {} };
		}

		const std::string_view name = field.first.Scalar();
		if (name.empty())
		{
			return { EYamlMapValidationError::EmptyKey, {} };
		}
		if (!fields.Insert(name))
		{
			return { EYamlMapValidationError::DuplicateKey, std::string(name) };
		}
	}

	return {};
}

Utils::YamlMapValidationResult Utils::ValidateYamlMapFields(
	const YAML::Node& map,
	const TVector<std::string_view>& requiredFields,
	const TVector<std::string_view>& optionalFields)
{
	const YamlMapValidationResult mapValidation = ValidateYamlMap(map);
	if (!mapValidation.IsValid())
	{
		return mapValidation;
	}

	TSet<std::string_view> expectedFields;
	for (const std::string_view field : requiredFields)
	{
		expectedFields.Insert(field);
	}
	for (const std::string_view field : optionalFields)
	{
		expectedFields.Insert(field);
	}

	TSet<std::string_view> actualFields;
	for (const auto& field : map)
	{
		const std::string_view name = field.first.Scalar();
		actualFields.Insert(name);
		if (!expectedFields.Contains(name))
		{
			return { EYamlMapValidationError::UnknownField, std::string(name) };
		}
	}

	for (const std::string_view field : requiredFields)
	{
		if (!actualFields.Contains(field))
		{
			return { EYamlMapValidationError::MissingField, std::string(field) };
		}
	}

	return {};
}
