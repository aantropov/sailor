#include "Core/YamlUtils.h"
#include "Core/YamlSerializable.h"
#include "Core/JsonSerializable.h"

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace Sailor;

namespace
{
	enum class ETextMode { Default, Streamed };

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	void TestYamlNodeEqualityIsStructuralAndMapOrderIndependent()
	{
		const YAML::Node lhs = YAML::Load(
			"{ transform: { position: [1, 2, 3], enabled: true }, name: Duck }");
		const YAML::Node reordered = YAML::Load(
			"{ name: Duck, transform: { enabled: true, position: [1, 2, 3] } }");
		const YAML::Node reorderedSequence = YAML::Load(
			"{ name: Duck, transform: { enabled: true, position: [3, 2, 1] } }");
		const YAML::Node changedValue = YAML::Load(
			"{ name: Goose, transform: { enabled: true, position: [1, 2, 3] } }");
		YAML::Node programmatic;
		programmatic["name"] = "Duck";
		programmatic["transform"]["enabled"] = true;
		programmatic["transform"]["position"].push_back(1);
		programmatic["transform"]["position"].push_back(2);
		programmatic["transform"]["position"].push_back(3);

		Require(Utils::AreYamlNodesEqual(lhs, reordered),
			"YAML map equality must ignore insertion order at every nesting level");
		Require(Utils::AreYamlNodesEqual(lhs, programmatic),
			"semantic YAML equality must ignore parser-specific tags");
		Require(!Utils::AreYamlNodesEqual(lhs, reorderedSequence),
			"YAML sequence equality must preserve element order");
		Require(!Utils::AreYamlNodesEqual(lhs, changedValue),
			"YAML scalar changes must make structurally similar nodes unequal");

		std::string canonicalLhs;
		std::string canonicalReordered;
		Require(Utils::CanonicalizeYaml(
				lhs,
				canonicalLhs,
				Utils::EYamlCanonicalizationMode::SemanticValue),
			"semantic YAML canonicalization must accept ordinary maps");
		Require(Utils::CanonicalizeYaml(
				reordered,
				canonicalReordered,
				Utils::EYamlCanonicalizationMode::SemanticValue),
			"semantic YAML canonicalization must accept reordered maps");
		Require(canonicalLhs == canonicalReordered,
			"semantic YAML canonicalization must be map-order independent");

		std::string strictCanonicalLhs;
		std::string strictCanonicalReordered;
		Require(Utils::CanonicalizeYaml(
				lhs,
				strictCanonicalLhs,
				Utils::EYamlCanonicalizationMode::StrictDocument) &&
			Utils::CanonicalizeYaml(
				reordered,
				strictCanonicalReordered,
				Utils::EYamlCanonicalizationMode::StrictDocument) &&
			strictCanonicalLhs == strictCanonicalReordered,
			"strict YAML canonicalization must be map-order independent");

		YAML::Node duplicateKeys(YAML::NodeType::Map);
		duplicateKeys.force_insert("name", "Duck");
		duplicateKeys.force_insert("name", "Duck");
		std::string canonicalDuplicateKeys;
		Require(Utils::CanonicalizeYaml(
				duplicateKeys,
				canonicalDuplicateKeys,
				Utils::EYamlCanonicalizationMode::SemanticValue),
			"semantic YAML canonicalization must preserve duplicate pairs");
		Require(!Utils::CanonicalizeYaml(
				duplicateKeys,
				canonicalDuplicateKeys,
				Utils::EYamlCanonicalizationMode::StrictDocument),
			"strict YAML canonicalization must reject duplicate keys");

		YAML::Node nestedDuplicateKeys(YAML::NodeType::Map);
		nestedDuplicateKeys.force_insert("name", "Duck");
		nestedDuplicateKeys.force_insert("name", "Goose");
		YAML::Node nestedDuplicateDocument;
		nestedDuplicateDocument["object"] = nestedDuplicateKeys;
		Require(!Utils::CanonicalizeYaml(
				nestedDuplicateDocument,
				canonicalDuplicateKeys,
				Utils::EYamlCanonicalizationMode::StrictDocument),
			"strict YAML canonicalization must reject nested duplicate keys");

		YAML::Node taggedAsset("Duck");
		taggedAsset.SetTag("!asset");
		YAML::Node taggedModel("Duck");
		taggedModel.SetTag("!model");
		Require(Utils::AreYamlNodesEqual(taggedAsset, taggedModel),
			"semantic YAML equality must ignore explicit tags");
		std::string canonicalTaggedAsset;
		std::string canonicalTaggedModel;
		Require(Utils::CanonicalizeYaml(
				taggedAsset,
				canonicalTaggedAsset,
				Utils::EYamlCanonicalizationMode::StrictDocument) &&
			Utils::CanonicalizeYaml(
				taggedModel,
				canonicalTaggedModel,
				Utils::EYamlCanonicalizationMode::StrictDocument) &&
			canonicalTaggedAsset != canonicalTaggedModel,
			"strict YAML canonicalization must preserve explicit tags");

		std::string deepYaml;
		for (size_t depth = 0; depth < 66; ++depth)
		{
			deepYaml += "{ child: ";
		}
		deepYaml += "Duck";
		deepYaml.append(66, '}');
		const YAML::Node deepNode = YAML::Load(deepYaml);
		const YAML::Node clonedDeepNode = YAML::Clone(deepNode);
		Require(Utils::AreYamlNodesEqual(deepNode, clonedDeepNode),
			"semantic YAML equality must preserve the previous unbounded value semantics");
		Require(!Utils::CanonicalizeYaml(
				deepNode,
				canonicalLhs,
				Utils::EYamlCanonicalizationMode::StrictDocument),
			"strict YAML canonicalization must enforce its depth limit");

		const YAML::Node undefined(YAML::NodeType::Undefined);
		const YAML::Node anotherUndefined(YAML::NodeType::Undefined);
		const YAML::Node nullNode(YAML::NodeType::Null);
		const YAML::Node emptyMap = YAML::Load("{}");
		const YAML::Node anotherEmptyMap = YAML::Load("{}");
		const YAML::Node missing = emptyMap["missing"];
		const YAML::Node anotherMissing = anotherEmptyMap["missing"];
		Require(Utils::AreYamlNodesEqual(undefined, anotherUndefined),
			"two undefined YAML nodes must compare equal");
		Require(Utils::AreYamlNodesEqual(missing, anotherMissing),
			"invalid nodes from missing const lookups must compare as undefined");
		Require(!Utils::AreYamlNodesEqual(undefined, nullNode),
			"undefined and explicit null YAML nodes must remain distinct");
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
		Require(
			!Utils::TryLoadSingleYamlDocument("# no document\n\n", document, diagnostic) &&
				diagnostic.find("found 0") != std::string::npos && !document.IsDefined(),
			"comments alone should not become an implicit null document");
		Require(
			Utils::TryLoadSingleYamlDocument("---\n...\n", document, diagnostic) &&
				document.IsNull() && diagnostic.empty(),
			"an explicit null document should count as one document and clear old diagnostics");
		Require(
			Utils::TryLoadSingleYamlDocument("- one\n- two\n", document, diagnostic) &&
				document.IsSequence() && document.size() == 2,
			"single-document parsing should not require a map root");
		Require(
			Utils::TryLoadSingleYamlDocument("name: &name Sailor\ncopy: *name\n", document, diagnostic) &&
				document["copy"].as<std::string>() == "Sailor" && document["name"].is(document["copy"]),
			"single-document parsing should preserve YAML aliases");
		Require(
			!Utils::TryLoadSingleYamlDocument("---\nvalue: 1\n---\nvalue: [2\n", document, diagnostic) &&
				!diagnostic.empty() && !document.IsDefined(),
			"a malformed later document must not publish the valid first document");
		Require(
			!Utils::TryLoadSingleYamlDocument("---\n1\n---\n2\n---\n3\n", document, diagnostic) &&
				diagnostic.find("found 3") != std::string::npos && !document.IsDefined(),
			"document-count diagnostics should describe the complete stream");
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
		const char bounded[] = { 'n', 'a', 'm', 'e', 'x' };
		const std::string_view name(bounded, 4);
		Require(Utils::CountYamlMapField(map, name) == 2 && Utils::FindYamlMapField(map, name).is(first),
			"field lookup must respect a bounded, non-null-terminated name without copying it");
		Require(Utils::CountYamlMapField(map, {}) == 0 && !Utils::FindYamlMapField(map, {}).IsDefined(),
			"an empty field-name view must remain a valid missing-key query");

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
		const TVector<std::string_view> required{ "name", "value" };
		const TVector<std::string_view> optional{ "enabled" };

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
		const std::string longName(256, 'x');
		Utils::YamlMapValidationResult retained;
		{
			YAML::Node temporary;
			temporary[longName] = true;
			retained = Utils::ValidateYamlMapFields(temporary, {});
		}
		Require(retained.m_error == Utils::EYamlMapValidationError::UnknownField && retained.m_fieldName == longName,
			"validation results must own diagnostic names after their YAML document is destroyed");
		{
			std::string name = longName + ":ignored suffix";
			const TVector<std::string_view> fields{ std::string_view(name).substr(0, longName.size()) };
			YAML::Node document(YAML::NodeType::Map);
			retained = Utils::ValidateYamlMapFields(document, fields);
			document[longName] = true;
			Require(Utils::ValidateYamlMapFields(document, fields).IsValid(),
				"required fields must use the exact view length without reading its suffix");
		}
		Require(retained.m_error == Utils::EYamlMapValidationError::MissingField && retained.m_fieldName == longName,
			"missing-field diagnostics must own the borrowed field name after its source is destroyed");
	}

