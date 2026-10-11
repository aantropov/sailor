#include "Reflection.h"
#include "ReflectionMetadata.h"
#include "YamlUtils.h"

#include <ctime>

using namespace Sailor;

namespace
{
	bool IndexEntries(const YAML::Node& metadata, std::string_view section,
		TMap<std::string, YAML::Node>& entries, std::string& error)
	{
		const auto sequence = metadata[section];
		if (!sequence || !sequence.IsSequence())
		{
			error = "Type metadata section '" + std::string(section) + "' must be a sequence.";
			return false;
		}
		for (const YAML::Node& entry : sequence)
		{
			if (!entry.IsMap() || (section == "enums" && entry.size() != 1))
			{
				error = "Invalid type metadata entry in '" + std::string(section) + "'.";
				return false;
			}
			const auto name = section == "enums" ? entry.begin()->first : entry["typename"];
			if (!name || !name.IsScalar() || name.Scalar().empty() || !entries.Insert(name.Scalar(), entry))
			{
				error = "Duplicate or invalid type metadata identity in '" + std::string(section) + "'.";
				return false;
			}
		}
		return true;
	}

	bool IndexCatalog(const YAML::Node& metadata, ReflectedTypeCatalog& catalog, std::string& error)
	{
		if (!metadata.IsMap())
		{
			error = "Type metadata must be a map.";
			return false;
		}
		return IndexEntries(metadata, "engineTypes", catalog.m_types, error) &&
			IndexEntries(metadata, "cdos", catalog.m_defaults, error) &&
			IndexEntries(metadata, "enums", catalog.m_enums, error);
	}

	bool ValidateDefault(const YAML::Node& value, std::string_view type,
		const ReflectedTypeCatalog& catalog, std::string& error)
	{
		if (type.starts_with("List<") && type.ends_with('>'))
		{
			if (value.IsNull())
			{
				return true;
			}
			if (!value.IsSequence())
			{
				error = "Expected a list default for '" + std::string(type) + "'.";
				return false;
			}
			for (const YAML::Node& item : value)
			{
				if (!ValidateDefault(item, type.substr(5, type.size() - 6), catalog, error))
				{
					return false;
				}
			}
			return true;
		}
		if (type.starts_with("enum "))
		{
			const auto definition = catalog.m_enums.Find(std::string(type));
			if (definition != catalog.m_enums.end() && value.IsScalar())
			{
				for (const YAML::Node& member : definition.Value().begin()->second)
				{
					if (member.IsScalar() && member.Scalar() == value.Scalar())
					{
						return true;
					}
				}
			}
			error = "Default value for '" + std::string(type) + "' is not a declared member.";
			return false;
		}
		const auto schema = catalog.m_types.Find(std::string(type));
		if (schema != catalog.m_types.end() && value.IsMap())
		{
			const YAML::Node properties = schema.Value()["properties"];
			if (!properties.IsMap())
			{
				return true;
			}
			for (const auto& property : value)
			{
				const auto fieldType = properties[property.first.Scalar()];
				if (fieldType && fieldType.IsScalar() && !ValidateDefault(property.second, fieldType.Scalar(), catalog, error))
				{
					return false;
				}
			}
		}
		return true;
	}
}

YAML::Node Reflection::ExportTypes(const TVector<const TypeInfo*>& types)
{
	YAML::Node metadata;
	metadata["timeStamp"] = std::time(nullptr);
	for (const auto section : { "engineTypes", "cdos", "enums", "assetTypes" })
	{
		metadata[section] = YAML::Node(YAML::NodeType::Sequence);
	}
	TSet<std::string> typeNames;
	TSet<std::string> enumNames;
	for (const auto* type : types)
	{
		typeNames.Insert(type->Name());
		metadata["engineTypes"].push_back(type->Serialize());
		YAML::Node defaults;
		defaults["typename"] = type->Name();
		defaults["defaultValues"] = YAML::Clone(*type->GetDefaultValues());
		metadata["cdos"].push_back(defaults);
	}
	for (const auto* type : types)
	{
		type->AppendValueTypes(metadata, typeNames, enumNames);
	}
	return metadata;
}

