#include "Core/YamlUtils.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace Sailor;

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	void TestSingleDocumentLoading()
	{
		YAML::Node document;
		std::string diagnostic;
		Require(
			Utils::TryLoadSingleYamlDocument("name: Sailor\nvalue: 42\n", document, diagnostic),
			"one YAML document should load: " + diagnostic);
		Require(document.IsMap() && document["name"].as<std::string>() == "Sailor",
			"single-document loading should return the parsed document");

		Require(
			!Utils::TryLoadSingleYamlDocument({}, document, diagnostic) &&
				diagnostic.find("found 0") != std::string::npos && !document.IsDefined(),
			"an empty payload should be rejected as zero documents");
		Require(
			!Utils::TryLoadSingleYamlDocument("---\nvalue: 1\n---\nvalue: 2\n", document, diagnostic) &&
				diagnostic.find("found 2") != std::string::npos && !document.IsDefined(),
			"a multi-document payload should be rejected without exposing a document");
		Require(
			!Utils::TryLoadSingleYamlDocument("value: [1\n", document, diagnostic) &&
				!diagnostic.empty() && !document.IsDefined(),
			"malformed YAML should report the parser failure without exposing a document");
	}

	void TestMapStructureValidation()
	{
		YAML::Node map(YAML::NodeType::Map);
		map.force_insert("name", "first");
		map.force_insert("name", "second");
		const Utils::YamlMapValidationResult duplicate = Utils::ValidateYamlMap(map);
		Require(
			duplicate.m_error == Utils::EYamlMapValidationError::DuplicateKey &&
				duplicate.m_fieldName == "name",
			"map validation should identify duplicate scalar keys");

		YAML::Node first;
		Require(Utils::CountYamlMapField(map, "name", &first) == 2u &&
			first.as<std::string>() == "first",
			"field counting should preserve duplicate detection and the first-match lookup contract");
		Require(Utils::FindYamlMapField(map, "name").as<std::string>() == "first",
			"field lookup should match yaml-cpp's first-match map indexing behavior");

		YAML::Node nonScalarKey(YAML::NodeType::Sequence);
		nonScalarKey.push_back("key");
		YAML::Node nonScalarMap(YAML::NodeType::Map);
		nonScalarMap.force_insert(nonScalarKey, "value");
		Require(
			Utils::ValidateYamlMap(nonScalarMap).m_error ==
				Utils::EYamlMapValidationError::NonScalarKey,
			"map validation should reject non-scalar keys");

		YAML::Node emptyKeyMap(YAML::NodeType::Map);
		emptyKeyMap.force_insert("", "value");
		Require(
			Utils::ValidateYamlMap(emptyKeyMap).m_error ==
				Utils::EYamlMapValidationError::EmptyKey,
			"map validation should reject empty field names");
		Require(
			Utils::ValidateYamlMap(YAML::Node(YAML::NodeType::Sequence)).m_error ==
				Utils::EYamlMapValidationError::ExpectedMap,
			"map validation should reject non-map nodes");
	}

	void TestExactFieldValidation()
	{
		const TVector<std::string> required{ "name", "value" };
		const TVector<std::string> optional{ "enabled" };

		YAML::Node valid(YAML::NodeType::Map);
		valid["name"] = "Sailor";
		valid["value"] = 42;
		valid["enabled"] = true;
		Require(Utils::ValidateYamlMapFields(valid, required, optional).IsValid(),
			"required and optional fields should validate");

		YAML::Node unknown = YAML::Clone(valid);
		unknown["unexpected"] = 1;
		const Utils::YamlMapValidationResult unknownResult =
			Utils::ValidateYamlMapFields(unknown, required, optional);
		Require(
			unknownResult.m_error == Utils::EYamlMapValidationError::UnknownField &&
				unknownResult.m_fieldName == "unexpected",
			"exact field validation should identify unknown fields");

		YAML::Node missing = YAML::Clone(valid);
		missing.remove("value");
		const Utils::YamlMapValidationResult missingResult =
			Utils::ValidateYamlMapFields(missing, required, optional);
		Require(
			missingResult.m_error == Utils::EYamlMapValidationError::MissingField &&
				missingResult.m_fieldName == "value",
			"exact field validation should identify missing required fields");
	}

	void TestScalarDecoding()
	{
		uint32_t value = 0u;
		std::string diagnostic;
		Require(Utils::TryDecodeYamlScalar(YAML::Node("42"), value, diagnostic) && value == 42u,
			"typed scalar decoding should convert valid scalar values");

		YAML::Node map(YAML::NodeType::Map);
		map["value"] = 42;
		Require(!Utils::TryDecodeYamlScalar(map, value, diagnostic) &&
			diagnostic.find("scalar") != std::string::npos,
			"typed scalar decoding should reject structured nodes");
		Require(!Utils::TryDecodeYamlScalar(YAML::Node("not-an-integer"), value, diagnostic) &&
			!diagnostic.empty(),
			"typed scalar decoding should report conversion failures");
	}
}

int main()
{
	try
	{
		TestSingleDocumentLoading();
		TestMapStructureValidation();
		TestExactFieldValidation();
		TestScalarDecoding();
		std::cout << "[PASS] YAML utility contracts" << std::endl;
		return 0;
	}
	catch (const std::exception& exception)
	{
		std::cerr << "[FAIL] YAML utility contracts: " << exception.what() << std::endl;
		return 1;
	}
}
