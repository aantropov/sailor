#include "Workspace/WorkspaceModuleManager.h"
#include "Containers/Containers.h"

#include "Components/Component.h"
#include "Core/Reflection.h"
#include "Core/ReflectionMetadata.h"
#include "Core/YamlUtils.h"
#include "ECS/ECSAutoRegistration.h"
#include "Workspace/WorkspaceModuleApi.h"
#include "Workspace/WorkspacePathEncoding.h"

#include <cstring>
#include <utility>

namespace
{
	using namespace Sailor;
	using namespace Sailor::Workspace;

	constexpr uint64_t MaxApiStringLength = 4096;

	struct CollectedWorkspaceType
	{
		const TypeInfo* m_typeInfo{};
		uint64_t m_typeSize = 0;
		uint64_t m_typeAlignment = 0;
		TWorkspacePlacementFactoryV1 m_placementFactory{};
	};

	struct WorkspaceTypeCollector
	{
		TVector<CollectedWorkspaceType> m_types;
		std::string m_error;
	};

	uint32_t SAILOR_WORKSPACE_CALL CollectWorkspaceType(
		void* context, const WorkspaceTypeDescriptorV1* descriptor) noexcept
	{
		if (!context || !descriptor)
		{
			return static_cast<uint32_t>(EWorkspaceModuleResult::InvalidArgument);
		}
		auto& collector = *static_cast<WorkspaceTypeCollector*>(context);
		if (descriptor->structSize < sizeof(WorkspaceTypeDescriptorV1) || !descriptor->typeInfo ||
			!descriptor->placementFactory || descriptor->typeSize == 0 || descriptor->typeAlignment == 0 ||
			(descriptor->typeAlignment & (descriptor->typeAlignment - 1)) != 0)
		{
			collector.m_error = "Workspace module returned an invalid type descriptor.";
			return static_cast<uint32_t>(EWorkspaceModuleResult::RegistrationFailed);
		}
		const auto* type = static_cast<const TypeInfo*>(descriptor->typeInfo);
		if (type->Name().empty() || type->Name().size() > MaxApiStringLength || type->Base().size() > MaxApiStringLength)
		{
			collector.m_error = "Workspace module returned an invalid reflected type name.";
			return static_cast<uint32_t>(EWorkspaceModuleResult::RegistrationFailed);
		}
		collector.m_types.Add({ type, descriptor->typeSize, descriptor->typeAlignment, descriptor->placementFactory });
		return static_cast<uint32_t>(EWorkspaceModuleResult::Success);
	}

	std::filesystem::path GetModuleFilename(const std::string& moduleName)
	{
#if defined(_WIN32)
		return moduleName + ".dll";
#elif defined(__APPLE__)
		return "lib" + moduleName + ".dylib";
#else
		return "lib" + moduleName + ".so";
#endif
	}

	using CollectedTypeInfos = TMap<std::string, const TypeInfo*>;

	bool ValidateComponentHierarchy(
		const TypeInfo& typeInfo,
		const CollectedTypeInfos& collectedTypes,
		std::string& outError)
	{
		TSet<std::string> visitedTypes;
		const TypeInfo* currentType = &typeInfo;
		while (currentType != nullptr)
		{
			const std::string& currentTypeName = currentType->Name();
			if (!visitedTypes.Insert(currentTypeName))
			{
				outError = "Workspace type '" + typeInfo.Name() +
					"' contains a cycle in its reflected base hierarchy.";
				return false;
			}

			if (currentTypeName == "Sailor::Component")
			{
				return true;
			}

			const std::string& baseTypeName = currentType->Base();
			if (baseTypeName.empty())
			{
				break;
			}

			const auto collectedBase = collectedTypes.Find(baseTypeName);
			if (collectedBase != collectedTypes.end())
			{
				currentType = collectedBase.Value();
			}
			else
			{
				currentType = Reflection::IsWorkspaceTypeRegistered(baseTypeName)
					? nullptr
					: Reflection::TryGetTypeByName(baseTypeName);
			}
			if (currentType != nullptr && currentType->Name() != baseTypeName)
			{
				currentType = nullptr;
			}
		}

		outError = "Workspace type '" + typeInfo.Name() +
			"' does not resolve to Sailor::Component through registered or collected bases.";
		return false;
	}

}

Sailor::Workspace::WorkspaceModuleManager::WorkspaceModuleManager() noexcept = default;

Sailor::Workspace::WorkspaceModuleManager::~WorkspaceModuleManager() noexcept
{
	Unload();
}

