#include "AssetRegistry/AssetCache.h"
#include "Components/Component.h"
#include "Core/Reflection.h"
#include "Core/YamlUtils.h"
#include "Engine/EngineLoop.h"
#include "Memory/ObjectAllocator.hpp"
#include "Platform/DynamicLibrary.h"
#include "Workspace/WorkspaceModuleApi.h"
#include "Workspace/WorkspaceContext.h"
#include "Workspace/WorkspaceModuleManager.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

using namespace Sailor;
using namespace Sailor::Workspace;

namespace WorkspaceOwnerSentinel
{
	class OwnerSentinelComponent final : public Sailor::Component
	{
		SAILOR_WORKSPACE_REFLECTABLE(OwnerSentinelComponent)

	public:
		OwnerSentinelComponent() = default;
	};
}

REFL_AUTO(
	type(WorkspaceOwnerSentinel::OwnerSentinelComponent, bases<Sailor::Component>)
)

namespace
{
	constexpr const char* FixtureModuleName = "WorkspaceFixture";
	constexpr const char* FixtureTypeName = "WorkspaceFixture::FixtureComponent";

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	std::filesystem::path ModuleFilename(const std::string& moduleName)
	{
#if defined(_WIN32)
		return moduleName + ".dll";
#elif defined(__APPLE__)
		return "lib" + moduleName + ".dylib";
#else
		return "lib" + moduleName + ".so";
#endif
	}

	void WriteManifest(
		const std::filesystem::path& root,
		const std::string& moduleName,
		const std::string& logicOutputPath = "Binaries")
	{
		std::filesystem::create_directories(root);
		std::filesystem::create_directories(root.parent_path() / "Engine" / "Content");
		std::ofstream manifest(root / "workspace.sailor");
		manifest
			<< "manifestVersion: 1\n"
			<< "workspaceId: 00000000-0000-0000-0000-000000000001\n"
			<< "name: Workspace Module Runtime Fixture\n"
			<< "enginePath: ../Engine\n"
			<< "engineReferenceKind: source\n"
			<< "contentPath: Content\n"
			<< "sourcePath: Source\n"
			<< "generatedProjectPath: Generated\n"
			<< "cachePath: Cache\n"
			<< "buildPath: Cache/Build\n"
			<< "logicOutputPath: " << logicOutputPath << "\n"
			<< "logicModuleName: " << moduleName << "\n";
		Require(manifest.good(), "workspace fixture manifest should be writable");
	}

	WorkspaceContext ResolveContext(
		const std::filesystem::path& root,
		const std::filesystem::path& manifest = {})
	{
		WorkspaceContextResolveResult result = ResolveWorkspaceContext(root, manifest);
		Require(result.IsSuccess(), "workspace context should resolve: " + result.m_message);
		return std::move(result.m_context);
	}

	const WorkspaceModuleLoadResult& LoadWorkspaceModule(
		WorkspaceModuleManager& manager,
		const std::filesystem::path& root,
		const std::string& config)
	{
		const WorkspaceContext context = ResolveContext(root);
		return manager.Load(context, config);
	}

	std::filesystem::path InstallFixture(
		const std::filesystem::path& root,
		const std::string& config,
		const std::string& moduleName,
		const std::filesystem::path& sourceLibrary,
		const std::string& logicOutputPath = "Binaries")
	{
		const std::filesystem::path destination =
			root / PathFromUtf8(logicOutputPath) / PathFromUtf8(config) / ModuleFilename(moduleName);
		std::filesystem::create_directories(destination.parent_path());
		// Replace after unload; overwriting a signed dylib invalidates cached code pages.
		std::filesystem::remove(destination);
		std::filesystem::copy_file(sourceLibrary, destination);
		return destination;
	}

	bool ContainsEngineType(const YAML::Node& metadata, const std::string& typeName)
	{
		for (const YAML::Node& type : metadata["engineTypes"])
		{
			if (type["typename"] && type["typename"].as<std::string>() == typeName)
			{
				return true;
			}
		}
		return false;
	}

	YAML::Node FindMetadataEntry(
		const YAML::Node& entries,
		const std::string& typeName)
	{
		for (const YAML::Node& entry : entries)
		{
			if (entry["typename"] && entry["typename"].as<std::string>() == typeName)
			{
				return entry;
			}
		}

		return YAML::Node(YAML::NodeType::Undefined);
	}

	size_t CountEnumEntries(const YAML::Node& entries, const std::string& enumName)
	{
		return static_cast<size_t>(std::count_if(entries.begin(), entries.end(), [&](const YAML::Node& entry)
			{
				return entry[enumName].IsDefined();
			}));
	}

	void TestDiscoveryFailures(const std::filesystem::path& tempRoot, const std::string& config)
	{
		const std::filesystem::path engineRoot = tempRoot / "engine-only";
		std::filesystem::create_directories(engineRoot);
		const auto engineContext = ResolveContext(engineRoot);
		Require(engineContext.IsEngineMode() && engineContext.GetManifest().empty() &&
			engineContext.GetRoot() == engineContext.GetEngineRoot() &&
			engineContext.GetCache() == engineContext.GetEngineRoot() / "Cache",
			"without a project, the engine root must own content and cache without inventing a manifest");
		WorkspaceModuleManager engineManager;
		const auto& engine = engineManager.Load(engineContext, config);
		Require(engine.m_status == EWorkspaceModuleLoadStatus::NotConfigured && !engineManager.IsRegistered(),
			"workspace without a manifest should preserve engine-only startup");
		std::cout << "Engine-only module discovery: no manifest/module and engine-owned cache passed\n";

		const std::filesystem::path missingRoot = tempRoot / "missing";
		WriteManifest(missingRoot, FixtureModuleName);
		WorkspaceModuleManager missingManager;
		const auto& missing = LoadWorkspaceModule(missingManager, missingRoot, config);
		Require(missing.m_status == EWorkspaceModuleLoadStatus::ModuleNotFound,
			"missing workspace binary should return ModuleNotFound");
		Require(missing.m_message.find(config) != std::string::npos,
			"missing-module diagnostic should identify the active configuration");

		const std::filesystem::path ambiguousRoot = tempRoot / "ambiguous";
		std::filesystem::create_directories(ambiguousRoot);
		std::ofstream(ambiguousRoot / "one.sailor") << "manifestVersion: 1\n";
		std::ofstream(ambiguousRoot / "two.sailor") << "manifestVersion: 1\n";
		const WorkspaceContextResolveResult ambiguous = ResolveWorkspaceContext(ambiguousRoot);
		Require(!ambiguous.IsSuccess(),
			"multiple non-default manifests should require an explicit path");
	}

