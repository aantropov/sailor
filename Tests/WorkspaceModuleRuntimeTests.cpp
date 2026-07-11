#include "Components/Component.h"
#include "Core/Reflection.h"
#include "Engine/EngineLoop.h"
#include "Memory/ObjectAllocator.hpp"
#include "Platform/DynamicLibrary.h"
#include "Workspace/WorkspaceModuleApi.h"
#include "Workspace/WorkspaceModuleManager.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <yaml-cpp/yaml.h>

using namespace Sailor;
using namespace Sailor::Workspace;

namespace
{
	constexpr const char* FixtureModuleName = "WorkspaceFixture";
	constexpr const char* FixtureTypeName = "WorkspaceFixture::FixtureComponent";

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
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

	void WriteManifest(const std::filesystem::path& root, const std::string& moduleName)
	{
		std::filesystem::create_directories(root);
		std::ofstream manifest(root / "workspace.sailor");
		manifest
			<< "manifestVersion: 1\n"
			<< "logicOutputPath: Binaries\n"
			<< "logicModuleName: " << moduleName << "\n";
		Require(manifest.good(), "workspace fixture manifest should be writable");
	}

	std::filesystem::path InstallFixture(
		const std::filesystem::path& root,
		const std::string& config,
		const std::string& moduleName,
		const std::filesystem::path& sourceLibrary)
	{
		const std::filesystem::path destination = root / "Binaries" / config / ModuleFilename(moduleName);
		std::filesystem::create_directories(destination.parent_path());
		std::filesystem::copy_file(
			sourceLibrary,
			destination,
			std::filesystem::copy_options::overwrite_existing);
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

	void TestDiscoveryFailures(const std::filesystem::path& tempRoot, const std::string& config)
	{
		const std::filesystem::path legacyRoot = tempRoot / "legacy";
		std::filesystem::create_directories(legacyRoot);
		WorkspaceModuleManager legacyManager;
		const auto& legacy = legacyManager.Load(legacyRoot, {}, config);
		Require(legacy.m_status == EWorkspaceModuleLoadStatus::NotConfigured,
			"workspace without a manifest should preserve engine-only startup");

		const std::filesystem::path missingRoot = tempRoot / "missing";
		WriteManifest(missingRoot, FixtureModuleName);
		WorkspaceModuleManager missingManager;
		const auto& missing = missingManager.Load(missingRoot, {}, config);
		Require(missing.m_status == EWorkspaceModuleLoadStatus::ModuleNotFound,
			"missing workspace binary should return ModuleNotFound");
		Require(missing.m_message.find(config) != std::string::npos,
			"missing-module diagnostic should identify the active configuration");

		const std::filesystem::path ambiguousRoot = tempRoot / "ambiguous";
		std::filesystem::create_directories(ambiguousRoot);
		std::ofstream(ambiguousRoot / "one.sailor") << "manifestVersion: 1\n";
		std::ofstream(ambiguousRoot / "two.sailor") << "manifestVersion: 1\n";
		WorkspaceModuleManager ambiguousManager;
		const auto& ambiguous = ambiguousManager.Load(ambiguousRoot, {}, config);
		Require(ambiguous.m_status == EWorkspaceModuleLoadStatus::ManifestAmbiguous,
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

	void TestIncompatibleModule(
		const std::filesystem::path& tempRoot,
		const std::filesystem::path& incompatibleLibrary,
		const std::string& config)
	{
		const std::filesystem::path root = tempRoot / "incompatible";
		WriteManifest(root, "IncompatibleFixture");
		InstallFixture(root, config, "IncompatibleFixture", incompatibleLibrary);

		WorkspaceModuleManager manager;
		const auto& result = manager.Load(root, {}, config);
		Require(result.m_status == EWorkspaceModuleLoadStatus::AbiMismatch,
			"incompatible module should fail before metadata or registration callbacks");
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
		const auto& result = manager.Load(root, {}, config);
		Require(result.m_status == EWorkspaceModuleLoadStatus::EntryPointMissing,
			"module without the V1 API symbol should return EntryPointMissing");
		Require(result.m_message.find(WorkspaceModuleApiEntryPointV1) != std::string::npos,
			"missing-entry diagnostic should name the required symbol");
		Require(!manager.IsRegistered(), "missing entry point must not leave registrations behind");
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
		EngineLoop engineLoop;
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
		const auto& result = manager.Load(root, {}, config);
		Require(result.m_status == EWorkspaceModuleLoadStatus::ApiInvalid,
			"renaming a module must not bypass its declared module identity");
		Require(!manager.IsRegistered(), "identity mismatch must not leave registrations behind");
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

		const std::filesystem::path root = tempRoot / "registered";
		WriteManifest(root, FixtureModuleName);
		InstallFixture(root, config, FixtureModuleName, fixtureLibrary);

		WorkspaceModuleManager manager;
		const auto& result = manager.Load(root, {}, config);
		Require(result.IsSuccess(), "workspace fixture should load and register: " + result.m_message);
		Require(result.m_numRegisteredTypes == 1, "workspace fixture should register one type");
		Require(Reflection::IsWorkspaceTypeRegistered(FixtureTypeName),
			"workspace fixture type should be visible in unified reflection lookup");
		Require(Reflection::TryGetTypeByName("Sailor::Component") == engineComponentType,
			"loading a workspace DLL must not replace host engine type registration");

		const TypeInfo* fixtureType = Reflection::TryGetTypeByName(FixtureTypeName);
		Require(fixtureType != nullptr, "workspace fixture TypeInfo should be discoverable by name");
		auto allocator = Memory::ObjectAllocatorPtr::Make(Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		ComponentPtr component = Reflection::CreateObject<Component>(*fixtureType, allocator);
		Require(static_cast<bool>(component), "workspace placement factory should instantiate the custom component");
		{
			const ReflectedData reflected = component->GetReflectedData();
			Require(reflected.GetProperties()["moveSpeed"].as<float>() == 5.0f,
				"instantiated workspace component should expose its reflected default value");
		}
		Require(Reflection::GetCDO(FixtureTypeName).GetProperties()["moveSpeed"].as<float>() == 5.0f,
			"workspace registry should preserve metadata defaults");

		const YAML::Node engineTypesDuring = Reflection::ExportEngineTypes();
		Require(engineTypesDuring["engineTypes"].size() == engineTypesBefore["engineTypes"].size(),
			"workspace registration must not change engine-only type count");
		Require(!ContainsEngineType(engineTypesDuring, FixtureTypeName),
			"workspace registration must not leak into engine-only metadata");

		const std::filesystem::path collisionRoot = tempRoot / "collision";
		WriteManifest(collisionRoot, FixtureModuleName);
		InstallFixture(collisionRoot, config, FixtureModuleName, fixtureLibrary);
		WorkspaceModuleManager collisionManager;
		const auto& collision = collisionManager.Load(collisionRoot, {}, config);
		Require(collision.m_status == EWorkspaceModuleLoadStatus::RegistrationFailed,
			"second module with the same type should fail transactional preflight");
		Require(Reflection::GetNumWorkspaceTypes(result.m_moduleName + "@" + result.m_modulePath.generic_string()) == 1,
			"failed collision must not remove the first module registration");

		component.ForcelyDestroyObject();
		Require(manager.Unload(), "workspace fixture should unload after all custom objects are destroyed");
		Require(Reflection::TryGetTypeByName(FixtureTypeName) == nullptr,
			"workspace type should be removed before its library closes");
		Require(Reflection::TryGetTypeByName("Sailor::Component") == engineComponentType,
			"workspace unload must preserve host engine type registration");
		const auto& reload = collisionManager.Load(collisionRoot, {}, config);
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
	if (argc != 5)
	{
		std::cerr << "Usage: WorkspaceModuleRuntimeTests <fixture> <incompatible-fixture> "
			"<missing-entry-fixture> <config>" << std::endl;
		return 1;
	}

	const std::filesystem::path fixtureLibrary = std::filesystem::absolute(argv[1]);
	const std::filesystem::path incompatibleLibrary = std::filesystem::absolute(argv[2]);
	const std::filesystem::path missingEntryLibrary = std::filesystem::absolute(argv[3]);
	const std::string config = argv[4];
	const auto uniqueSuffix = std::chrono::steady_clock::now().time_since_epoch().count();
	const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() /
		("sailor-workspace-module-runtime-" + std::to_string(uniqueSuffix));

	try
	{
		TestDynamicLibraryFailures(tempRoot, missingEntryLibrary);
		TestDiscoveryFailures(tempRoot, config);
		TestUnknownReflectedType();
		TestWorldInstantiationGuards();
		TestIncompatibleModule(tempRoot, incompatibleLibrary, config);
		TestMissingEntryPoint(tempRoot, missingEntryLibrary, config);
		TestModuleIdentityMismatch(tempRoot, fixtureLibrary, config);
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
