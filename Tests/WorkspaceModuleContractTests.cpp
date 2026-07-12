#include "Components/Component.h"
#include "Core/Reflection.h"
#include "Memory/ObjectAllocator.hpp"
#include "Workspace/WorkspaceModuleApi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
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

	struct WorkspaceDescriptorCapture
	{
		WorkspaceTypeDescriptorV1 m_descriptor{};
		size_t m_numDescriptors = 0;
	};

	struct FactoryInvocationGate
	{
		std::mutex m_mutex;
		std::condition_variable m_conditionVariable;
		bool m_bEntered = false;
		bool m_bRelease = false;
	};

	uint32_t SAILOR_WORKSPACE_CALL CaptureWorkspaceDescriptor(
		void* context,
		const WorkspaceTypeDescriptorV1* descriptor) noexcept
	{
		auto* capture = static_cast<WorkspaceDescriptorCapture*>(context);
		if (capture == nullptr || descriptor == nullptr ||
			descriptor->structSize < sizeof(WorkspaceTypeDescriptorV1) ||
			capture->m_numDescriptors != 0)
		{
			return static_cast<uint32_t>(EWorkspaceModuleResult::RegistrationFailed);
		}

		capture->m_descriptor = *descriptor;
		++capture->m_numDescriptors;
		return static_cast<uint32_t>(EWorkspaceModuleResult::Success);
	}

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

	const YAML::Node FindEnum(const YAML::Node& enums, const std::string& enumName)
	{
		for (const YAML::Node& reflectedEnum : enums)
		{
			if (reflectedEnum[enumName])
			{
				return reflectedEnum[enumName];
			}
		}

		return YAML::Node(YAML::NodeType::Undefined);
	}

	bool ContainsScalar(const YAML::Node& values, const std::string& expected)
	{
		return std::any_of(values.begin(), values.end(), [&](const YAML::Node& value)
			{
				return value.IsScalar() && value.as<std::string>() == expected;
			});
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
		Require(
			type["properties"]["registryLookupSucceeded"].as<std::string>() == "bool",
			"workspace component metadata should preserve reflected bool properties");
		Require(!type["properties"]["readOnlyValue"].IsDefined(),
			"getter-only properties should remain outside the writable property schema");
		Require(type["readOnlyProperties"].IsSequence() &&
			ContainsScalar(type["readOnlyProperties"], "readOnlyValue") &&
			ContainsScalar(type["readOnlyProperties"], "skippedReadOnlyValue"),
			"workspace metadata should explicitly identify serialized read-only properties");
		Require(type["properties"]["skippedDefault"].as<std::string>() == "float",
			"SkipCDO properties should remain available in the writable property schema");
		Require(type["properties"]["mode"].as<std::string>().rfind("enum ", 0) == 0,
			"workspace component metadata should preserve reflected enum properties");
		const YAML::Node fixtureModeValues = FindEnum(
			metadata["enums"],
			type["properties"]["mode"].as<std::string>());
		Require(fixtureModeValues.IsSequence() && fixtureModeValues.size() == 2,
			"workspace metadata should export values for reflected enum properties");
		Require(fixtureModeValues[0].as<std::string>() == "Default" &&
			fixtureModeValues[1].as<std::string>() == "Alternate",
			"workspace metadata should preserve reflected enum value names");
		const YAML::Node mobilityValues = FindEnum(
			metadata["enums"],
			type["properties"]["mobility"].as<std::string>());
		Require(mobilityValues.IsSequence() && mobilityValues.size() > 0,
			"workspace metadata should describe referenced engine enum values");
		Require(type["properties"]["offset"].IsScalar(),
			"workspace component metadata should preserve custom structured property types");
		Require(type["properties"]["nullableComponent"].IsScalar(),
			"workspace component metadata should preserve object-reference property types");

		const YAML::Node defaultObject = FindType(metadata["cdos"], FixtureTypeName);
		Require(defaultObject.IsDefined(), "workspace metadata should include fixture defaults");
		Require(
			defaultObject["defaultValues"]["moveSpeed"].as<float>() == 5.0f,
			"workspace metadata should preserve reflected default values");
		Require(
			!defaultObject["defaultValues"]["registryLookupSucceeded"].as<bool>(),
			"metadata construction should observe the fixture before runtime registration");
		Require(defaultObject["defaultValues"]["fileId"].IsScalar(),
			"workspace defaults should include inherited Component fileId metadata");
		Require(defaultObject["defaultValues"]["instanceId"].IsScalar(),
			"workspace defaults should include inherited Component instanceId metadata");
		Require(defaultObject["defaultValues"]["readOnlyValue"].as<int32_t>() == 17,
			"workspace defaults should include getter-only reflected properties");
		Require(!defaultObject["defaultValues"]["skippedReadOnlyValue"].IsDefined(),
			"workspace defaults should omit getter-only properties marked SkipCDO");
		Require(!defaultObject["defaultValues"]["skippedDefault"].IsDefined(),
			"workspace defaults should omit properties marked SkipCDO");
		Require(defaultObject["defaultValues"]["mode"].as<std::string>() == "Default",
			"workspace defaults should preserve valid enum values");
		Require(defaultObject["defaultValues"]["offset"].IsSequence() &&
			defaultObject["defaultValues"]["offset"].size() == 3,
			"workspace defaults should preserve valid structured values");
		Require(defaultObject["defaultValues"]["nullableComponent"].IsNull(),
			"workspace defaults should preserve null object references without decoding them");
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

	void TestFactoryInvocationLease()
	{
		const WorkspaceModuleApiV1* moduleApi = SailorGetWorkspaceModuleApiV1();
		Require(moduleApi != nullptr && moduleApi->registerTypes != nullptr,
			"workspace fixture should expose its registration callback");

		WorkspaceDescriptorCapture capture;
		WorkspaceHostApiV1 hostApi{};
		hostApi.structSize = static_cast<uint32_t>(sizeof(WorkspaceHostApiV1));
		hostApi.apiVersion = WorkspaceHostApiVersion;
		hostApi.context = &capture;
		hostApi.collectType = &CaptureWorkspaceDescriptor;
		Require(
			static_cast<EWorkspaceModuleResult>(moduleApi->registerTypes(&hostApi)) ==
				EWorkspaceModuleResult::Success &&
				capture.m_numDescriptors == 1,
			"workspace fixture should expose exactly one registration descriptor");

		const auto* typeInfo = static_cast<const TypeInfo*>(capture.m_descriptor.typeInfo);
		Require(typeInfo != nullptr && typeInfo->Name() == FixtureTypeName,
			"workspace fixture descriptor should expose its reflected TypeInfo");
		Require(capture.m_descriptor.placementFactory != nullptr,
			"workspace fixture descriptor should expose its placement factory");
		Require(capture.m_descriptor.canonicalDefaultValues != nullptr &&
			capture.m_descriptor.canonicalDefaultValuesLength > 0,
			"workspace fixture descriptor should expose a canonical defaults snapshot");
		Require(capture.m_descriptor.flags == 0,
			"workspace fixture descriptor should not report reflected property ambiguity");
		const YAML::Node descriptorDefaults = YAML::Load(std::string(
			capture.m_descriptor.canonicalDefaultValues,
			static_cast<size_t>(capture.m_descriptor.canonicalDefaultValuesLength)));
		Require(descriptorDefaults["nullableComponent"].IsNull(),
			"canonical descriptor defaults should preserve null object references");

		const YAML::Node metadata = YAML::Load(ReadMetadata());
		const YAML::Node defaultObject = FindType(metadata["cdos"], FixtureTypeName);
		Require(defaultObject["defaultValues"].IsMap(),
			"workspace fixture should expose reflected defaults for lease validation");

		const auto gate = std::make_shared<FactoryInvocationGate>();
		const TWorkspacePlacementFactoryV1 placementFactory = capture.m_descriptor.placementFactory;
		Reflection::WorkspaceTypeRegistration registration;
		registration.m_typeInfo = typeInfo;
		registration.m_alignment = static_cast<size_t>(capture.m_descriptor.typeAlignment);
		registration.m_defaultObject = Reflection::CreateReflectedData(
			*typeInfo,
			defaultObject["defaultValues"]);
		registration.m_placementFactory = [gate, placementFactory](void* destination) -> IReflectable*
		{
			{
				std::unique_lock lock(gate->m_mutex);
				gate->m_bEntered = true;
				gate->m_conditionVariable.notify_all();
				gate->m_conditionVariable.wait(lock, [&gate]() { return gate->m_bRelease; });
			}

			if (placementFactory(destination) == nullptr)
			{
				return nullptr;
			}

			return static_cast<Component*>(destination);
		};

		constexpr const char* owner = "WorkspaceModuleContractTests.FactoryInvocationLease";
		std::vector<Reflection::WorkspaceTypeRegistration> registrations;
		registrations.emplace_back(std::move(registration));
		std::string registrationError;
		Require(Reflection::RegisterWorkspaceTypes(owner, std::move(registrations), registrationError),
			"workspace fixture should register for lease validation: " + registrationError);

		auto allocator = Memory::ObjectAllocatorPtr::Make(
			Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		auto firstConstruction = std::async(std::launch::async, [&]()
			{
				return Reflection::CreateObject<Component>(*typeInfo, allocator);
			});

		bool bFactoryEntered = false;
		{
			std::unique_lock lock(gate->m_mutex);
			bFactoryEntered = gate->m_conditionVariable.wait_for(
				lock,
				std::chrono::seconds(2),
				[&gate]() { return gate->m_bEntered; });
		}

		if (!bFactoryEntered)
		{
			{
				std::lock_guard lock(gate->m_mutex);
				gate->m_bRelease = true;
			}
			gate->m_conditionVariable.notify_all();
			ComponentPtr firstObject = firstConstruction.get();
			Reflection::UnregisterWorkspaceTypes(owner);
			if (firstObject)
			{
				firstObject.ForcelyDestroyObject();
			}
			Require(false, "workspace placement factory should begin within the test deadline");
		}

		auto unregister = std::async(std::launch::async, [&]()
			{
				return Reflection::UnregisterWorkspaceTypes(owner);
			});

		const auto removalDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (Reflection::IsWorkspaceTypeRegistered(FixtureTypeName) &&
			std::chrono::steady_clock::now() < removalDeadline)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}

		const bool bRegistrationRemoved = !Reflection::IsWorkspaceTypeRegistered(FixtureTypeName);
		ComponentPtr secondObject;
		if (bRegistrationRemoved)
		{
			secondObject = Reflection::CreateObject<Component>(*typeInfo, allocator);
		}
		const bool bUnregisterWaited = unregister.wait_for(std::chrono::milliseconds(100)) !=
			std::future_status::ready;

		{
			std::lock_guard lock(gate->m_mutex);
			gate->m_bRelease = true;
		}
		gate->m_conditionVariable.notify_all();

		ComponentPtr firstObject = firstConstruction.get();
		const size_t numRemoved = unregister.get();
		const bool bFirstObjectConstructed = static_cast<bool>(firstObject);
		const bool bSecondObjectRejected = !secondObject;
		if (firstObject)
		{
			firstObject.ForcelyDestroyObject();
		}
		if (secondObject)
		{
			secondObject.ForcelyDestroyObject();
		}

		Require(bRegistrationRemoved,
			"unregister should hide a workspace type while an active factory drains");
		Require(bUnregisterWaited,
			"unregister should wait until the active workspace factory returns");
		Require(bSecondObjectRejected,
			"new workspace construction should fail after unregister begins");
		Require(numRemoved == 1, "unregister should remove the fixture registration exactly once");
		Require(bFirstObjectConstructed,
			"the leased workspace factory should finish after its gate is released");
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

	void TestStablePropertyNames()
	{
		Require(TypeInfo::GetReflectedPropertyTypeName<uint32_t>() == "uint32",
			"workspace metadata should use a platform-independent uint32 property name");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "BufferContract", TestBufferContract },
		{ "MetadataSchemaAndDefaults", TestMetadataSchemaAndDefaults },
		{ "RepeatedAndConcurrentCalls", TestRepeatedAndConcurrentCalls },
		{ "FactoryInvocationLease", TestFactoryInvocationLease },
		{ "EngineRegistryRemainsEngineOnly", TestEngineRegistryRemainsEngineOnly },
		{ "StablePropertyNames", TestStablePropertyNames },
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