	void TestDynamicLibraryFailures(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& inertLibrary)
	{
		Platform::DynamicLibrary library;
		Require(!library.Open(tempRoot / "does-not-exist" / ModuleFilename(FixtureModuleName)),
			"dynamic loader should reject a missing library");
		Require(!library.GetError().empty(), "missing-library diagnostic should not be empty");

		Require(library.Open(inertLibrary),
			"dynamic loader should open the inert fixture: " + library.GetError());
		Require(library.GetSymbol("SailorWorkspaceMissingEntryFixtureSentinel") != nullptr,
			"dynamic loader should resolve an exported fixture symbol");
		Require(library.GetSymbol("SailorDefinitelyMissingWorkspaceSymbol") == nullptr,
			"dynamic loader should report a missing symbol");
		Require(!library.GetError().empty(), "missing-symbol diagnostic should not be empty");
		Require(library.Close(), "dynamic loader should close the fixture after symbol validation");
	}

	void TestFailedOwnerClaimPreservesExistingRegistration(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& fixtureLibrary,
		const std::string& config)
	{
		const std::filesystem::path root = tempRoot / "duplicate-owner";
		WriteManifest(root, FixtureModuleName);
		const std::filesystem::path installedFixture = InstallFixture(
			root,
			config,
			FixtureModuleName,
			fixtureLibrary);
		std::error_code pathError;
		const std::filesystem::path modulePath = std::filesystem::weakly_canonical(
			installedFixture,
			pathError);
		Require(!pathError, "duplicate-owner fixture path should be canonicalizable");
		const std::string owner = std::string(FixtureModuleName) + "@" + PathToUtf8(modulePath);

		const TypeInfo& sentinelType = WorkspaceOwnerSentinel::OwnerSentinelComponent::GetStaticTypeInfo();
		Reflection::WorkspaceTypeRegistration sentinelRegistration;
		sentinelRegistration.m_typeInfo = &sentinelType;
		sentinelRegistration.m_alignment = alignof(WorkspaceOwnerSentinel::OwnerSentinelComponent);
		sentinelRegistration.m_defaultObject = Reflection::CreateReflectedData(
			sentinelType,
			YAML::Node(YAML::NodeType::Map));
		sentinelRegistration.m_placementFactory = [](void* destination) -> IReflectable*
		{
			return new (destination) WorkspaceOwnerSentinel::OwnerSentinelComponent();
		};

		Sailor::TVector<Reflection::WorkspaceTypeRegistration> registrations;
		registrations.Add(std::move(sentinelRegistration));
		std::string registrationError;
		Require(
			Reflection::RegisterWorkspaceTypes(owner, std::move(registrations), registrationError),
			"sentinel owner registration should succeed: " + registrationError);

		WorkspaceModuleManager manager;
		const EWorkspaceModuleLoadStatus resultStatus = LoadWorkspaceModule(manager, root, config).m_status;
		const bool bOwnerPreserved = Reflection::GetNumWorkspaceTypes(owner) == 1 &&
			Reflection::IsWorkspaceTypeRegistered(sentinelType.Name());
		Reflection::UnregisterWorkspaceTypes(owner);

		Require(resultStatus == EWorkspaceModuleLoadStatus::RegistrationFailed,
			"a module must not claim an owner that is already registered: " + manager.GetResult().m_message);
		Require(!manager.IsRegistered(), "a rejected owner claim must leave the manager unregistered");
		Require(bOwnerPreserved,
			"a rejected manager must not unregister the active registration owned by another manager");
	}

