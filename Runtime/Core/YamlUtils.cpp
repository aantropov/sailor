#include "Core/YamlUtils.h"

#include <sstream>

#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/parser.h>

using namespace Sailor;

namespace
{
	class YamlDocumentCounter final : public YAML::EventHandler
	{
	public:
		void OnDocumentStart(const YAML::Mark&) override
		{
			++m_numDocuments;
		}

		void OnDocumentEnd() override {}
		void OnNull(const YAML::Mark&, YAML::anchor_t) override {}
		void OnAlias(const YAML::Mark&, YAML::anchor_t) override {}
		void OnScalar(
			const YAML::Mark&,
			const std::string&,
			YAML::anchor_t,
			const std::string&) override {}
		void OnSequenceStart(
			const YAML::Mark&,
			const std::string&,
			YAML::anchor_t,
			YAML::EmitterStyle::value) override {}
		void OnSequenceEnd() override {}
		void OnMapStart(
			const YAML::Mark&,
			const std::string&,
			YAML::anchor_t,
			YAML::EmitterStyle::value) override {}
		void OnMapEnd() override {}

		size_t GetNumDocuments() const noexcept
		{
			return m_numDocuments;
		}

	private:
		size_t m_numDocuments = 0u;
	};
}

bool Utils::TryLoadSingleYamlDocument(
	const std::string& payload,
	YAML::Node& outDocument,
	std::string& outDiagnostic) noexcept
{
	outDocument = YAML::Node(YAML::NodeType::Undefined);
	size_t documentCount = 0u;
	if (!External::GuardYamlExceptions(
			[&]()
			{
				std::istringstream input(payload);
				YAML::Parser parser(input);
				YamlDocumentCounter counter;
				while (parser.HandleNextDocument(counter)) {}
				documentCount = counter.GetNumDocuments();
				if (documentCount == 1u)
				{
					outDocument = YAML::Load(payload);
				}
			},
			outDiagnostic))
	{
		return false;
	}

	if (documentCount != 1u)
	{
		outDiagnostic = "expected exactly one YAML document, found " +
			std::to_string(documentCount);
		return false;
	}

	outDiagnostic.clear();
	return true;
}

size_t Utils::CountYamlMapField(
	const YAML::Node& map,
	const std::string& fieldName,
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
	const std::string& fieldName)
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

	TSet<std::string> fields;
	for (const auto& field : map)
	{
		if (!field.first.IsScalar())
		{
			return { EYamlMapValidationError::NonScalarKey, {} };
		}

		const std::string name = field.first.Scalar();
		if (name.empty())
		{
			return { EYamlMapValidationError::EmptyKey, name };
		}
		if (!fields.Insert(name))
		{
			return { EYamlMapValidationError::DuplicateKey, name };
		}
	}

	return {};
}

Utils::YamlMapValidationResult Utils::ValidateYamlMapFields(
	const YAML::Node& map,
	const TVector<std::string>& requiredFields,
	const TVector<std::string>& optionalFields)
{
	const YamlMapValidationResult mapValidation = ValidateYamlMap(map);
	if (!mapValidation.IsValid())
	{
		return mapValidation;
	}

	TSet<std::string> expectedFields;
	for (const std::string& field : requiredFields)
	{
		expectedFields.Insert(field);
	}
	for (const std::string& field : optionalFields)
	{
		expectedFields.Insert(field);
	}

	TSet<std::string> actualFields;
	for (const auto& field : map)
	{
		const std::string name = field.first.Scalar();
		actualFields.Insert(name);
		if (!expectedFields.Contains(name))
		{
			return { EYamlMapValidationError::UnknownField, name };
		}
	}

	for (const std::string& field : requiredFields)
	{
		if (!actualFields.Contains(field))
		{
			return { EYamlMapValidationError::MissingField, field };
		}
	}

	return {};
}