const Sailor::Workspace::WorkspaceModuleLoadResult& Sailor::Workspace::WorkspaceModuleManager::Load(
	const WorkspaceContext& context,
	std::string buildConfig) noexcept
{
	if (!Unload())
	{
		return m_result;
	}

	m_result = {};
	m_state = EWorkspaceModuleState::NotConfigured;
	m_result.m_buildConfig = std::move(buildConfig);
	m_result.m_manifestPath = context.GetManifest();
	m_result.m_moduleName = context.GetModuleName();

	if (context.IsLegacy())
	{
		m_result.m_status = EWorkspaceModuleLoadStatus::NotConfigured;
		m_result.m_message = "No workspace manifest is active; runtime will use engine-only types.";
		return m_result;
	}

	std::error_code pathError;
	const std::filesystem::path root = context.GetRoot();
	if (root.empty() || !std::filesystem::is_directory(root, pathError) || pathError)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::WorkspaceInvalid,
			"Workspace module activation requires a valid resolved workspace context.");
	}
	const std::string& moduleName = context.GetModuleName();

	if (m_result.m_buildConfig.empty() ||
		m_result.m_buildConfig == "." ||
		m_result.m_buildConfig == ".." ||
		m_result.m_buildConfig.find_first_of("/\\") != std::string::npos)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::ManifestInvalid,
			"Workspace module build configuration is invalid: '" + m_result.m_buildConfig + "'.");
	}

	pathError.clear();
	const std::filesystem::path modulePath = std::filesystem::weakly_canonical(
		context.GetLogicOutput() / PathFromUtf8(m_result.m_buildConfig) / GetModuleFilename(moduleName),
		pathError);
	m_result.m_modulePath = modulePath;
	if (pathError || !IsPathWithin(root, modulePath))
	{
		return Fail(
			EWorkspaceModuleLoadStatus::ManifestInvalid,
			"Workspace module path escapes the workspace: '" + PathToUtf8(modulePath) + "'.");
	}

	if (!std::filesystem::is_regular_file(modulePath))
	{
		return Fail(
			EWorkspaceModuleLoadStatus::ModuleNotFound,
			"Workspace module for configuration '" + m_result.m_buildConfig +
			"' was not found at '" + PathToUtf8(modulePath) + "'. Build the workspace logic project first.");
	}

	pathError.clear();
	const std::filesystem::path loadPath = std::filesystem::canonical(modulePath, pathError);
	if (pathError || !IsPathWithin(root, loadPath) ||
		!std::filesystem::is_regular_file(loadPath, pathError) || pathError)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::ManifestInvalid,
			"Workspace module path changed or escaped the workspace before loading: '" +
				PathToUtf8(modulePath) + "'.");
	}
	m_result.m_modulePath = loadPath;

	Reflection::SetEngineAutoRegistrationSuppressed(true);
	ECS::SetAutoRegistrationSuppressed(true);
	const bool bLibraryOpened = m_library.Open(loadPath);
	ECS::SetAutoRegistrationSuppressed(false);
	Reflection::SetEngineAutoRegistrationSuppressed(false);
	if (!bLibraryOpened)
	{
		return Fail(EWorkspaceModuleLoadStatus::NativeLoadFailed, m_library.GetError());
	}
	m_state = EWorkspaceModuleState::Loaded;

	void* getModuleApiSymbol = m_library.GetSymbol(WorkspaceModuleApiEntryPointV1);
	if (getModuleApiSymbol == nullptr)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::EntryPointMissing,
			m_library.GetError() + " Expected entry point '" + WorkspaceModuleApiEntryPointV1 + "'.");
	}
	static_assert(sizeof(TGetWorkspaceModuleApiV1) == sizeof(getModuleApiSymbol));
	TGetWorkspaceModuleApiV1 getModuleApi = nullptr;
	std::memcpy(&getModuleApi, &getModuleApiSymbol, sizeof(getModuleApi));

	const WorkspaceModuleApiV1* moduleApi = getModuleApi();
	if (moduleApi == nullptr ||
		moduleApi->structSize < sizeof(WorkspaceModuleApiV1) ||
		moduleApi->apiVersion != WorkspaceModuleApiVersion ||
		moduleApi->moduleName == nullptr ||
		moduleApi->moduleNameLength == 0 ||
		moduleApi->moduleNameLength > MaxApiStringLength ||
		moduleApi->abiTag == nullptr ||
		moduleApi->abiTagLength == 0 ||
		moduleApi->abiTagLength > MaxApiStringLength ||
		moduleApi->registerTypes == nullptr)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::ApiInvalid,
			"Workspace module '" + PathToUtf8(modulePath) + "' returned an invalid V1 API table.");
	}

	const std::string apiModuleName(moduleApi->moduleName, static_cast<size_t>(moduleApi->moduleNameLength));
	if (apiModuleName != moduleName)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::ApiInvalid,
			"Workspace module identity mismatch: manifest expects '" + moduleName +
			"', but the loaded module reports '" + apiModuleName + "'.");
	}

	if (moduleApi->abiTagLength != GetWorkspaceModuleAbiTagV1Length() ||
		std::memcmp(moduleApi->abiTag, GetWorkspaceModuleAbiTagV1(),
			static_cast<size_t>(moduleApi->abiTagLength)) != 0)
	{
		const std::string actualAbi(moduleApi->abiTag, static_cast<size_t>(moduleApi->abiTagLength));
		return Fail(
			EWorkspaceModuleLoadStatus::AbiMismatch,
			"Workspace module ABI mismatch for '" + PathToUtf8(modulePath) + "'. Expected '" +
			GetWorkspaceModuleAbiTagV1() + "', received '" + actualAbi + "'. Rebuild the module with this engine configuration.");
	}

	WorkspaceTypeCollector collector;
	WorkspaceHostApiV1 hostApi{};
	hostApi.structSize = static_cast<uint32_t>(sizeof(WorkspaceHostApiV1));
	hostApi.apiVersion = WorkspaceHostApiVersion;
	hostApi.context = &collector;
	hostApi.collectType = &CollectWorkspaceType;

	const auto registrationResult = static_cast<EWorkspaceModuleResult>(moduleApi->registerTypes(&hostApi));
	if (registrationResult != EWorkspaceModuleResult::Success)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::RegistrationFailed,
			collector.m_error.empty()
				? "Workspace module failed to enumerate its reflected types."
				: collector.m_error);
	}

	if (!collector.m_error.empty())
	{
		return Fail(EWorkspaceModuleLoadStatus::RegistrationFailed, collector.m_error);
	}

	TMap<std::string, const TypeInfo*> collectedTypeInfos;
	TVector<const TypeInfo*> types;
	TSet<std::string> typeNames;
	std::string metadataError;
	for (const auto& collected : collector.m_types)
	{
		const auto& type = *collected.m_typeInfo;
		if (type.Size() != collected.m_typeSize || type.Alignment() != collected.m_typeAlignment ||
			Reflection::TryGetTypeByName(type.Name()) || !collectedTypeInfos.Insert(type.Name(), &type))
		{
			return Fail(EWorkspaceModuleLoadStatus::RegistrationFailed,
				"Workspace type descriptor for '" + type.Name() + "' is incompatible or conflicts with an existing type.");
		}
		types.Add(&type);
		typeNames.Insert(type.Name());
	}
	for (const auto* type : types)
	{
		if (type->HasAmbiguousProperties())
		{
			return Fail(EWorkspaceModuleLoadStatus::MetadataInvalid,
				"Workspace type '" + type->Name() + "' contains ambiguous or shadowed reflected property names.");
		}
		if (!ValidateComponentHierarchy(*type, collectedTypeInfos, metadataError))
		{
			return Fail(EWorkspaceModuleLoadStatus::MetadataInvalid, std::move(metadataError));
		}
		if (!type->GetDefaultValues())
		{
			return Fail(EWorkspaceModuleLoadStatus::MetadataInvalid,
				"Workspace type '" + type->Name() + "' has no reflected default object.");
		}
	}

	auto preparedMetadata = TUniquePtr<ReflectedTypeCatalog>::Make();
	YAML::Node metadata = Reflection::ExportTypes(types);
	metadata["moduleName"] = moduleName;
	metadata["metadataVersion"] = WorkspaceTypeMetadataVersion;
	YAML::Node editorMetadata;
	if (!Reflection::PrepareTypeCatalog(std::move(metadata), std::move(typeNames), *preparedMetadata, metadataError) ||
		!Reflection::MergeTypeMetadata(Reflection::ExportEngineTypes(), *preparedMetadata, editorMetadata, metadataError) ||
		!External::TryDumpYaml(preparedMetadata->m_metadata, m_metadata, metadataError))
	{
		return Fail(EWorkspaceModuleLoadStatus::MetadataInvalid, std::move(metadataError));
	}

	// Cache identity follows the schema and defaults, not the time of export.
	YAML::Node catalogIdentity = YAML::Clone(preparedMetadata->m_metadata);
	catalogIdentity.remove("timeStamp");
	std::string canonicalMetadata;
	if (!Utils::CanonicalizeYaml(catalogIdentity, canonicalMetadata, Utils::EYamlCanonicalizationMode::SemanticValue))
	{
		return Fail(EWorkspaceModuleLoadStatus::MetadataInvalid, "Cannot canonicalize workspace type metadata.");
	}

	TVector<Reflection::WorkspaceTypeRegistration> registrations;
	registrations.Reserve(collector.m_types.Num());
	for (const auto& collected : collector.m_types)
	{
		Reflection::WorkspaceTypeRegistration registration;
		registration.m_typeInfo = collected.m_typeInfo;
		registration.m_alignment = static_cast<size_t>(collected.m_typeAlignment);
		registration.m_defaultObject = Reflection::CreateReflectedData(*collected.m_typeInfo,
			YAML::Clone(*collected.m_typeInfo->GetDefaultValues()));
		const TWorkspacePlacementFactoryV1 placementFactory = collected.m_placementFactory;
		registration.m_placementFactory = [placementFactory](void* destination) -> IReflectable*
		{
			return placementFactory(destination) ? static_cast<Component*>(destination) : nullptr;
		};
		registrations.Add(std::move(registration));
	}

	std::string candidateOwner = moduleName + "@" + PathToUtf8(modulePath);
	std::string registrationError;
	if (!Reflection::RegisterWorkspaceTypes(candidateOwner, std::move(registrations), registrationError))
	{
		return Fail(EWorkspaceModuleLoadStatus::RegistrationFailed, std::move(registrationError));
	}
	m_owner.swap(candidateOwner);
	m_editorMetadata = std::move(preparedMetadata);
	m_typeCatalogHash = HashString(canonicalMetadata);

	m_state = EWorkspaceModuleState::Registered;
	m_result.m_status = EWorkspaceModuleLoadStatus::Success;
	m_result.m_numRegisteredTypes = collector.m_types.Num();
	m_result.m_message = "Loaded workspace module '" + moduleName + "' from '" +
		PathToUtf8(modulePath) + "' with " + std::to_string(collector.m_types.Num()) +
		" reflected type(s).";
	return m_result;
}

