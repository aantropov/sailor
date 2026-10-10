#include "Components/Component.h"
#include "Components/CameraComponent.h"
#include "Core/Reflection.h"
#include "Core/YamlUtils.h"
#include "Memory/ObjectAllocator.hpp"
#include "Memory/SharedPtr.hpp"
#include "Workspace/WorkspaceModuleApi.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace Sailor;
using namespace Sailor::Workspace;

extern "C" uint32_t SAILOR_WORKSPACE_CALL SailorWorkspaceFixtureConstructionCount() noexcept;

namespace RangeAnnotationFixture
{
	class BaseProperties
	{
	public:
		int32_t GetInheritedReadOnly() const { return 5; }
	};

	class NumericProperties : public BaseProperties
	{
	public:
		int32_t GetSignedValue() const { return m_signedValue; }
		void SetSignedValue(int32_t value) { m_signedValue = value; }
		uint32_t GetUnsignedValue() const { return m_unsignedValue; }
		void SetUnsignedValue(uint32_t value) { m_unsignedValue = value; }
		int32_t GetSkippedReadOnly() const { return 9; }
		int32_t GetTransientValue() const { return 11; }

	private:
		int32_t m_signedValue = 0;
		uint32_t m_unsignedValue = 0;
	};
}

namespace StableTypeNameFixture
{
	class ProjectAsset
	{
	};
}

REFL_AUTO(
	type(RangeAnnotationFixture::BaseProperties),
	func(GetInheritedReadOnly, property("inheritedReadOnly"))
)