	void TestWorkspaceCdoAddressStability()
	{
		constexpr const char* primaryOwner = "WorkspaceModuleRuntimeTests.CdoAddressPrimary";
		constexpr const char* growthOwner = "WorkspaceModuleRuntimeTests.CdoAddressGrowth";
		const TypeInfo& sentinelType = WorkspaceOwnerSentinel::OwnerSentinelComponent::GetStaticTypeInfo();

		auto makeRegistration = [](const TypeInfo& type, const YAML::Node& properties)
		{
			Reflection::WorkspaceTypeRegistration registration;
			registration.m_typeInfo = &type;
			registration.m_alignment = alignof(WorkspaceOwnerSentinel::OwnerSentinelComponent);
			registration.m_defaultObject = Reflection::CreateReflectedData(type, properties);
			registration.m_placementFactory = [](void*) -> IReflectable* { return nullptr; };
			return registration;
		};

		YAML::Node sentinelProperties(YAML::NodeType::Map);
		sentinelProperties["addressStabilitySentinel"] = 73;
		TVector<Reflection::WorkspaceTypeRegistration> primaryRegistrations;
		primaryRegistrations.Add(makeRegistration(sentinelType, sentinelProperties));
		std::string registrationError;
		Require(
			Reflection::RegisterWorkspaceTypes(primaryOwner, std::move(primaryRegistrations), registrationError),
			"primary CDO address-stability registration should succeed: " + registrationError);

		const ReflectedData* addressBeforeGrowth = &Reflection::GetCDO(sentinelType.Name());
		const ReflectedData snapshot = *addressBeforeGrowth;

		std::vector<TypeInfo> fillerTypes;
		fillerTypes.reserve(64);
		TVector<Reflection::WorkspaceTypeRegistration> growthRegistrations;
		growthRegistrations.Reserve(64);
		for (size_t i = 0; i < 64; ++i)
		{
			fillerTypes.emplace_back(sentinelType);
			YAML::Node fillerType = fillerTypes.back().Serialize();
			fillerType["typename"] = "WorkspaceCdoAddressFiller::Type" + std::to_string(i);
			fillerTypes.back().Deserialize(fillerType);
			growthRegistrations.Add(makeRegistration(fillerTypes.back(), YAML::Node(YAML::NodeType::Map)));
		}

		const bool bGrowthRegistered = Reflection::RegisterWorkspaceTypes(
			growthOwner,
			std::move(growthRegistrations),
			registrationError);
		if (!bGrowthRegistered)
		{
			Reflection::UnregisterWorkspaceTypes(primaryOwner);
			Require(false, "growth CDO registrations should succeed: " + registrationError);
		}

		const ReflectedData* addressAfterGrowth = &Reflection::GetCDO(sentinelType.Name());
		const bool bAddressStable = addressBeforeGrowth == addressAfterGrowth;
		const bool bContentsStable = *addressAfterGrowth == snapshot &&
			addressAfterGrowth->GetProperties()["addressStabilitySentinel"].as<int>() == 73;
		const size_t numGrowthRemoved = Reflection::UnregisterWorkspaceTypes(growthOwner);
		const size_t numPrimaryRemoved = Reflection::UnregisterWorkspaceTypes(primaryOwner);

		Require(bAddressStable,
			"workspace CDO address should remain stable when later registrations grow the registry map");
		Require(bContentsStable,
			"workspace CDO contents should remain intact when later registrations grow the registry map");
		Require(numGrowthRemoved == 64 && numPrimaryRemoved == 1,
			"address-stability registrations should be removed exactly once");
	}

	void TestIncompatibleModule(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& incompatibleLibrary,
		const std::string& config)
	{
		const std::filesystem::path root = tempRoot / "incompatible";
		WriteManifest(root, "IncompatibleFixture");
		InstallFixture(root, config, "IncompatibleFixture", incompatibleLibrary);

		WorkspaceModuleManager manager;
		const auto& result = LoadWorkspaceModule(manager, root, config);
		Require(result.m_status == EWorkspaceModuleLoadStatus::AbiMismatch,
			"incompatible module should fail before metadata or registration callbacks");
		Require(result.m_message.find("sailor-workspace-abi-" +
			std::to_string(WorkspaceModuleAbiRevision - 1)) != std::string::npos,
			"ABI mismatch diagnostics should identify the stale module revision");
		Require(!manager.IsRegistered(), "incompatible module must not leave registrations behind");
	}

	void TestMissingEntryPoint(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& missingEntryLibrary,
		const std::string& config)
	{
		const std::filesystem::path root = tempRoot / "missing-entry-point";
		WriteManifest(root, "MissingEntryFixture");
		InstallFixture(root, config, "MissingEntryFixture", missingEntryLibrary);

		WorkspaceModuleManager manager;
		const auto& result = LoadWorkspaceModule(manager, root, config);
		Require(result.m_status == EWorkspaceModuleLoadStatus::EntryPointMissing,
			"module without the V1 API symbol should return EntryPointMissing");
		Require(result.m_message.find(WorkspaceModuleApiEntryPointV1) != std::string::npos,
			"missing-entry diagnostic should name the required symbol");
		Require(!manager.IsRegistered(), "missing entry point must not leave registrations behind");
	}

	void TestInterfaceMismatch(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& staleLibrary,
		const std::filesystem::path& validLibrary,
		const std::string& config)
	{
		const auto root = tempRoot / "interface-mismatch";
		WriteManifest(root, "InterfaceMismatchFixture");
		const auto installed = InstallFixture(root, config, "InterfaceMismatchFixture", staleLibrary);
		Platform::DynamicLibrary observer(installed);
		Require(observer.IsOpen(), "the stale SDK fixture should load for callback observation");
		const auto getApi = reinterpret_cast<TGetWorkspaceModuleApiV1>(observer.GetSymbol(WorkspaceModuleApiEntryPointV1));
		using TCallbackCount = uint32_t (SAILOR_WORKSPACE_CALL *)() noexcept;
		const auto callbackCount = reinterpret_cast<TCallbackCount>(
			observer.GetSymbol("SailorWorkspaceInterfaceFixtureCallbackCount"));
		Require(getApi && callbackCount, "the stale SDK fixture should expose its API and callback counter");
		const auto* api = getApi();
		const std::string_view staleTag(api->abiTag, api->abiTagLength);
		const std::string_view currentTag = GetWorkspaceModuleAbiTagV1();
		const auto interfaceOffset = currentTag.find(";interface=");
		Require(interfaceOffset != std::string_view::npos &&
			staleTag.substr(0, interfaceOffset) == currentTag.substr(0, interfaceOffset) && staleTag != currentTag,
			"the fixture must differ only in SDK identity, not compiler, configuration or ABI revision");

		WorkspaceModuleManager manager;
		const auto& result = LoadWorkspaceModule(manager, root, config);
		Require(result.m_status == EWorkspaceModuleLoadStatus::AbiMismatch,
			"a module built against different engine headers must be rejected: " + result.m_message);
		Require(callbackCount() == 0, "ABI rejection must precede all metadata and registration callbacks");
		Require(!manager.IsRegistered() && manager.GetMetadata().empty(),
			"an incompatible SDK must not publish types or metadata");

		const auto validRoot = tempRoot / "interface-recovery";
		WriteManifest(validRoot, FixtureModuleName);
		InstallFixture(validRoot, config, FixtureModuleName, validLibrary);
		Require(LoadWorkspaceModule(manager, validRoot, config).IsSuccess(),
			"the same loader must accept a module rebuilt against the current SDK");
		Require(Reflection::TryGetTypeByName(FixtureTypeName) != nullptr && manager.Unload(),
			"the rebuilt module must register and unload its reflected component");
		std::cout << "[PASS] Workspace SDK mismatch rejects before callbacks; current module loads" << std::endl;
	}

