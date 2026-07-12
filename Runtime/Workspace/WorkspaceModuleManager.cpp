#include "Workspace/WorkspaceModuleManager.h"

#include "Components/Component.h"
#include "Core/Reflection.h"
#include "Workspace/WorkspaceModuleApi.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

namespace
{
	using namespace Sailor;
	using namespace Sailor::Workspace;

	constexpr uint64_t MaxApiStringLength = 4096;
	constexpr uint64_t MaxMetadataPayloadSize = 16 * 1024 * 1024;

	struct CollectedWorkspaceType
	{
		std::string m_typeName;
		std::string m_baseTypeName;
		const TypeInfo* m_typeInfo{};
		uint64_t m_typeSize = 0;
		uint64_t m_typeAlignment = 0;
		std::string m_canonicalDefaultValues;
		uint32_t m_flags = 0;
		TWorkspacePlacementFactoryV1 m_placementFactory{};
	};

	struct WorkspaceTypeCollector
	{
		std::vector<CollectedWorkspaceType> m_types;
		std::string m_error;
		uint64_t m_totalCanonicalDefaultValuesLength = 0;
	};

	uint32_t SAILOR_WORKSPACE_CALL CollectWorkspaceType(
		void* context,
		const WorkspaceTypeDescriptorV1* descriptor) noexcept
	{
		if (context == nullptr || descriptor == nullptr)
		{
			return static_cast<uint32_t>(EWorkspaceModuleResult::InvalidArgument);
		}

		auto& collector = *static_cast<WorkspaceTypeCollector*>(context);
		try
		{
			if (descriptor->structSize < sizeof(WorkspaceTypeDescriptorV1) ||
				descriptor->typeName == nullptr ||
				descriptor->typeNameLength == 0 ||
				descriptor->typeNameLength > MaxApiStringLength ||
				descriptor->baseTypeName == nullptr ||
				descriptor->baseTypeNameLength > MaxApiStringLength ||
				descriptor->typeInfo == nullptr ||
				descriptor->typeSize == 0 ||
				descriptor->typeAlignment == 0 ||
				(descriptor->typeAlignment & (descriptor->typeAlignment - 1)) != 0 ||
				descriptor->canonicalDefaultValues == nullptr ||
				descriptor->canonicalDefaultValuesLength == 0 ||
				descriptor->canonicalDefaultValuesLength > MaxMetadataPayloadSize ||
				(descriptor->flags & ~WorkspaceTypeDescriptorKnownFlags) != 0 ||
				descriptor->placementFactory == nullptr)
			{
				collector.m_error = "Workspace module returned an invalid type descriptor.";
				return static_cast<uint32_t>(EWorkspaceModuleResult::RegistrationFailed);
			}
			if (descriptor->canonicalDefaultValuesLength >
				MaxMetadataPayloadSize - collector.m_totalCanonicalDefaultValuesLength)
			{
				collector.m_error = "Workspace module returned too much canonical default data.";
				return static_cast<uint32_t>(EWorkspaceModuleResult::RegistrationFailed);
			}

			CollectedWorkspaceType type;
			type.m_typeName.assign(descriptor->typeName, static_cast<size_t>(descriptor->typeNameLength));
			type.m_baseTypeName.assign(
				descriptor->baseTypeName,
				static_cast<size_t>(descriptor->baseTypeNameLength));
			type.m_typeInfo = static_cast<const TypeInfo*>(descriptor->typeInfo);
			type.m_typeSize = descriptor->typeSize;
			type.m_typeAlignment = descriptor->typeAlignment;
			type.m_canonicalDefaultValues.assign(
				descriptor->canonicalDefaultValues,
				static_cast<size_t>(descriptor->canonicalDefaultValuesLength));
			type.m_flags = descriptor->flags;
			type.m_placementFactory = descriptor->placementFactory;
			collector.m_types.emplace_back(std::move(type));
			collector.m_totalCanonicalDefaultValuesLength +=
				descriptor->canonicalDefaultValuesLength;
			return static_cast<uint32_t>(EWorkspaceModuleResult::Success);
		}
		catch (const std::exception& e)
		{
			try
			{
				collector.m_error = "Failed to collect workspace type descriptor: " + std::string(e.what());
			}
			catch (...)
			{
				collector.m_error.clear();
			}
			return static_cast<uint32_t>(EWorkspaceModuleResult::RegistrationFailed);
		}
		catch (...)
		{
			collector.m_error = "Failed to collect workspace type descriptor.";
			return static_cast<uint32_t>(EWorkspaceModuleResult::RegistrationFailed);
		}
	}