	void TestBorrowedSerializationNames()
	{
		YAML::Node document;
		std::string key = "a long borrowed field name:ignored suffix";
		const std::string_view name(key.data(), key.find(':'));
		Sailor::Serialize(document, name, 42);
		int value = 0;
		Require(Sailor::Deserialize(document, name, value) && value == 42 && document.size() == 1,
			"serialization must use only the bounded field name, without requiring a terminator");
		key.assign(1024, 'x');
		Require(document["a long borrowed field name"].as<int>() == 42,
			"the serialized document must own its key after the borrowed source is replaced");
		Require(!Sailor::Deserialize(document, "missing", value) && value == 42,
			"a missing field must leave the caller's value unchanged");

		FileRevision revision{ 123456789, true };
		const char revisionName[] = { 'r', 'e', 'v', 'x' };
		Sailor::Serialize(document, std::string_view(revisionName, 3), revision);
		FileRevision decoded;
		Require(Sailor::Deserialize(document, std::string_view(revisionName, 3), decoded) && decoded == revision,
			"the FileRevision overload must preserve the same bounded-name contract");

		Sailor::Serialize(document, "mode", ETextMode::Streamed);
		ETextMode mode = ETextMode::Default;
		Require(document["mode"].Scalar() == "Streamed" &&
			Sailor::Deserialize(document, "mode", mode) && mode == ETextMode::Streamed,
			"enum serialization must retain readable text and decode without owning a second copy");
		document["mode"] = "Unknown";
		Require(!Sailor::Deserialize(document, "mode", mode) && mode == ETextMode::Streamed,
			"an unknown enum name must not overwrite the previous value");

		json jsonMode;
		Sailor::SerializeEnum<ETextMode>(jsonMode, ETextMode::Default);
		Sailor::DeserializeEnum<ETextMode>(jsonMode, mode);
		Require(jsonMode == "Default" && mode == ETextMode::Default,
			"JSON enum readers must preserve the same textual contract");
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
		TestYamlNodeEqualityIsStructuralAndMapOrderIndependent();
		TestSingleDocumentLoading();
		TestMapStructureValidation();
		TestExactFieldValidation();
		TestBorrowedSerializationNames();
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