bool Sailor::Workspace::WorkspaceModuleManager::BuildEditorTypeMetadata(
	const YAML::Node& engineMetadata,
	YAML::Node& outMetadata,
	std::string& outError) const noexcept
{
	if (!IsRegistered())
	{
		YAML::Node engineOnly = YAML::Clone(engineMetadata);
		outMetadata = std::move(engineOnly);
		outError.clear();
		return true;
	}

	return Reflection::MergeTypeMetadata(engineMetadata, *m_editorMetadata, outMetadata, outError);
}

bool Sailor::Workspace::WorkspaceModuleManager::Unload() noexcept
{
	if (!m_owner.empty())
	{
		Reflection::UnregisterWorkspaceTypes(m_owner);
		m_owner.clear();
	}

	m_metadata.clear();
	m_typeCatalogHash = 0;
	m_editorMetadata.Clear();
	if (!m_library.Close())
	{
		m_state = EWorkspaceModuleState::Failed;
		m_result.m_status = EWorkspaceModuleLoadStatus::NativeUnloadFailed;
		m_result.m_message = m_library.GetError();
		return false;
	}

	if (m_state != EWorkspaceModuleState::NotConfigured)
	{
		m_state = EWorkspaceModuleState::Unloaded;
	}
	return true;
}

const Sailor::Workspace::WorkspaceModuleLoadResult& Sailor::Workspace::WorkspaceModuleManager::Fail(
	EWorkspaceModuleLoadStatus status,
	std::string message) noexcept
{
	if (!m_owner.empty())
	{
		Reflection::UnregisterWorkspaceTypes(m_owner);
		m_owner.clear();
	}

	m_editorMetadata.Clear();
	if (m_library.IsOpen() && !m_library.Close())
	{
		message += " Additionally, the module could not be unloaded: " + m_library.GetError();
	}

	m_metadata.clear();
	m_typeCatalogHash = 0;
	m_state = EWorkspaceModuleState::Failed;
	m_result.m_status = status;
	m_result.m_message = std::move(message);
	m_result.m_numRegisteredTypes = 0;
	return m_result;
}