	bool IsCIdentifier(const std::string& value)
	{
		if (value.empty())
		{
			return false;
		}

		auto isIdentifierStart = [](char character)
		{
			return character == '_' ||
				(character >= 'A' && character <= 'Z') ||
				(character >= 'a' && character <= 'z');
		};

		if (!isIdentifierStart(value.front()))
		{
			return false;
		}

		return std::all_of(value.begin() + 1, value.end(), [&](char character)
			{
				return isIdentifierStart(character) || (character >= '0' && character <= '9');
			});
	}

	bool IsSafeRelativePath(const std::filesystem::path& path)
	{
		if (path.empty() || path.is_absolute())
		{
			return false;
		}

		for (const std::filesystem::path& component : path)
		{
			if (component == "..")
			{
				return false;
			}
		}

		return true;
	}

	bool IsInside(const std::filesystem::path& root, const std::filesystem::path& candidate)
	{
		const std::filesystem::path relative = candidate.lexically_relative(root);
		if (relative.empty() || relative.is_absolute())
		{
			return false;
		}

		const auto first = relative.begin();
		return first != relative.end() && *first != "..";
	}

	bool HasSailorExtension(const std::filesystem::path& path)
	{
		std::string extension = path.extension().string();
		std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char character)
			{
				return static_cast<char>(std::tolower(character));
			});
		return extension == ".sailor";
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

	using MetadataEntries = std::unordered_map<std::string, YAML::Node>;
	using CollectedTypeInfos = std::unordered_map<std::string, const TypeInfo*>;
	constexpr size_t MaxCanonicalYamlDepth = 64;
	constexpr size_t MaxCanonicalYamlNodes = 262144;
	constexpr size_t MaxCanonicalYamlBytes = 64 * 1024 * 1024;

	bool IndexMetadataEntries(
		const YAML::Node& entries,
		const char* sectionName,
		MetadataEntries& outEntries,
		std::string& outError)
	{
		outEntries.clear();
		for (const YAML::Node& entry : entries)
		{
			if (!entry.IsMap() || !entry["typename"] || !entry["typename"].IsScalar())
			{
				outError = std::string("Workspace metadata section '") + sectionName +
					"' contains an entry without a scalar typename.";
				return false;
			}

			const std::string typeName = entry["typename"].as<std::string>();
			if (typeName.empty() || !outEntries.emplace(typeName, entry).second)
			{
				outError = std::string("Workspace metadata section '") + sectionName +
					"' contains duplicate or empty typename '" + typeName + "'.";
				return false;
			}
		}

		return true;
	}

	bool ValidatePropertySchema(
		const YAML::Node& metadataProperties,
		const TypeInfo& typeInfo,
		std::string& outError)
	{
		const auto& reflectedProperties = typeInfo.Properties();
		if (!metadataProperties.IsDefined())
		{
			outError = "Workspace metadata property schema for '" + typeInfo.Name() +
				"' is missing.";
			return false;
		}

		if (reflectedProperties.Num() == 0)
		{
			if (metadataProperties.IsNull())
			{
				return true;
			}
		}

		if (!metadataProperties.IsMap() || metadataProperties.size() != reflectedProperties.Num())
		{
			outError = "Workspace metadata property schema for '" + typeInfo.Name() +
				"' does not match its reflected TypeInfo.";
			return false;
		}

		std::unordered_set<std::string> propertyNames;
		for (const auto& property : metadataProperties)
		{
			if (!property.first.IsScalar() || !property.second.IsScalar())
			{
				outError = "Workspace metadata property schema for '" + typeInfo.Name() +
					"' must contain scalar names and type names.";
				return false;
			}

			const std::string propertyName = property.first.as<std::string>();
			const std::string propertyType = property.second.as<std::string>();
			if (!propertyNames.emplace(propertyName).second ||
				!reflectedProperties.ContainsKey(propertyName) ||
				reflectedProperties[propertyName] != propertyType)
			{
				outError = "Workspace metadata property '" + typeInfo.Name() + "::" +
					propertyName + "' does not match its reflected TypeInfo.";
				return false;
			}
		}

		return true;
	}

	bool ValidateComponentHierarchy(
		const TypeInfo& typeInfo,
		const CollectedTypeInfos& collectedTypes,
		std::string& outError)
	{
		std::unordered_set<std::string> visitedTypes;
		const TypeInfo* currentType = &typeInfo;
		while (currentType != nullptr)
		{
			const std::string& currentTypeName = currentType->Name();
			if (!visitedTypes.emplace(currentTypeName).second)
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

			const auto collectedBase = collectedTypes.find(baseTypeName);
			if (collectedBase != collectedTypes.end())
			{
				currentType = collectedBase->second;
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

	bool AppendCanonicalText(
		std::string& destination,
		const std::string& value,
		size_t& remainingBytes)
	{
		if (value.size() > remainingBytes)
		{
			return false;
		}

		destination.append(value);
		remainingBytes -= value.size();
		return true;
	}

	bool AppendCanonicalCharacter(
		std::string& destination,
		char value,
		size_t& remainingBytes)
	{
		if (remainingBytes == 0)
		{
			return false;
		}

		destination.push_back(value);
		--remainingBytes;
		return true;
	}

	bool AppendCanonicalField(
		std::string& destination,
		const std::string& value,
		size_t& remainingBytes)
	{
		const std::string length = std::to_string(value.size());
		return AppendCanonicalText(destination, length, remainingBytes) &&
			AppendCanonicalCharacter(destination, ':', remainingBytes) &&
			AppendCanonicalText(destination, value, remainingBytes);
	}

	bool AppendCanonicalYaml(
		const YAML::Node& node,
		std::string& destination,
		size_t depth,
		size_t& remainingNodes,
		size_t& remainingBytes)
	{
		if (depth > MaxCanonicalYamlDepth || remainingNodes == 0)
		{
			return false;
		}
		--remainingNodes;

		switch (node.Type())
		{
		case YAML::NodeType::Undefined:
			return AppendCanonicalCharacter(destination, 'U', remainingBytes);
		case YAML::NodeType::Null:
			return AppendCanonicalCharacter(destination, 'N', remainingBytes) &&
				AppendCanonicalField(destination, node.Tag(), remainingBytes);
		case YAML::NodeType::Scalar:
			return AppendCanonicalCharacter(destination, 'S', remainingBytes) &&
				AppendCanonicalField(destination, node.Tag(), remainingBytes) &&
				AppendCanonicalField(destination, node.Scalar(), remainingBytes);
		case YAML::NodeType::Sequence:
		{
			const std::string numElements = std::to_string(node.size());
			if (!AppendCanonicalCharacter(destination, 'Q', remainingBytes) ||
				!AppendCanonicalField(destination, node.Tag(), remainingBytes) ||
				!AppendCanonicalText(destination, numElements, remainingBytes) ||
				!AppendCanonicalCharacter(destination, ':', remainingBytes))
			{
				return false;
			}
			for (const YAML::Node& element : node)
			{
				std::string canonicalElement;
				if (!AppendCanonicalYaml(
					element,
					canonicalElement,
					depth + 1,
					remainingNodes,
					remainingBytes) ||
					!AppendCanonicalField(destination, canonicalElement, remainingBytes))
				{
					return false;
				}
			}
			return true;
		}
		case YAML::NodeType::Map:
		{
			std::vector<std::pair<std::string, std::string>> entries;
			entries.reserve(node.size());
			for (const auto& entry : node)
			{
				std::string canonicalKey;
				std::string canonicalValue;
				if (!AppendCanonicalYaml(
					entry.first,
					canonicalKey,
					depth + 1,
					remainingNodes,
					remainingBytes) ||
					!AppendCanonicalYaml(
						entry.second,
						canonicalValue,
						depth + 1,
						remainingNodes,
						remainingBytes))
				{
					return false;
				}
				entries.emplace_back(std::move(canonicalKey), std::move(canonicalValue));
			}

			std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs)
				{
					return lhs.first < rhs.first;
				});
			for (size_t index = 1; index < entries.size(); ++index)
			{
				if (entries[index - 1].first == entries[index].first)
				{
					return false;
				}
			}

			const std::string numEntries = std::to_string(entries.size());
			if (!AppendCanonicalCharacter(destination, 'M', remainingBytes) ||
				!AppendCanonicalField(destination, node.Tag(), remainingBytes) ||
				!AppendCanonicalText(destination, numEntries, remainingBytes) ||
				!AppendCanonicalCharacter(destination, ':', remainingBytes))
			{
				return false;
			}
			for (const auto& entry : entries)
			{
				if (!AppendCanonicalField(destination, entry.first, remainingBytes) ||
					!AppendCanonicalField(destination, entry.second, remainingBytes))
				{
					return false;
				}
			}
			return true;
		}
		}

		return false;
	}

	bool CanonicalizeYaml(const YAML::Node& node, std::string& destination)
	{
		destination.clear();
		size_t remainingNodes = MaxCanonicalYamlNodes;
		size_t remainingBytes = MaxCanonicalYamlBytes;
		return AppendCanonicalYaml(node, destination, 0, remainingNodes, remainingBytes);
	}

	bool HasUniqueScalarStringKeys(const YAML::Node& node)
	{
		if (!node.IsMap() || node.size() > MaxCanonicalYamlNodes)
		{
			return false;
		}

		std::unordered_set<std::string> keys;
		keys.reserve(node.size());
		for (const auto& entry : node)
		{
			if (!entry.first.IsScalar() || !keys.emplace(entry.first.Scalar()).second)
			{
				return false;
			}
		}

		return true;
	}

	bool ValidateCanonicalDefaultValues(
		const YAML::Node& defaultValues,
		const CollectedWorkspaceType& collected,
		std::string& outError)
	{
		const YAML::Node canonicalDefaultValues = YAML::Load(collected.m_canonicalDefaultValues);
		std::string canonicalMetadata;
		std::string canonicalDescriptor;
		if (!HasUniqueScalarStringKeys(defaultValues) ||
			!HasUniqueScalarStringKeys(canonicalDefaultValues) ||
			!CanonicalizeYaml(defaultValues, canonicalMetadata) ||
			!CanonicalizeYaml(canonicalDefaultValues, canonicalDescriptor) ||
			canonicalMetadata != canonicalDescriptor)
		{
			outError = "Workspace metadata defaults for '" + collected.m_typeName +
				"' do not match the canonical descriptor snapshot.";
			return false;
		}

		return true;
	}
}