bool Reflection::PrepareTypeCatalog(YAML::Node metadata, TSet<std::string> registeredTypes,
	ReflectedTypeCatalog& outCatalog, std::string& outError)
{
	ReflectedTypeCatalog catalog;
	if (!IndexCatalog(metadata, catalog, outError))
	{
		return false;
	}
	for (auto entry = catalog.m_enums.begin(); entry != catalog.m_enums.end(); ++entry)
	{
		const YAML::Node values = entry.Value().begin()->second;
		if (!values.IsSequence() || values.size() == 0)
		{
			outError = "Reflected enum '" + entry.Key() + "' has no declared values.";
			return false;
		}
	}
	for (auto defaults = catalog.m_defaults.begin(); defaults != catalog.m_defaults.end(); ++defaults)
	{
		if (!ValidateDefault(defaults.Value()["defaultValues"], defaults.Key(), catalog, outError))
		{
			return false;
		}
	}
	catalog.m_metadata = std::move(metadata);
	catalog.m_registeredTypes = std::move(registeredTypes);
	outCatalog = std::move(catalog);
	outError.clear();
	return true;
}

bool Reflection::MergeTypeMetadata(const YAML::Node& engineMetadata, const ReflectedTypeCatalog& workspace,
	YAML::Node& outMetadata, std::string& outError)
{
	ReflectedTypeCatalog engine;
	if (!IndexCatalog(engineMetadata, engine, outError))
	{
		return false;
	}
	TSet<std::string> sharedTypes;
	TSet<std::string> sharedEnums;
	for (auto type = workspace.m_types.begin(); type != workspace.m_types.end(); ++type)
	{
		const auto existing = engine.m_types.Find(type.Key());
		if (existing == engine.m_types.end())
		{
			continue;
		}
		const auto existingDefaults = engine.m_defaults.Find(type.Key());
		const auto defaults = workspace.m_defaults.Find(type.Key());
		if (workspace.m_registeredTypes.Contains(type.Key()) ||
			!Utils::AreYamlNodesEqual(type.Value(), existing.Value()) ||
			existingDefaults == engine.m_defaults.end() || defaults == workspace.m_defaults.end() ||
			!Utils::AreYamlNodesEqual(defaults.Value(), existingDefaults.Value()))
		{
			outError = "Workspace type metadata conflicts with engine identity '" + type.Key() + "'.";
			return false;
		}
		sharedTypes.Insert(type.Key());
	}
	for (auto defaults = workspace.m_defaults.begin(); defaults != workspace.m_defaults.end(); ++defaults)
	{
		if (engine.m_defaults.ContainsKey(defaults.Key()) && !sharedTypes.Contains(defaults.Key()))
		{
			outError = "Workspace defaults conflict with engine identity '" + defaults.Key() + "'.";
			return false;
		}
	}
	for (auto entry = workspace.m_enums.begin(); entry != workspace.m_enums.end(); ++entry)
	{
		const auto existing = engine.m_enums.Find(entry.Key());
		if (existing == engine.m_enums.end())
		{
			continue;
		}
		if (!Utils::AreYamlNodesEqual(entry.Value(), existing.Value()))
		{
			outError = "Workspace enum metadata conflicts with engine identity '" + entry.Key() + "'.";
			return false;
		}
		sharedEnums.Insert(entry.Key());
	}
	YAML::Node merged = YAML::Clone(engineMetadata);
	for (const auto section : { "engineTypes", "cdos", "enums" })
	{
		for (const YAML::Node& entry : workspace.m_metadata[section])
		{
			const bool bEnum = std::string_view(section) == "enums";
			const auto& name = bEnum ? entry.begin()->first.Scalar() : entry["typename"].Scalar();
			if ((bEnum ? sharedEnums : sharedTypes).Contains(name))
			{
				continue;
			}
			merged[section].push_back(YAML::Clone(entry));
		}
	}
	merged["moduleName"] = workspace.m_metadata["moduleName"].Scalar();
	merged["metadataVersion"] = workspace.m_metadata["metadataVersion"].as<uint32_t>();
	outMetadata = std::move(merged);
	outError.clear();
	return true;
}
