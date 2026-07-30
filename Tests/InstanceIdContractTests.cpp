#include "Engine/InstanceId.h"
#include "Core/Reflection.h"
#include "Core/Utils.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;

namespace Sailor
{
	struct ComponentIdentityReflectionFixture
	{
		InstanceId m_instanceId;
	};
}

REFL_AUTO(
	type(Sailor::ComponentIdentityReflectionFixture),
	field(m_instanceId)
)

namespace
{
	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	InstanceId Parse(const std::string& value)
	{
		InstanceId result;
		result.Deserialize(YAML::Node(value));
		return result;
	}

	bool IsHexString(const std::string& value)
	{
		return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char character)
			{
				return std::isxdigit(character) != 0;
			});
	}

	ReflectedData MakeComponentReflection(const YAML::Node& properties)
	{
		return Reflection::CreateReflectedData(
			TypeInfo::Get<ComponentIdentityReflectionFixture>(),
			properties);
	}

	void TestGeneratedGameObjectIdsUseCanonicalFormat()
	{
		for (uint32_t i = 0; i < 256; ++i)
		{
			const InstanceId id = InstanceId::GenerateNewInstanceId();
			Require(id.ToString().length() == 20, "generated game-object IDs must contain exactly 20 hex characters");
			Require(IsHexString(id.ToString()), "generated game-object IDs must contain only hex characters");
			Require(id.IsGameObjectId(), "generated game-object IDs must be recognized as game-object IDs");
			Require(id.GameObjectId() == id, "a generated direct ID must resolve to itself");
		}
	}

	void TestLegacyDirectGameObjectIdsRemainReadable()
	{
		const std::string legacyIds[] = {
			"0123456789ABCDEF",
			"0123456789ABCDEFF",
			"0123456789ABCDEFFF",
			"0123456789ABCDEFFFF",
			"0123456789ABCDEFFFFF"
		};

		for (const std::string& value : legacyIds)
		{
			const InstanceId id = Parse(value);
			Require(id.IsGameObjectId(), "legacy 16-20 character game-object IDs must remain valid");
			Require(id.GameObjectId() == id, "legacy direct IDs must resolve to themselves");
		}
	}

	void TestMalformedDirectGameObjectIdsAreRejected()
	{
		const std::string malformedIds[] = {
			"0123456789ABCDE",
			"0123456789ABCDEFFFFF0",
			"0123456789ABCDEG",
			"0123456789ABCDEF_0123456789ABCDEF"
		};

		for (const std::string& value : malformedIds)
		{
			const InstanceId id = Parse(value);
			Require(!id.IsGameObjectId(), "malformed direct IDs must not be classified as game-object IDs");
		}
	}

	void TestComponentIdsResolveLegacyAndCanonicalParents()
	{
		const InstanceId legacyParent = Parse("0123456789ABCDEF");
		const InstanceId canonicalParent = Parse("0123456789ABCDEFFFFF");
		const InstanceId componentParts[] = {
			Parse("FEDCBA9876543210"),
			Parse("FFF4417DA65649588B6B279D47D0EC3E")
		};

		for (const InstanceId& componentPart : componentParts)
		{
			for (const InstanceId& parent : { legacyParent, canonicalParent })
			{
				const InstanceId component(componentPart, parent);
				Require(!component.IsGameObjectId(), "a component ID must not be classified as a direct game-object ID");
				Require(component.ComponentId() == componentPart, "component IDs must preserve legacy 16- or 32-character prefixes");
				Require(component.GameObjectId() == parent, "component IDs must resolve their legacy or canonical parent suffix");
			}
		}

		const InstanceId malformedSuffix = Parse("FEDCBA9876543210_not-a-game-object");
		Require(malformedSuffix.GameObjectId() == InstanceId::Invalid, "malformed component parent suffixes must be rejected");
	}

	void TestMalformedComponentIdsAreRejected()
	{
		const std::string malformedIds[] = {
			"FEDCBA987654321_0123456789ABCDEF",
			"FEDCBA98765432100_0123456789ABCDEF",
			"FEDCBA987654321G_0123456789ABCDEF",
			"FFF4417DA65649588B6B279D47D0EC3_0123456789ABCDEF",
			"0FFF4417DA65649588B6B279D47D0EC3E_0123456789ABCDEF",
			"FFF4417DA65649588B6B279D47D0EC3G_0123456789ABCDEF",
			"FEDCBA9876543210_0123456789ABCDE",
			"FEDCBA9876543210_0123456789ABCDEG",
			"FEDCBA9876543210_0123456789ABCDEF_extra"
		};

		for (const std::string& value : malformedIds)
		{
			const InstanceId id = Parse(value);
			Require(id.ComponentId() == InstanceId::Invalid,
				"malformed component prefixes and separators must be rejected");
			Require(id.GameObjectId() == InstanceId::Invalid,
				"a malformed component ID must not expose an otherwise valid parent suffix");
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

		Require(Utils::AreYamlNodesEqual(lhs, reordered),
			"YAML map equality must ignore insertion order at every nesting level");
		Require(!Utils::AreYamlNodesEqual(lhs, reorderedSequence),
			"YAML sequence equality must preserve element order");
		Require(!Utils::AreYamlNodesEqual(lhs, changedValue),
			"YAML scalar changes must make structurally similar nodes unequal");

		const YAML::Node undefined(YAML::NodeType::Undefined);
		const YAML::Node anotherUndefined(YAML::NodeType::Undefined);
		const YAML::Node nullNode(YAML::NodeType::Null);
		Require(Utils::AreYamlNodesEqual(undefined, anotherUndefined),
			"two undefined YAML nodes must compare equal");
		Require(!Utils::AreYamlNodesEqual(undefined, nullNode),
			"undefined and explicit null YAML nodes must remain distinct");
	}

	void TestReflectedComponentIdentityUsesStrictSharedValidation()
	{
		const std::string validIdentity =
			"FFF4417DA65649588B6B279D47D0EC3E_0123456789ABCDEFFFFF";
		YAML::Node validProperties(YAML::NodeType::Map);
		validProperties["instanceId"] = validIdentity;

		InstanceId parsedIdentity = Parse("1111111111111111_10010010010010010000");
		std::string diagnostic = "stale diagnostic";
		Require(Utils::TryGetComponentInstanceId(
				MakeComponentReflection(validProperties),
				parsedIdentity,
				diagnostic),
			"a reflected component with valid component and owner IDs must be accepted");
		Require(parsedIdentity == Parse(validIdentity),
			"shared component identity parsing must preserve the complete instanceId");
		Require(diagnostic.empty(),
			"a successful component identity parse must clear stale diagnostics");

		const std::string malformedIdentities[] = {
			"0123456789ABCDEFFFFF",
			"FFF4417DA65649588B6B279D47D0EC3E_invalid-owner",
			"invalid-component_0123456789ABCDEFFFFF"
		};
		for (const std::string& malformedIdentity : malformedIdentities)
		{
			YAML::Node malformedProperties(YAML::NodeType::Map);
			malformedProperties["instanceId"] = malformedIdentity;
			parsedIdentity = Parse(validIdentity);
			diagnostic.clear();
			Require(!Utils::TryGetComponentInstanceId(
					MakeComponentReflection(malformedProperties),
					parsedIdentity,
					diagnostic),
				"a component identity must be rejected when either embedded ID is invalid");
			Require(parsedIdentity == InstanceId::Invalid,
				"a rejected reflected identity must not leak a partially valid InstanceId");
			Require(diagnostic.find("component and game-object IDs") != std::string::npos,
				"malformed embedded IDs must produce the shared structural diagnostic");
		}

		YAML::Node missingProperties(YAML::NodeType::Map);
		parsedIdentity = Parse(validIdentity);
		Require(!Utils::TryGetComponentInstanceId(
				MakeComponentReflection(missingProperties),
				parsedIdentity,
				diagnostic),
			"a reflected component without instanceId must be rejected");
		Require(diagnostic == "the reflected component has no instanceId",
			"a missing component identity must produce the shared diagnostic");
		Require(parsedIdentity == InstanceId::Invalid,
			"a missing reflected identity must clear the output InstanceId");

		YAML::Node invalidYamlProperties(YAML::NodeType::Map);
		invalidYamlProperties["instanceId"]["nested"] = true;
		parsedIdentity = Parse(validIdentity);
		Require(!Utils::TryGetComponentInstanceId(
				MakeComponentReflection(invalidYamlProperties),
				parsedIdentity,
				diagnostic),
			"a non-scalar component instanceId must be rejected through the YAML exception boundary");
		Require(diagnostic.find("the reflected component has an invalid instanceId") == 0,
			"a YAML conversion failure must produce the shared invalid-identity diagnostic");
		Require(parsedIdentity == InstanceId::Invalid,
			"a failed YAML conversion must clear the output InstanceId");

		ReflectedData invalidReflection;
		parsedIdentity = Parse(validIdentity);
		Require(!Utils::TryGetComponentInstanceId(
				invalidReflection,
				parsedIdentity,
				diagnostic),
			"an invalid reflected component must be rejected");
		Require(diagnostic == "the reflected component is invalid",
			"an invalid reflection must produce the shared diagnostic");
		Require(parsedIdentity == InstanceId::Invalid,
			"an invalid reflection must clear the output InstanceId");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "GeneratedGameObjectIdsUseCanonicalFormat", TestGeneratedGameObjectIdsUseCanonicalFormat },
		{ "LegacyDirectGameObjectIdsRemainReadable", TestLegacyDirectGameObjectIdsRemainReadable },
		{ "MalformedDirectGameObjectIdsAreRejected", TestMalformedDirectGameObjectIdsAreRejected },
		{ "ComponentIdsResolveLegacyAndCanonicalParents", TestComponentIdsResolveLegacyAndCanonicalParents },
		{ "MalformedComponentIdsAreRejected", TestMalformedComponentIdsAreRejected },
		{ "YamlNodeEqualityIsStructuralAndMapOrderIndependent", TestYamlNodeEqualityIsStructuralAndMapOrderIndependent },
		{ "ReflectedComponentIdentityUsesStrictSharedValidation", TestReflectedComponentIdentityUsesStrictSharedValidation },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