Sailor::Workspace::WorkspaceModuleManager::~WorkspaceModuleManager() noexcept
{
	Unload();
}

const Sailor::Workspace::WorkspaceModuleLoadResult& Sailor::Workspace::WorkspaceModuleManager::Load(
	const std::filesystem::path& workspaceRoot,
	const std::filesystem::path& requestedManifestPath,
	std::string buildConfig) noexcept
{
	EWorkspaceModuleLoadStatus yamlFailureStatus = EWorkspaceModuleLoadStatus::ManifestInvalid;
	try
	{
		if (!Unload())
		{
			return m_result;
		}

		m_result = {};
		m_state = EWorkspaceModuleState::NotConfigured;
		m_result.m_buildConfig = std::move(buildConfig);

		std::error_code pathError;
		const std::filesystem::path root = std::filesystem::weakly_canonical(
			std::filesystem::absolute(workspaceRoot, pathError),
			pathError);
		if (pathError || root.empty() || !std::filesystem::is_directory(root))
		{
			return Fail(
				EWorkspaceModuleLoadStatus::WorkspaceInvalid,
				"Workspace module discovery requires an existing workspace directory: '" +
				workspaceRoot.generic_string() + "'.");
		}

		std::filesystem::path manifestPath;
		if (!requestedManifestPath.empty())
		{
			manifestPath = requestedManifestPath.is_absolute()
				? requestedManifestPath
				: root / requestedManifestPath;
			manifestPath = std::filesystem::weakly_canonical(manifestPath, pathError);
			if (pathError || !std::filesystem::is_regular_file(manifestPath) || !IsInside(root, manifestPath))
			{
				m_result.m_manifestPath = manifestPath;
				return Fail(
					EWorkspaceModuleLoadStatus::ManifestNotFound,
					"Workspace manifest was not found inside the workspace: '" +
					manifestPath.generic_string() + "'.");
			}
		}
		else
		{
			const std::filesystem::path defaultManifest = root / "workspace.sailor";
			if (std::filesystem::is_regular_file(defaultManifest))
			{
				manifestPath = defaultManifest;
			}
			else
			{
				std::vector<std::filesystem::path> manifests;
				for (std::filesystem::directory_iterator it(root, pathError), end; !pathError && it != end; it.increment(pathError))
				{
					if (it->is_regular_file() && HasSailorExtension(it->path()))
					{
						manifests.emplace_back(it->path());
					}
				}

				if (pathError)
				{
					return Fail(
						EWorkspaceModuleLoadStatus::WorkspaceInvalid,
						"Failed to enumerate workspace manifests in '" + root.generic_string() +
						"': " + pathError.message() + ".");
				}

				std::sort(manifests.begin(), manifests.end());
				if (manifests.empty())
				{
					m_result.m_status = EWorkspaceModuleLoadStatus::NotConfigured;
					m_result.m_message = "No workspace manifest was found; runtime will use engine-only types.";
					return m_result;
				}

				if (manifests.size() != 1)
				{
					return Fail(
						EWorkspaceModuleLoadStatus::ManifestAmbiguous,
						"Multiple .sailor manifests were found in '" + root.generic_string() +
						"'. Pass --workspace-manifest with the exact manifest path.");
				}

				manifestPath = manifests.front();
			}
		}

		pathError.clear();
		manifestPath = std::filesystem::weakly_canonical(manifestPath, pathError);
		if (pathError || !std::filesystem::is_regular_file(manifestPath) || !IsInside(root, manifestPath))
		{
			m_result.m_manifestPath = manifestPath;
			return Fail(
				EWorkspaceModuleLoadStatus::ManifestNotFound,
				"Workspace manifest was not found inside the workspace: '" +
				manifestPath.generic_string() + "'.");
		}

		m_result.m_manifestPath = manifestPath;
		const YAML::Node manifest = YAML::LoadFile(manifestPath.string());
		const uint32_t manifestVersion = manifest["manifestVersion"]
			? manifest["manifestVersion"].as<uint32_t>()
			: 0;
		if (manifestVersion != 1)
		{
			return Fail(
				EWorkspaceModuleLoadStatus::ManifestInvalid,
				"Workspace manifest '" + manifestPath.generic_string() +
				"' has unsupported manifestVersion '" + std::to_string(manifestVersion) + "'.");
		}

		const std::string logicOutputPath = manifest["logicOutputPath"]
			? manifest["logicOutputPath"].as<std::string>()
			: "Binaries";
		const std::string moduleName = manifest["logicModuleName"]
			? manifest["logicModuleName"].as<std::string>()
			: "SailorGame";
		m_result.m_moduleName = moduleName;

		if (!IsSafeRelativePath(logicOutputPath) || !IsCIdentifier(moduleName))
		{
			return Fail(
				EWorkspaceModuleLoadStatus::ManifestInvalid,
				"Workspace manifest '" + manifestPath.generic_string() +
				"' contains an unsafe logicOutputPath or logicModuleName.");
		}

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
			root / logicOutputPath / m_result.m_buildConfig / GetModuleFilename(moduleName),
			pathError);
		m_result.m_modulePath = modulePath;
		if (pathError || !IsInside(root, modulePath))
		{
			return Fail(
				EWorkspaceModuleLoadStatus::ManifestInvalid,
				"Workspace module path escapes the workspace: '" + modulePath.generic_string() + "'.");
		}

		if (!std::filesystem::is_regular_file(modulePath))
		{
			return Fail(
				EWorkspaceModuleLoadStatus::ModuleNotFound,
				"Workspace module for configuration '" + m_result.m_buildConfig +
				"' was not found at '" + modulePath.generic_string() + "'. Build the workspace logic project first.");
		}

		Reflection::SetEngineAutoRegistrationSuppressed(true);
		const bool bLibraryOpened = m_library.Open(modulePath);
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
			moduleApi->getMetadata == nullptr ||
			moduleApi->registerTypes == nullptr)
		{
			return Fail(
				EWorkspaceModuleLoadStatus::ApiInvalid,
				"Workspace module '" + modulePath.generic_string() + "' returned an invalid V1 API table.");
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
				"Workspace module ABI mismatch for '" + modulePath.generic_string() + "'. Expected '" +
				GetWorkspaceModuleAbiTagV1() + "', received '" + actualAbi + "'. Rebuild the module with this engine configuration.");
		}

		uint64_t metadataSize = 0;
		const auto queryResult = static_cast<EWorkspaceModuleResult>(
			moduleApi->getMetadata(nullptr, 0, &metadataSize));
		if (queryResult != EWorkspaceModuleResult::BufferTooSmall ||
			metadataSize == 0 || metadataSize > MaxMetadataPayloadSize)
		{
			return Fail(
				EWorkspaceModuleLoadStatus::MetadataInvalid,
				"Workspace module returned an invalid metadata size.");
		}

		m_metadata.assign(static_cast<size_t>(metadataSize), '\0');
		uint64_t writtenSize = 0;
		const auto metadataResult = static_cast<EWorkspaceModuleResult>(
			moduleApi->getMetadata(m_metadata.data(), metadataSize, &writtenSize));
		if (metadataResult != EWorkspaceModuleResult::Success || writtenSize != metadataSize)
		{
			return Fail(
				EWorkspaceModuleLoadStatus::MetadataInvalid,
				"Workspace module failed to write its V1 metadata payload.");
		}

		yamlFailureStatus = EWorkspaceModuleLoadStatus::MetadataInvalid;
		const YAML::Node metadata = YAML::Load(m_metadata);
		if (!metadata.IsMap() ||
			!metadata["metadataVersion"] ||
			metadata["metadataVersion"].as<uint32_t>() != WorkspaceTypeMetadataVersion ||
			!metadata["moduleName"] ||
			metadata["moduleName"].as<std::string>() != moduleName ||
			!metadata["engineTypes"].IsSequence() ||
			!metadata["cdos"].IsSequence())
		{
			return Fail(
				EWorkspaceModuleLoadStatus::MetadataInvalid,
				"Workspace module metadata schema or module identity is invalid.");
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

		MetadataEntries metadataTypes;
		MetadataEntries metadataDefaults;
		std::string metadataError;
		if (!IndexMetadataEntries(metadata["engineTypes"], "engineTypes", metadataTypes, metadataError) ||
			!IndexMetadataEntries(metadata["cdos"], "cdos", metadataDefaults, metadataError))
		{
			return Fail(EWorkspaceModuleLoadStatus::MetadataInvalid, std::move(metadataError));
		}

		if (metadataTypes.size() != collector.m_types.size() ||
			metadataDefaults.size() != collector.m_types.size())
		{
			return Fail(
				EWorkspaceModuleLoadStatus::MetadataInvalid,
				"Workspace module metadata must contain exactly one type and one CDO entry per registration descriptor.");
		}

		CollectedTypeInfos collectedTypeInfos;
		collectedTypeInfos.reserve(collector.m_types.size());
		for (const CollectedWorkspaceType& collected : collector.m_types)
		{
			if (collected.m_typeInfo == nullptr ||
				collected.m_typeInfo->Name() != collected.m_typeName ||
				collected.m_typeInfo->Base() != collected.m_baseTypeName ||
				collected.m_typeInfo->Size() != collected.m_typeSize ||
				collected.m_typeSize > std::numeric_limits<size_t>::max() ||
				collected.m_typeAlignment > std::numeric_limits<size_t>::max() ||
				Reflection::TryGetTypeByName(collected.m_typeName) != nullptr ||
				!collectedTypeInfos.emplace(collected.m_typeName, collected.m_typeInfo).second)
			{
				return Fail(
					EWorkspaceModuleLoadStatus::RegistrationFailed,
					"Workspace type descriptor for '" + collected.m_typeName +
					"' is incompatible or conflicts with an existing type.");
			}
		}

		std::vector<Reflection::WorkspaceTypeRegistration> registrations;
		registrations.reserve(collector.m_types.size());
		for (const CollectedWorkspaceType& collected : collector.m_types)
		{
			if ((collected.m_flags & WorkspaceTypeDescriptorFlagAmbiguousProperties) != 0)
			{
				return Fail(
					EWorkspaceModuleLoadStatus::MetadataInvalid,
					"Workspace type '" + collected.m_typeName +
					"' contains ambiguous or shadowed reflected property names.");
			}

			const auto metadataTypeIt = metadataTypes.find(collected.m_typeName);
			const auto metadataDefaultsIt = metadataDefaults.find(collected.m_typeName);
			if (metadataTypeIt == metadataTypes.end() || metadataDefaultsIt == metadataDefaults.end())
			{
				return Fail(
					EWorkspaceModuleLoadStatus::MetadataInvalid,
					"Workspace metadata is missing type or CDO data for '" + collected.m_typeName + "'.");
			}

			const YAML::Node& metadataType = metadataTypeIt->second;
			const YAML::Node& metadataDefaultObject = metadataDefaultsIt->second;
			if (!metadataType["base"] || !metadataType["base"].IsScalar() ||
				metadataType["base"].as<std::string>() != collected.m_baseTypeName)
			{
				return Fail(
					EWorkspaceModuleLoadStatus::MetadataInvalid,
					"Workspace metadata base type does not match the descriptor for '" +
					collected.m_typeName + "'.");
			}

			if (!ValidateComponentHierarchy(
				*collected.m_typeInfo,
				collectedTypeInfos,
				metadataError) ||
				!ValidatePropertySchema(
					metadataType["properties"], *collected.m_typeInfo, metadataError) ||
				!ValidateCanonicalDefaultValues(
					metadataDefaultObject["defaultValues"], collected, metadataError))
			{
				return Fail(EWorkspaceModuleLoadStatus::MetadataInvalid, std::move(metadataError));
			}

			Reflection::WorkspaceTypeRegistration registration;
			registration.m_typeInfo = collected.m_typeInfo;
			registration.m_alignment = static_cast<size_t>(collected.m_typeAlignment);
			registration.m_defaultObject = Reflection::CreateReflectedData(
				*collected.m_typeInfo,
				metadataDefaultObject["defaultValues"]);
			const TWorkspacePlacementFactoryV1 placementFactory = collected.m_placementFactory;
			registration.m_placementFactory = [placementFactory](void* destination) -> IReflectable*
			{
				if (placementFactory(destination) == nullptr)
				{
					return nullptr;
				}

				return static_cast<Component*>(destination);
			};
			registrations.emplace_back(std::move(registration));
		}

		m_owner = moduleName + "@" + modulePath.generic_string();
		std::string registrationError;
		if (!Reflection::RegisterWorkspaceTypes(m_owner, std::move(registrations), registrationError))
		{
			return Fail(EWorkspaceModuleLoadStatus::RegistrationFailed, std::move(registrationError));
		}

		m_state = EWorkspaceModuleState::Registered;
		m_result.m_status = EWorkspaceModuleLoadStatus::Success;
		m_result.m_numRegisteredTypes = collector.m_types.size();
		m_result.m_message = "Loaded workspace module '" + moduleName + "' from '" +
			modulePath.generic_string() + "' with " + std::to_string(collector.m_types.size()) +
			" reflected type(s).";
		return m_result;
	}
	catch (const YAML::Exception& e)
	{
		return Fail(
			yamlFailureStatus,
			"Failed to parse workspace manifest or module metadata: " + std::string(e.what()));
	}
	catch (const std::exception& e)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::RegistrationFailed,
			"Workspace module activation failed: " + std::string(e.what()));
	}
	catch (...)
	{
		return Fail(
			EWorkspaceModuleLoadStatus::RegistrationFailed,
			"Workspace module activation failed with an unknown error.");
	}
}

bool Sailor::Workspace::WorkspaceModuleManager::Unload() noexcept
{
	if (!m_owner.empty())
	{
		Reflection::UnregisterWorkspaceTypes(m_owner);
		m_owner.clear();
	}

	m_metadata.clear();
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

	if (m_library.IsOpen() && !m_library.Close())
	{
		message += " Additionally, the module could not be unloaded: " + m_library.GetError();
	}

	m_metadata.clear();
	m_state = EWorkspaceModuleState::Failed;
	m_result.m_status = status;
	m_result.m_message = std::move(message);
	m_result.m_numRegisteredTypes = 0;
	return m_result;
}