REFL_AUTO(
	type(RangeAnnotationFixture::NumericProperties, bases<RangeAnnotationFixture::BaseProperties>),
	func(GetSignedValue, property("signedValue"), Sailor::Attributes::Range(-10.0, 10.0)),
	func(SetSignedValue, property("signedValue")),
	func(GetUnsignedValue, property("unsignedValue"), Sailor::Attributes::Range(0.0, 20.0)),
	func(SetUnsignedValue, property("unsignedValue")),
	func(GetSkippedReadOnly, property("skippedReadOnly"), Sailor::Attributes::SkipCDO()),
	func(GetTransientValue, property("transientValue"), Sailor::Attributes::Transient())
)

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

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	YAML::Node ReadMetadata()
	{
		WorkspaceDescriptorCapture capture;
		const WorkspaceHostApiV1 host{ sizeof(WorkspaceHostApiV1), WorkspaceHostApiVersion, &capture, &CaptureWorkspaceDescriptor };
		const auto* api = SailorGetWorkspaceModuleApiV1();
		Require(api->registerTypes(&host) == static_cast<uint32_t>(EWorkspaceModuleResult::Success),
			"module should enumerate its reflected types");
		const auto* type = static_cast<const TypeInfo*>(capture.m_descriptor.typeInfo);
		Require(type && type->GetDefaultValues(), "TypeInfo should capture the typed default object");
		YAML::Node metadata = Reflection::ExportTypes({ type });
		metadata["moduleName"] = std::string(api->moduleName, api->moduleNameLength);
		metadata["metadataVersion"] = WorkspaceTypeMetadataVersion;
		return metadata;
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

	void TestRegistrationContract()
	{
		const auto* api = SailorGetWorkspaceModuleApiV1();
		Require(api->registerTypes(nullptr) == static_cast<uint32_t>(EWorkspaceModuleResult::InvalidArgument),
			"registration should reject a missing host table");
		WorkspaceDescriptorCapture capture;
		WorkspaceHostApiV1 host{ sizeof(WorkspaceHostApiV1), WorkspaceHostApiVersion, &capture, &CaptureWorkspaceDescriptor };
		host.structSize = 0;
		Require(api->registerTypes(&host) == static_cast<uint32_t>(EWorkspaceModuleResult::InvalidArgument),
			"registration should reject an incomplete host table");
		host.structSize = sizeof(host);
		host.apiVersion = 0;
		Require(api->registerTypes(&host) == static_cast<uint32_t>(EWorkspaceModuleResult::InvalidArgument),
			"registration should reject an incompatible host protocol");
		host.apiVersion = WorkspaceHostApiVersion;
		host.collectType = nullptr;
		Require(api->registerTypes(&host) == static_cast<uint32_t>(EWorkspaceModuleResult::InvalidArgument),
			"registration should reject a missing collector");
	}

	void TestMetadataSchemaAndDefaults()
	{
		const YAML::Node metadata = ReadMetadata();
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
		Require(type["propertyRanges"].IsMap(),
			"workspace component metadata should provide a propertyRanges map");
		Require(
			type["propertyRanges"]["moveSpeed"]["min"].as<double>() == 0.0 &&
			type["propertyRanges"]["moveSpeed"]["max"].as<double>() == 10.0,
			"workspace component metadata should preserve reflected numeric ranges");
		Require(
			type["properties"]["registryLookupSucceeded"].as<std::string>() == "bool",
			"workspace component metadata should preserve reflected bool properties");
		Require(!type["properties"]["readOnlyValue"].IsDefined(),
			"getter-only properties should remain outside the writable property schema");
		Require(type["readOnlyProperties"].IsSequence() &&
			ContainsScalar(type["readOnlyProperties"], "readOnlyValue") &&
			ContainsScalar(type["readOnlyProperties"], "skippedReadOnlyValue"),
			"workspace metadata should explicitly identify serialized read-only properties");
		WorkspaceDescriptorCapture capture;
		const WorkspaceHostApiV1 host{ sizeof(WorkspaceHostApiV1), WorkspaceHostApiVersion, &capture, &CaptureWorkspaceDescriptor };
		Require(SailorGetWorkspaceModuleApiV1()->registerTypes(&host) == static_cast<uint32_t>(EWorkspaceModuleResult::Success),
			"workspace type descriptor should be available for common schema comparison");
		const auto* typeInfo = static_cast<const TypeInfo*>(capture.m_descriptor.typeInfo);
		Require(Utils::AreYamlNodesEqual(typeInfo->Serialize(), type),
			"workspace and engine TypeInfo serialization must produce the same complete property schema");
		Require(type["properties"]["skippedDefault"].as<std::string>() == "float",
			"SkipCDO properties should remain available in the writable property schema");
		Require(type["properties"]["mode"].as<std::string>() == "enum WorkspaceFixture::EFixtureMode",
			"workspace component metadata should preserve the enum's declaring namespace");
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
		const YAML::Node settingsType = FindType(metadata["engineTypes"], "WorkspaceFixture::FixtureSettings");
		const YAML::Node tuningType = FindType(metadata["engineTypes"], "WorkspaceFixture::FixtureTuning");
		Require(type["properties"]["settings"].as<std::string>() == "WorkspaceFixture::FixtureSettings" &&
			settingsType["properties"]["layers"].as<std::string>() == "List<WorkspaceFixture::FixtureTuning>" &&
			settingsType["properties"]["modes"].as<std::string>() == "List<enum WorkspaceFixture::EFixtureMode>",
			"workspace catalog should include value types reached through nested records and lists");
		Require(tuningType["propertyRanges"]["gain"]["max"].as<float>() == 1.0f,
			"nested value schemas should use the same range metadata as component schemas");
		const YAML::Node tuningDefaults = FindType(metadata["cdos"], "WorkspaceFixture::FixtureTuning");
		Require(tuningDefaults["defaultValues"]["gain"].as<float>() == 0.25f,
			"workspace catalog should export defaults for nested value types");

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
		const YAML::Node expected = ReadMetadata();
		const int64_t timestamp = expected["timeStamp"].as<int64_t>();
		std::string expectedValue;
		Require(Utils::CanonicalizeYaml(expected, expectedValue, Utils::EYamlCanonicalizationMode::SemanticValue),
			"fixture metadata should have a canonical value");
		// YAML's const traversal updates lazy caches; share only the comparison text.
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
							auto actual = ReadMetadata();
							actual["timeStamp"] = timestamp;
							std::string actualValue;
							if (!Utils::CanonicalizeYaml(actual, actualValue, Utils::EYamlCanonicalizationMode::SemanticValue) ||
								actualValue != expectedValue)
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
		Require(SailorWorkspaceFixtureConstructionCount() == 1,
			"repeated concurrent catalog requests must capture exactly one component default object");
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
		Require(typeInfo->GetDefaultValues() != nullptr,
			"TypeInfo should expose the captured default object");
		Require(!typeInfo->HasAmbiguousProperties(), "fixture properties should be unambiguous");
		Require(typeInfo->Size() == capture.m_descriptor.typeSize &&
			typeInfo->Alignment() == capture.m_descriptor.typeAlignment,
			"registration descriptor should match the compiled TypeInfo layout");
		const YAML::Node& descriptorDefaults = *typeInfo->GetDefaultValues();
		Require(descriptorDefaults["nullableComponent"].IsNull(),
			"canonical descriptor defaults should preserve null object references");

		const YAML::Node metadata = ReadMetadata();
		const YAML::Node defaultObject = FindType(metadata["cdos"], FixtureTypeName);
		Require(defaultObject["defaultValues"].IsMap(),
			"workspace fixture should expose reflected defaults for lease validation");

		const auto gate = TSharedPtr<FactoryInvocationGate>::Make();
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
		Sailor::TVector<Reflection::WorkspaceTypeRegistration> registrations;
		registrations.Add(std::move(registration));
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
		Require(
			TypeInfo::GetReflectedPropertyTypeName<
				TObjectPtr<StableTypeNameFixture::ProjectAsset>>() ==
				"TObjectPtr<StableTypeNameFixture::ProjectAsset>",
			"workspace metadata should export canonical TObjectPtr pointee types without compiler mangling");
	}

	void TestEngineRangeAnnotation()
	{
		const TypeInfo& cameraType = TypeInfo::Get<CameraComponent>();
		const auto fovRange = cameraType.PropertyRanges().Find("fov");
		Require(fovRange != cameraType.PropertyRanges().end(),
			"CameraComponent fov should expose its editor range through TypeInfo");
		Require(fovRange.Value().m_min == 1.0 && fovRange.Value().m_max == 179.0,
			"CameraComponent fov should preserve its declared range bounds");

		const YAML::Node metadata = cameraType.Serialize();
		Require(metadata["propertyRanges"].IsMap() &&
			metadata["propertyRanges"]["fov"]["min"].as<double>() == 1.0 &&
			metadata["propertyRanges"]["fov"]["max"].as<double>() == 179.0,
			"engine TypeInfo serialization should export propertyRanges metadata");

		for (bool bHasExplicitEmptyMap : { false, true })
		{
			YAML::Node withoutRanges = YAML::Clone(metadata);
			withoutRanges.remove("propertyRanges");
			if (bHasExplicitEmptyMap) withoutRanges["propertyRanges"] = YAML::Node(YAML::NodeType::Map);
			TypeInfo parsedType = cameraType;
			parsedType.Deserialize(withoutRanges);
			Require(parsedType.PropertyRanges().IsEmpty(),
				"absent or empty optional ranges must clear previously populated annotations");
			withoutRanges["propertyRanges"] = YAML::Node(YAML::NodeType::Map);
			Require(Utils::AreYamlNodesEqual(parsedType.Serialize(), withoutRanges),
				"the current producer must normalize absent ranges to an explicit empty map without losing other metadata");
			parsedType.Deserialize(metadata);
			Require(Utils::AreYamlNodesEqual(parsedType.Serialize(), metadata),
				"reading annotated metadata again must restore ranges without retaining the prior empty state");
		}
		std::cout << "Reflection metadata: absent/empty ranges, canonical map and annotation replacement passed\n";

		const TypeInfo& numericType = TypeInfo::Get<RangeAnnotationFixture::NumericProperties>();
		Require(numericType.PropertyRanges()["signedValue"].m_min == -10.0 &&
			numericType.PropertyRanges()["signedValue"].m_max == 10.0 &&
			numericType.PropertyRanges()["unsignedValue"].m_min == 0.0 &&
			numericType.PropertyRanges()["unsignedValue"].m_max == 20.0,
			"TypeInfo should export representable int32 and uint32 property ranges");

		const YAML::Node numericMetadata = numericType.Serialize();
		const YAML::Node readOnly = numericMetadata["readOnlyProperties"];
		Require(readOnly.size() == 2 && ContainsScalar(readOnly, "inheritedReadOnly") &&
			ContainsScalar(readOnly, "skippedReadOnly") && !ContainsScalar(readOnly, "transientValue"),
			"common TypeInfo must preserve inherited read-only and SkipCDO properties but exclude transient values");
		TypeInfo roundTrip = numericType;
		roundTrip.Deserialize(numericMetadata);
		Require(Utils::AreYamlNodesEqual(roundTrip.Serialize(), numericMetadata),
			"common type metadata must retain read-only properties on a serialization round trip");
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "RegistrationContract", TestRegistrationContract },
		{ "MetadataSchemaAndDefaults", TestMetadataSchemaAndDefaults },
		{ "RepeatedAndConcurrentCalls", TestRepeatedAndConcurrentCalls },
		{ "FactoryInvocationLease", TestFactoryInvocationLease },
		{ "EngineRegistryRemainsEngineOnly", TestEngineRegistryRemainsEngineOnly },
		{ "StablePropertyNames", TestStablePropertyNames },
		{ "EngineRangeAnnotation", TestEngineRangeAnnotation },
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