	void TestPreviousSdkModule(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& staleLibrary,
		const std::filesystem::path& rebuiltLibrary,
		const std::string& config)
	{
		const auto root = tempRoot / "previous-sdk";
		WriteManifest(root, FixtureModuleName);
		InstallFixture(root, config, FixtureModuleName, staleLibrary);
		WorkspaceModuleManager manager;
		const auto& result = LoadWorkspaceModule(manager, root, config);
		Require(result.m_status == EWorkspaceModuleLoadStatus::AbiMismatch,
			"a real module from the previous SDK must be rejected before reading TypeInfo: " + result.m_message);
		std::cout << "[PASS] Previous SDK: " << result.m_message << std::endl;
		InstallFixture(root, config, FixtureModuleName, rebuiltLibrary);
		Require(LoadWorkspaceModule(manager, root, config).IsSuccess() && manager.Unload(),
			"rebuilding the same workspace module must restore loading");
	}

	void TestChangedTypeCatalogReload(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& initialLibrary,
		const std::filesystem::path& reloadedLibrary,
		const std::string& config)
	{
		constexpr const char* reloadedTypeName = "WorkspaceFixture::ReloadedComponent";
		const auto root = tempRoot / "changed-catalog";
		WriteManifest(root, FixtureModuleName);
		InstallFixture(root, config, FixtureModuleName, initialLibrary);
		WorkspaceModuleManager manager;
		Require(LoadWorkspaceModule(manager, root, config).IsSuccess(), "initial module must load before replacement");
		const uint64_t initialHash = manager.GetTypeCatalogHash();
		Require(initialHash != 0, "a loaded module must publish its type catalog identity");
		const YAML::Node engineMetadata = Reflection::ExportEngineTypes();
		YAML::Node initialMetadata;
		std::string error;
		Require(manager.BuildEditorTypeMetadata(engineMetadata, initialMetadata, error) &&
			ContainsEngineType(initialMetadata, FixtureTypeName) && !ContainsEngineType(initialMetadata, reloadedTypeName),
			"the initial editor catalog must contain only the first module's component");
		Require(manager.Unload(), "the initial module must unload before replacing its binary");
		Require(manager.GetTypeCatalogHash() == 0, "unloading must clear the module's catalog identity");
		Require(LoadWorkspaceModule(manager, root, config).IsSuccess() && manager.GetTypeCatalogHash() == initialHash,
			"reloading the same module must preserve its type catalog identity");
		Require(manager.Unload(), "the unchanged module must unload before replacement");

		InstallFixture(root, config, FixtureModuleName, reloadedLibrary);
		Require(LoadWorkspaceModule(manager, root, config).IsSuccess(), "the rebuilt module must load at the same path");
		Require(manager.GetTypeCatalogHash() != 0 && manager.GetTypeCatalogHash() != initialHash,
			"changed types and defaults must publish a new catalog identity");
		YAML::Node reloadedMetadata;
		Require(manager.BuildEditorTypeMetadata(engineMetadata, reloadedMetadata, error) &&
			ContainsEngineType(reloadedMetadata, reloadedTypeName) && !ContainsEngineType(reloadedMetadata, FixtureTypeName),
			"reloading changed logic must remove old types and expose new types to the editor");
		Require(ContainsEngineType(initialMetadata, FixtureTypeName) && !ContainsEngineType(initialMetadata, reloadedTypeName),
			"reloading a module must not mutate an earlier editor catalog");
		Require(Reflection::TryGetTypeByName(FixtureTypeName) == nullptr,
			"the removed component must not remain in the reflection registry");
		const TypeInfo* reloadedType = Reflection::TryGetTypeByName(reloadedTypeName);
		Require(reloadedType != nullptr, "the new component must be registered");
		auto allocator = Memory::ObjectAllocatorPtr::Make(Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		auto component = Reflection::CreateObject<Component>(*reloadedType, allocator);
		Require(component && component->GetReflectedData().Serialize()["overrideProperties"]["m_capacity"].as<uint32_t>() == 12,
			"the replacement component must instantiate with its new default value");
		component.ForcelyDestroyObject();
		Require(manager.Unload() && Reflection::TryGetTypeByName(reloadedTypeName) == nullptr,
			"the replacement catalog must be removed on unload");
		std::cout << "[PASS] Changed workspace binary replaces reflected types and editor catalog" << std::endl;
	}

	void TestUnknownReflectedType()
	{
		YAML::Node serialized;
		serialized["typename"] = "MissingWorkspace::UnknownComponent";
		serialized["overrideProperties"] = YAML::Node(YAML::NodeType::Map);

		ReflectedData reflected;
		reflected.Deserialize(serialized);
		Require(!reflected.IsValid(),
			"unknown reflected type should deserialize as an invalid value instead of crashing");
	}

	void TestWorldInstantiationGuards()
	{
		EngineLoop engineLoop(120u);
		Require(engineLoop.GetFpsCap() == 120u,
			"EngineLoop should retain the FPS cap passed by application startup");
		Require(!engineLoop.InstantiateWorld({}, EngineLoop::DefaultWorldMask),
			"unavailable world prefab should be rejected without creating a partial world");
		Require(engineLoop.GetWorlds().IsEmpty(),
			"failed world instantiation must not add a world to the engine loop");
	}

	void TestModuleIdentityMismatch(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& fixtureLibrary,
		const std::string& config)
	{
		const std::filesystem::path root = tempRoot / "identity-mismatch";
		WriteManifest(root, "RenamedFixture");
		InstallFixture(root, config, "RenamedFixture", fixtureLibrary);

		WorkspaceModuleManager manager;
		const auto& result = LoadWorkspaceModule(manager, root, config);
		Require(result.m_status == EWorkspaceModuleLoadStatus::ApiInvalid,
			"renaming a module must not bypass its declared module identity");
		Require(!manager.IsRegistered(), "identity mismatch must not leave registrations behind");
	}

	void TestInvalidModules(const std::filesystem::path& tempRoot, const std::filesystem::path& validLibrary,
		const std::vector<std::filesystem::path>& invalidLibraries, const std::string& buildConfig)
	{
		const std::string config = buildConfig + reinterpret_cast<const char*>(u8" \u042f \u8239");
		const auto liveRoot = tempRoot / "live-owner";
		WriteManifest(liveRoot, FixtureModuleName);
		InstallFixture(liveRoot, config, FixtureModuleName, validLibrary);
		WorkspaceModuleManager liveManager;
		const auto& liveResult = LoadWorkspaceModule(liveManager, liveRoot, config);
		Require(liveResult.IsSuccess(), "valid owner must load before invalid module tests: " + liveResult.m_message);
		const TypeInfo* liveType = Reflection::TryGetTypeByName(FixtureTypeName);
		const std::string liveOwner = liveResult.m_moduleName + "@" + PathToUtf8(liveResult.m_modulePath);
		const YAML::Node engineBefore = Reflection::ExportEngineTypes();
		struct InvalidCase { EWorkspaceModuleLoadStatus m_status; const char* m_diagnostic; };
		using Status = EWorkspaceModuleLoadStatus;
		const InvalidCase cases[] = {
			{ Status::MetadataInvalid, "declared member" },
			{ Status::MetadataInvalid, "declared member" },
			{ Status::MetadataInvalid, "declared member" },
			{ Status::MetadataInvalid, "ambiguous" },
			{ Status::RegistrationFailed, "incompatible" },
			{ Status::RegistrationFailed, "incompatible" },
			{ Status::RegistrationFailed, "invalid type descriptor" },
			{ Status::RegistrationFailed, "invalid type descriptor" },
			{ Status::RegistrationFailed, "invalid type descriptor" },
			{ Status::MetadataInvalid, "does not resolve" },
			{ Status::RegistrationFailed, "conflicts" },
			{ Status::ApiInvalid, "API table" }
		};
		Require(invalidLibraries.size() == std::size(cases), "all compiled invalid fixtures must be supplied");
		WorkspaceModuleManager manager;
		for (size_t i = 0; i < std::size(cases); ++i)
		{
			const auto root = tempRoot / ("invalid-" + std::to_string(i + 1));
			WriteManifest(root, "InvalidFixture");
			InstallFixture(root, config, "InvalidFixture", invalidLibraries[i]);
			const auto& result = LoadWorkspaceModule(manager, root, config);
			Require(result.m_status == cases[i].m_status && result.m_message.find(cases[i].m_diagnostic) != std::string::npos,
				"invalid module case " + std::to_string(i + 1) + " must fail at its boundary: " + result.m_message);
			Require(manager.GetState() == EWorkspaceModuleState::Failed && manager.GetMetadata().empty() &&
				result.m_numRegisteredTypes == 0 && Reflection::TryGetTypeByName("InvalidWorkspace::FixtureComponent") == nullptr &&
				Reflection::TryGetTypeByName("InvalidWorkspace::BaseComponent") == nullptr,
				"a rejected module must not publish even its valid base component");
			Require(Reflection::TryGetTypeByName(FixtureTypeName) == liveType && Reflection::GetNumWorkspaceTypes(liveOwner) == 1,
				"rejected module must preserve the unrelated live owner");
			const YAML::Node engineAfter = Reflection::ExportEngineTypes();
			for (const char* section : { "engineTypes", "cdos", "enums", "assetTypes" })
				Require(Utils::AreYamlNodesEqual(engineBefore[section], engineAfter[section]),
					"rejected module must preserve engine metadata");
			std::cout << "[PASS] Invalid module case " << i + 1 << ": " << result.m_message << '\n';
		}
		Require(liveManager.Unload(), "live owner must unload after rejected modules");
		Require(LoadWorkspaceModule(manager, liveRoot, config).IsSuccess(), "loader must recover with a valid module");
		const TypeInfo* type = Reflection::TryGetTypeByName(FixtureTypeName);
		auto allocator = Memory::ObjectAllocatorPtr::Make(Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		ComponentPtr component = Reflection::CreateObject<Component>(*type, allocator);
		Require(component && component->GetReflectedData().GetProperties()["moveSpeed"].as<float>() == 5.0f,
			"recovered module must instantiate its component");
		component.ForcelyDestroyObject();
		Require(manager.Unload() && Reflection::TryGetTypeByName(FixtureTypeName) == nullptr,
			"recovered module must unload cleanly");
	}

	void TestRegistrationInstantiationAndCleanup(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& fixtureLibrary,
		const std::string& config)
	{
		const YAML::Node engineTypesBefore = Reflection::ExportEngineTypes();
		const TypeInfo* engineComponentType = Reflection::TryGetTypeByName("Sailor::Component");
		Require(engineComponentType != nullptr, "engine Component TypeInfo should be registered");
		Require(!ContainsEngineType(engineTypesBefore, FixtureTypeName),
			"workspace fixture must not be present before module registration");
		WorkspaceModuleManager engineOnlyManager;
		YAML::Node engineOnlyEditorTypes;
		std::string metadataError;
		Require(engineOnlyManager.BuildEditorTypeMetadata(
				engineTypesBefore,
				engineOnlyEditorTypes,
				metadataError),
			"unconfigured workspace should build engine-only editor metadata: " + metadataError);
		Require(engineOnlyEditorTypes["engineTypes"].size() == engineTypesBefore["engineTypes"].size() &&
			!ContainsEngineType(engineOnlyEditorTypes, FixtureTypeName),
			"unconfigured editor metadata should remain engine-only");

		const std::filesystem::path root = tempRoot / "registered workspace with spaces";
		const std::string logicOutputPath = "Logic Output With Spaces";
		WriteManifest(root, FixtureModuleName, logicOutputPath);
		InstallFixture(root, config, FixtureModuleName, fixtureLibrary, logicOutputPath);
		const WorkspaceContext context = ResolveContext(root);
		std::ofstream(root / "workspace.sailor", std::ios::trunc)
			<< "manifestVersion: 999\n";

		WorkspaceModuleManager manager;
		const auto& result = manager.Load(context, config);
		Require(result.IsSuccess(), "workspace fixture should load and register: " + result.m_message);
		{
			Platform::DynamicLibrary module(result.m_modulePath);
			using TAssetCacheSize = uint64_t (SAILOR_WORKSPACE_CALL *)() noexcept;
			const auto assetCacheSize = reinterpret_cast<TAssetCacheSize>(module.GetSymbol("SailorWorkspaceFixtureAssetCacheSize"));
			Require(assetCacheSize && assetCacheSize() == sizeof(AssetCache),
				"source and installed SDK clients must use the runtime's public class layout, including test-only fields");
		}
		Require(result.m_manifestPath == context.GetManifest(),
			"module manager should consume the captured context without reparsing a changed manifest");
		Require(result.m_numRegisteredTypes == 1, "workspace fixture should register one type");
		Require(Reflection::IsWorkspaceTypeRegistered(FixtureTypeName),
			"workspace fixture type should be visible in unified reflection lookup");
		Require(Reflection::TryGetTypeByName("Sailor::Component") == engineComponentType,
			"loading a workspace DLL must not replace host engine type registration");

		YAML::Node editorTypes;
		Require(manager.BuildEditorTypeMetadata(engineTypesBefore, editorTypes, metadataError),
			"active workspace should build combined editor metadata: " + metadataError);
		Require(editorTypes["engineTypes"].size() == engineTypesBefore["engineTypes"].size() + 4 &&
			editorTypes["cdos"].size() == engineTypesBefore["cdos"].size() + 4,
			"combined metadata should append the component, nested records and empty record exactly once");
		const auto sharedType = FindMetadataEntry(engineTypesBefore["engineTypes"], "Sailor::LandscapeVegetationSettings");
		Require(sharedType.IsMap() && Utils::AreYamlNodesEqual(sharedType,
			FindMetadataEntry(editorTypes["engineTypes"], "Sailor::LandscapeVegetationSettings")),
			"an engine-owned value record must be shared without registering another component");
		const auto emptyDefault = FindMetadataEntry(editorTypes["cdos"], "WorkspaceFixture::EmptySettings")["defaultValues"];
		Require(emptyDefault.IsMap() && emptyDefault.size() == 0,
			"an empty reflected value record must retain an explicit empty default map");
		Require(ContainsEngineType(editorTypes, "WorkspaceFixture::FixtureSettings") &&
			ContainsEngineType(editorTypes, "WorkspaceFixture::FixtureTuning") &&
			Reflection::TryGetTypeByName("WorkspaceFixture::FixtureSettings") == nullptr,
			"nested value types belong in the editor catalog without becoming registered component factories");
		Require(ContainsEngineType(editorTypes, FixtureTypeName),
			"combined editor metadata should expose the workspace component FQN");
		Require(editorTypes["moduleName"].as<std::string>() == FixtureModuleName,
			"combined editor metadata should identify the active workspace module");
		const YAML::Node editorDefaults = FindMetadataEntry(editorTypes["cdos"], FixtureTypeName);
		Require(editorDefaults.IsDefined() &&
			editorDefaults["defaultValues"]["moveSpeed"].as<float>() == 5.0f,
			"combined editor metadata should expose workspace component defaults");
		const YAML::Node editorFixtureType =
			FindMetadataEntry(editorTypes["engineTypes"], FixtureTypeName);
		Require(editorFixtureType["propertyRanges"]["moveSpeed"]["min"].as<double>() == 0.0 &&
			editorFixtureType["propertyRanges"]["moveSpeed"]["max"].as<double>() == 10.0,
			"combined editor metadata should preserve workspace property ranges");
		Require(CountEnumEntries(editorTypes["enums"], "enum Sailor::EMobilityType") == 1,
			"combined editor metadata should deduplicate referenced engine enum definitions");
		const std::string retainedMetadata = manager.GetMetadata();
		YAML::Node editableCatalog;
		Require(manager.BuildEditorTypeMetadata(engineTypesBefore, editableCatalog, metadataError),
			"a repeated catalog request should succeed");
		FindMetadataEntry(editableCatalog["engineTypes"], FixtureTypeName)["properties"]["moveSpeed"] = "string";
		FindMetadataEntry(editableCatalog["cdos"], "WorkspaceFixture::FixtureTuning")["defaultValues"]["gain"] = 999.0f;
		YAML::Node freshCatalog;
		Require(manager.BuildEditorTypeMetadata(engineTypesBefore, freshCatalog, metadataError) &&
			Utils::AreYamlNodesEqual(freshCatalog, editorTypes) && manager.GetMetadata() == retainedMetadata,
			"caller edits must not modify the retained catalog, nested defaults or metadata payload");

		YAML::Node changedEngine = YAML::Clone(engineTypesBefore);
		changedEngine["timeStamp"] = 42;
		YAML::Node callerType = YAML::Load("{typename: Caller::LateType, base: '', properties: {value: float}}");
		changedEngine["engineTypes"].push_back(callerType);
		Require(manager.BuildEditorTypeMetadata(changedEngine, freshCatalog, metadataError) &&
			ContainsEngineType(freshCatalog, "Caller::LateType") && freshCatalog["timeStamp"].as<int>() == 42,
			"a prepared workspace must still merge the caller's current engine metadata");
		Require(manager.BuildEditorTypeMetadata(engineTypesBefore, freshCatalog, metadataError) &&
			!ContainsEngineType(freshCatalog, "Caller::LateType"),
			"engine metadata from a previous caller must not become part of the retained workspace catalog");
		changedEngine["engineTypes"].push_back(YAML::Clone(editorFixtureType));
		YAML::Node rejectedCatalog;
		rejectedCatalog["sentinel"] = "unchanged";
		Require(!manager.BuildEditorTypeMetadata(changedEngine, rejectedCatalog, metadataError) &&
			rejectedCatalog.size() == 1 && rejectedCatalog["sentinel"].Scalar() == "unchanged",
			"a new engine collision must fail without partially publishing the prepared workspace catalog");
		Require(manager.BuildEditorTypeMetadata(engineTypesBefore, freshCatalog, metadataError) &&
			Utils::AreYamlNodesEqual(freshCatalog, editorTypes),
			"a failed merge must leave the prepared workspace available for later requests");
		for (const auto section : { "engineTypes", "cdos", "enums" })
		{
			YAML::Node conflictingEngine = YAML::Clone(engineTypesBefore);
			if (std::string_view(section) == "enums")
			{
				for (auto entry : conflictingEngine["enums"])
					if (entry["enum Sailor::ELandscapeVegetationResidency"])
						entry["enum Sailor::ELandscapeVegetationResidency"].push_back("Invented");
			}
			else
			{
				auto entry = FindMetadataEntry(conflictingEngine[section], "Sailor::LandscapeVegetationSettings");
				if (std::string_view(section) == "engineTypes") entry["properties"]["priority"] = "string";
				else entry["defaultValues"]["priority"] = 123.0f;
			}
			Require(!manager.BuildEditorTypeMetadata(conflictingEngine, rejectedCatalog, metadataError) &&
				rejectedCatalog.size() == 1 && rejectedCatalog["sentinel"].Scalar() == "unchanged",
				"a conflicting shared schema, default or enum must fail transactionally");
		}
		Require(manager.BuildEditorTypeMetadata(engineTypesBefore, freshCatalog, metadataError) &&
			Utils::AreYamlNodesEqual(freshCatalog, editorTypes), "shared-type conflicts must not poison the prepared catalog");

		const TypeInfo* fixtureType = Reflection::TryGetTypeByName(FixtureTypeName);
		Require(fixtureType != nullptr, "workspace fixture TypeInfo should be discoverable by name");
		auto allocator = Memory::ObjectAllocatorPtr::Make(Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		ComponentPtr component = Reflection::CreateObject<Component>(*fixtureType, allocator);
		Require(static_cast<bool>(component), "workspace placement factory should instantiate the custom component");
		{
			const ReflectedData reflected = component->GetReflectedData();
			Require(reflected.GetProperties()["moveSpeed"].as<float>() == 5.0f,
				"instantiated workspace component should expose its reflected default value");
			Require(reflected.GetProperties()["registryLookupSucceeded"].as<bool>(),
				"workspace component construction should re-enter reflection lookup without holding the registry lock");
			Require(reflected.GetProperties()["readOnlyValue"].as<int32_t>() == 17,
				"workspace component construction should preserve getter-only reflected defaults");
			Require(reflected.GetProperties()["skippedReadOnlyValue"].as<int32_t>() == 23,
				"workspace component serialization should retain getter-only SkipCDO properties");
			Require(reflected.GetProperties()["mode"].as<std::string>() == "Default",
				"workspace component construction should preserve validated enum defaults");
			Require(reflected.GetProperties()["mobility"].as<std::string>() == "Stationary",
				"workspace component construction should preserve referenced engine enum defaults");
			Require(reflected.GetProperties()["offset"].IsSequence(),
				"workspace component construction should preserve validated structured defaults");
			Require(reflected.GetProperties()["nullableComponent"].IsNull(),
				"workspace component construction should preserve a null object reference");
		}
		{
			YAML::Node serializedComponent;
			serializedComponent["typename"] = FixtureTypeName;
			serializedComponent["overrideProperties"]["moveSpeed"] = 12.5f;

			ReflectedData deserializedComponent;
			deserializedComponent.Deserialize(serializedComponent);
			Require(deserializedComponent.IsValid() &&
				deserializedComponent.GetTypeInfo().Name() == FixtureTypeName,
				"workspace component deserialization should preserve its exact FQN");

			ComponentPtr roundTrippedComponent = Reflection::CreateObject<Component>(*fixtureType, allocator);
			Require(static_cast<bool>(roundTrippedComponent) &&
				Reflection::ApplyReflection(roundTrippedComponent.GetRawPtr(), deserializedComponent),
				"workspace component should be recreated and populated from serialized reflection");
			const YAML::Node roundTrippedYaml = roundTrippedComponent->GetReflectedData().Serialize();
			Require(roundTrippedYaml["typename"].as<std::string>() == FixtureTypeName &&
				roundTrippedYaml["overrideProperties"]["moveSpeed"].as<float>() == 12.5f,
				"range annotations must not clamp workspace component values at runtime");
			roundTrippedComponent.ForcelyDestroyObject();
		}
		Require(Reflection::GetCDO(FixtureTypeName).GetProperties()["moveSpeed"].as<float>() == 5.0f,
			"workspace registry should preserve metadata defaults");
		Require(!Reflection::GetCDO(FixtureTypeName).GetProperties().ContainsKey("skippedDefault"),
			"workspace CDO should omit properties marked SkipCDO");
		Require(Reflection::GetCDO(FixtureTypeName).GetProperties()["nullableComponent"].IsNull(),
			"workspace CDO should preserve a null object reference without decoding it");

		const YAML::Node engineTypesDuring = Reflection::ExportEngineTypes();
		Require(engineTypesDuring["engineTypes"].size() == engineTypesBefore["engineTypes"].size(),
			"workspace registration must not change engine-only type count");
		Require(!ContainsEngineType(engineTypesDuring, FixtureTypeName),
			"workspace registration must not leak into engine-only metadata");

		const std::filesystem::path collisionRoot = tempRoot / "collision";
		WriteManifest(collisionRoot, FixtureModuleName);
		InstallFixture(collisionRoot, config, FixtureModuleName, fixtureLibrary);
		WorkspaceModuleManager collisionManager;
		const auto& collision = LoadWorkspaceModule(collisionManager, collisionRoot, config);
		Require(collision.m_status == EWorkspaceModuleLoadStatus::RegistrationFailed,
			"second module with the same type should fail transactional preflight");
		Require(Reflection::GetNumWorkspaceTypes(result.m_moduleName + "@" + PathToUtf8(result.m_modulePath)) == 1,
			"failed collision must not remove the first module registration");

		component.ForcelyDestroyObject();
		Require(manager.Unload(), "workspace fixture should unload after all custom objects are destroyed");
		Require(Reflection::TryGetTypeByName(FixtureTypeName) == nullptr,
			"workspace type should be removed before its library closes");
		Require(Reflection::TryGetTypeByName("Sailor::Component") == engineComponentType,
			"workspace unload must preserve host engine type registration");
		YAML::Node editorTypesAfterUnload;
		Require(manager.BuildEditorTypeMetadata(engineTypesBefore, editorTypesAfterUnload, metadataError),
			"unloaded workspace should rebuild a fresh engine-only editor catalog: " + metadataError);
		Require(!ContainsEngineType(editorTypesAfterUnload, FixtureTypeName) &&
			editorTypesAfterUnload["engineTypes"].size() == engineTypesBefore["engineTypes"].size(),
			"workspace unload must remove custom entries from editor metadata");
		const auto& reload = LoadWorkspaceModule(collisionManager, collisionRoot, config);
		Require(reload.IsSuccess(), "workspace module should reload after owner cleanup: " + reload.m_message);
		Require(collisionManager.Unload(), "reloaded workspace fixture should unload cleanly");
		Require(!Reflection::IsWorkspaceTypeRegistered(FixtureTypeName),
			"workspace registry should be empty after reload cleanup");

		const YAML::Node engineTypesAfter = Reflection::ExportEngineTypes();
		Require(engineTypesAfter["engineTypes"].size() == engineTypesBefore["engineTypes"].size(),
			"engine-only reflection count should survive workspace unload");
	}
}

int main(int argc, char** argv)
{
	if (argc != 19 && argc != 20)
	{
		std::cerr << "Usage: WorkspaceModuleRuntimeTests <fixture> <incompatible> <missing-entry> <interface-mismatch> "
			"<reloaded> <12 invalid fixtures> <config> [previous-sdk-fixture]\n";
		return 1;
	}
	const auto fixtureLibrary = std::filesystem::absolute(argv[1]);
	const auto incompatibleLibrary = std::filesystem::absolute(argv[2]);
	const auto missingEntryLibrary = std::filesystem::absolute(argv[3]);
	const auto interfaceMismatchLibrary = std::filesystem::absolute(argv[4]);
	const auto reloadedLibrary = std::filesystem::absolute(argv[5]);
	std::vector<std::filesystem::path> invalidLibraries;
	for (int i = 6; i < 18; ++i) invalidLibraries.push_back(std::filesystem::absolute(argv[i]));
	const std::string config = argv[18];
	const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
		PathFromUtf8("sailor-workspace-module-runtime-" + std::to_string(uniqueSuffix) +
			reinterpret_cast<const char*>(u8" \u042f \u00e9 \u8239 \U0001f6a2"));

	try
	{
		TestDynamicLibraryFailures(tempRoot, missingEntryLibrary);
		TestDiscoveryFailures(tempRoot, config);
		TestFailedOwnerClaimPreservesExistingRegistration(tempRoot, fixtureLibrary, config);
		TestWorkspaceCdoAddressStability();
		TestUnknownReflectedType();
		TestWorldInstantiationGuards();
		TestIncompatibleModule(tempRoot, incompatibleLibrary, config);
		TestInterfaceMismatch(tempRoot, interfaceMismatchLibrary, fixtureLibrary, config);
		if (argc == 20) TestPreviousSdkModule(tempRoot, std::filesystem::absolute(argv[19]), fixtureLibrary, config);
		TestChangedTypeCatalogReload(tempRoot, fixtureLibrary, reloadedLibrary, config);
		TestMissingEntryPoint(tempRoot, missingEntryLibrary, config);
		TestModuleIdentityMismatch(tempRoot, fixtureLibrary, config);
		TestInvalidModules(tempRoot, fixtureLibrary, invalidLibraries, config);
		TestRegistrationInstantiationAndCleanup(tempRoot, fixtureLibrary, config);
		std::filesystem::remove_all(tempRoot);
		std::cout << "[PASS] Workspace module runtime contract" << std::endl;
		return 0;
	}
	catch (const std::exception& e)
	{
		std::filesystem::remove_all(tempRoot);
		std::cerr << "[FAIL] Workspace module runtime contract: " << e.what() << std::endl;
		return 1;
	}
}
