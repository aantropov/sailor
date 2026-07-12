#include "Core/Reflection.h"
#include "Workspace/WorkspaceModuleApi.h"

#include <algorithm>
#include <atomic>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Sailor;
using namespace Sailor::Workspace;

namespace
{
	constexpr const char* FixtureTypeName = "WorkspaceFixture::FixtureComponent";

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	EWorkspaceModuleResult ExportMetadata(char* destination, uint64_t capacity, uint64_t* payloadSize)
	{
		const TGetWorkspaceTypeMetadataV1 exportFunction = &SailorGetWorkspaceTypeMetadataV1;
		return static_cast<EWorkspaceModuleResult>(exportFunction(destination, capacity, payloadSize));
	}

	std::string ReadMetadata()
	{
		uint64_t payloadSize = 0;
		Require(
			ExportMetadata(nullptr, 0, &payloadSize) == EWorkspaceModuleResult::BufferTooSmall,
			"metadata size query should request a caller-owned buffer");
		Require(payloadSize > 0, "metadata size query should return a non-empty payload size");

		std::string payload(static_cast<size_t>(payloadSize), '\0');
		uint64_t writtenSize = 0;
		Require(
			ExportMetadata(payload.data(), payloadSize, &writtenSize) == EWorkspaceModuleResult::Success,
			"metadata export should fill an exact-size buffer");
		Require(writtenSize == payloadSize, "metadata export should preserve the queried payload size");
		return payload;
	}

	const YAML::Node FindType(const YAML::Node& types, const std::string& typeName)
	{
		for (const YAML::Node& type : types)
		{
			if (type["typename"] && type["typename"].as<std::string>() == typeName)
			{
				return type;
			}
		}

		return YAML::Node(YAML::NodeType::Undefined);
	}

	bool ContainsType(const YAML::Node& metadata, const std::string& typeName)
	{
		return FindType(metadata["engineTypes"], typeName).IsDefined();
	}

	void TestBufferContract()
	{
		Require(
			ExportMetadata(nullptr, 0, nullptr) == EWorkspaceModuleResult::InvalidArgument,
			"metadata export should reject a missing payload size pointer");

		uint64_t payloadSize = 0;
		Require(
			ExportMetadata(nullptr, 1, &payloadSize) == EWorkspaceModuleResult::InvalidArgument,
			"metadata export should reject a null destination with non-zero capacity");
		Require(payloadSize > 0, "invalid destination should still report the required payload size");

		std::vector<char> tooSmall(static_cast<size_t>(payloadSize - 1), '#');
		uint64_t requiredSize = 0;
		Require(
			ExportMetadata(tooSmall.data(), tooSmall.size(), &requiredSize) == EWorkspaceModuleResult::BufferTooSmall,
			"metadata export should reject an undersized destination");
		Require(requiredSize == payloadSize, "undersized export should report the full required size");
		Require(
			std::all_of(tooSmall.begin(), tooSmall.end(), [](char value) { return value == '#'; }),
			"metadata export should not partially write an undersized destination");

		const std::string payload = ReadMetadata();
		Require(payload.back() != '\0', "metadata payload should not include a null terminator");
	}

	void TestMetadataSchemaAndDefaults()
	{
		const YAML::Node metadata = YAML::Load(ReadMetadata());
		Require(metadata.IsMap(), "workspace metadata should be a YAML map");
		Require(
			metadata["metadataVersion"].as<uint32_t>() == WorkspaceTypeMetadataVersion,
			"workspace metadata should declare the V1 schema");
		Require(
			metadata["moduleName"].as<std::string>() == "WorkspaceFixture",
			"workspace metadata should identify its module");
		Require(metadata["timeStamp"].IsScalar(), "workspace metadata should contain a timestamp");
		Require(metadata["engineTypes"].IsSequence(), "workspace metadata types should be a sequence");
		Require(metadata["cdos"].IsSequence(), "workspace metadata defaults should be a sequence");
		Require(metadata["enums"].IsSequence(), "workspace metadata enums should be a sequence");
		Require(metadata["assetTypes"].IsSequence(), "workspace metadata asset types should be a sequence");

		const YAML::Node type = FindType(metadata["engineTypes"], FixtureTypeName);
		Require(type.IsDefined(), "workspace metadata should include the fixture component type");
		Require(
			type["base"].as<std::string>() == "Sailor::Component",
			"workspace component metadata should preserve the reflected base type");
		Require(
			type["properties"]["moveSpeed"].as<std::string>() == "float",
			"workspace component metadata should preserve reflected float properties");

		const YAML::Node defaultObject = FindType(metadata["cdos"], FixtureTypeName);
		Require(defaultObject.IsDefined(), "workspace metadata should include fixture defaults");
		Require(
			defaultObject["defaultValues"]["moveSpeed"].as<float>() == 5.0f,
			"workspace metadata should preserve reflected default values");
	}

	void TestRepeatedAndConcurrentCalls()
	{
		const std::string expected = ReadMetadata();
		std::atomic<bool> failed = false;
		std::vector<std::thread> threads;

		for (size_t threadIndex = 0; threadIndex < 8; ++threadIndex)
		{
			threads.emplace_back([&]()
				{
					for (size_t iteration = 0; iteration < 32; ++iteration)
					{
						try
						{
							if (ReadMetadata() != expected)
							{
								failed = true;
								return;
							}
						}
						catch (...)
						{
							failed = true;
							return;
						}
					}
				});
		}

		for (std::thread& thread : threads)
		{
			thread.join();
		}

		Require(!failed, "metadata export should be stable across repeated concurrent calls");
	}

	void TestEngineRegistryRemainsEngineOnly()
	{
		const YAML::Node before = Reflection::ExportEngineTypes();
		Require(!ContainsType(before, FixtureTypeName), "workspace types should not register during module attach");
		const size_t engineTypeCount = before["engineTypes"].size();

		ReadMetadata();

		const YAML::Node after = Reflection::ExportEngineTypes();
		Require(!ContainsType(after, FixtureTypeName), "workspace metadata export should not mutate the engine registry");
		Require(
			after["engineTypes"].size() == engineTypeCount,
			"workspace metadata export should preserve the engine registry contents");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "BufferContract", TestBufferContract },
		{ "MetadataSchemaAndDefaults", TestMetadataSchemaAndDefaults },
		{ "RepeatedAndConcurrentCalls", TestRepeatedAndConcurrentCalls },
		{ "EngineRegistryRemainsEngineOnly", TestEngineRegistryRemainsEngineOnly },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& e)
		{
			std::cerr << "[FAIL] " << test.first << ": " << e.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
