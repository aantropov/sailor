#include "Engine/InstanceId.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

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
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "GeneratedGameObjectIdsUseCanonicalFormat", TestGeneratedGameObjectIdsUseCanonicalFormat },
		{ "LegacyDirectGameObjectIdsRemainReadable", TestLegacyDirectGameObjectIdsRemainReadable },
		{ "MalformedDirectGameObjectIdsAreRejected", TestMalformedDirectGameObjectIdsAreRejected },
		{ "ComponentIdsResolveLegacyAndCanonicalParents", TestComponentIdsResolveLegacyAndCanonicalParents },
		{ "MalformedComponentIdsAreRejected", TestMalformedComponentIdsAreRejected },
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
