#include "Core/Reflection.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/Animation/AnimationControllerAssetInfo.h"
#include "AssetRegistry/Audio/AudioAssetInfo.h"
#include "AssetRegistry/FrameGraph/FrameGraphAssetInfo.h"
#include "AssetRegistry/GlobalIllumination/GIProbesAssetInfo.h"
#include "AssetRegistry/Landscape/LandscapeVegetationAsset.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "AssetRegistry/Model/ModelAssetInfo.h"
#include "AssetRegistry/Prefab/PrefabAssetInfo.h"
#include "AssetRegistry/Shader/ShaderAssetInfo.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"
#include "AssetRegistry/World/WorldPrefabAssetInfo.h"
#include "Support/TempDirectory.h"

#include <array>
#include <cctype>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using namespace Sailor;

namespace AssetTypeFixture
{
	enum class EImportMode { Fast, Complete };

	class CustomAsset final : public AssetInfo
	{
	public:
		const TypeInfo& GetTypeInfo() const override { return TypeInfo::Get<CustomAsset>(); }
		float m_gain = 2.0f;
	};
}

REFL_AUTO(
	type(AssetTypeFixture::CustomAsset, bases<Sailor::AssetInfo>, Sailor::Attributes::Asset{ "custom-asset" }),
	field(m_gain)
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

	void TestColdVegetationCatalog()
	{
		const YAML::Node catalog = Reflection::ExportEngineTypes();
		for (const auto& type : catalog["assetTypes"])
		{
			if (type["typename"].as<std::string>() == "Sailor::LandscapeVegetationAssetInfo")
			{
				Require(type["extensions"].size() == 1 &&
					type["extensions"][0].as<std::string>() == "vegetation",
					"vegetation must have its registered extension before App initialization");
				return;
			}
		}
		throw std::runtime_error("the cold asset catalog omits LandscapeVegetationAssetInfo");
	}

	YAML::Node FindAsset(const YAML::Node& catalog, const std::string& name)
	{
		for (const auto& type : catalog["assetTypes"])
		{
			if (type["typename"].as<std::string>() == name)
			{
				return type;
			}
		}
		return YAML::Node(YAML::NodeType::Undefined);
	}

	void RequireImportedType(AssetRegistry& registry, const std::string& filename, const std::string& type)
	{
		const auto path = registry.GetWorkspaceContext().GetContent() / filename;
		AssetRegistry::WriteTextFile(path, "metadata discovery fixture");
		const FileId id = registry.GetOrLoadFile(filename);
		const AssetInfo* info = registry.GetAssetInfoPtr(id);
		Require(info != nullptr && info->GetAssetInfoType() == type,
			"catalog and imported asset metadata disagree for " + filename);
	}

	void TestHandlerCatalogParity()
	{
		Tests::TempDirectory directory("asset-type-catalog");
		const auto context = Workspace::ResolveWorkspaceContext(directory.Get());
		Require(context.IsSuccess(), context.m_message);
		AssetRegistry registry(context.m_context, nullptr);
		TextureAssetInfoHandler texture(&registry);
		ModelAssetInfoHandler model(&registry);
		AnimationAssetInfoHandler animation(&registry);
		AnimationControllerAssetInfoHandler controller(&registry);
		AnimationSetAssetInfoHandler animationSet(&registry);
		AudioAssetInfoHandler audio(&registry);
		MaterialAssetInfoHandler material(&registry);
		ShaderAssetInfoHandler shader(&registry);
		FrameGraphAssetInfoHandler frameGraph(&registry);
		GIProbesAssetInfoHandler probes(&registry);
		PrefabAssetInfoHandler prefab(&registry);
		WorldPrefabAssetInfoHandler world(&registry);
		LandscapeVegetationAssetInfoHandler vegetation(&registry);
		const std::array<IAssetInfoHandler*, 13> handlers = { &texture, &model, &animation,
			&controller, &animationSet, &audio, &material, &shader, &frameGraph, &probes,
			&prefab, &world, &vegetation };

		const YAML::Node catalog = Reflection::ExportEngineTypes();
		std::set<std::string> extensions;
		for (const auto handler : handlers)
		{
			YAML::Node defaults;
			handler->GetDefaultMeta(defaults);
			const std::string name = defaults["assetInfoType"].as<std::string>();
			const YAML::Node type = FindAsset(catalog, name);
			Require(type.IsDefined(), "registered handler is absent from catalog: " + name);
			Require(type["extensions"].IsSequence() && type["extensions"].size() > 0,
				"specialized asset handlers must export their extensions: " + name);
			for (const auto& item : type["extensions"])
			{
				std::string extension = item.as<std::string>();
				Require(extensions.insert(extension).second, "duplicate exported extension: " + extension);
				RequireImportedType(registry, "lower." + extension, name);
				for (char& c : extension)
				{
					c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
				}
				RequireImportedType(registry, "upper." + extension, name);
			}

			std::set<std::string> properties;
			for (const auto& property : type["properties"])
			{
				const std::string propertyName = property["name"].as<std::string>();
				Require(properties.insert(propertyName).second, "duplicate inherited asset property: " + propertyName);
				Require(defaults[propertyName].IsDefined(), "catalog property absent from actual metadata: " + propertyName);
			}
			for (const auto& property : defaults)
			{
				const std::string propertyName = property.first.as<std::string>();
				Require(propertyName == "assetInfoType" || properties.contains(propertyName),
					"serialized metadata property absent from catalog: " + propertyName);
			}
		}
		Require(extensions == std::set<std::string>{ "png", "bmp", "tga", "jpg", "gif", "psd", "dds", "hdr",
			"glb", "gltf", "anim", "animcontroller", "animset", "wav", "flac", "mp3", "mat", "shader", "glsl",
			"renderer", "probes", "prefab", "world", "vegetation" }, "the catalog must retain all supported file formats");
	}

	void TestAnimationDoesNotClaimModelFiles()
	{
		Tests::TempDirectory directory("asset-handler-order");
		const auto context = Workspace::ResolveWorkspaceContext(directory.Get());
		Require(context.IsSuccess(), context.m_message);
		AssetRegistry registry(context.m_context, nullptr);
		AnimationAssetInfoHandler animation(&registry);
		ModelAssetInfoHandler model(&registry);
		RequireImportedType(registry, "model.gltf", "Sailor::ModelAssetInfo");
		RequireImportedType(registry, "model.glb", "Sailor::ModelAssetInfo");
		RequireImportedType(registry, "clip.anim", "Sailor::AnimationAssetInfo");

		const FileId derivedId = FileId::CreateNewFileId();
		YAML::Node metadata;
		animation.GetDefaultMeta(metadata);
		metadata["fileId"] = derivedId.Serialize();
		metadata["filename"] = "model.gltf";
		AssetRegistry::WriteTextFile(context.m_context.GetContent() / "derived.anim.asset", metadata);
		Require(registry.ScanContentFolder() && registry.CompleteScanProcessing(), "derived animation metadata scan must complete");
		Require(registry.GetAssetInfoPtr<AnimationAssetInfoPtr>(derivedId) != nullptr,
			"derived glTF animation metadata must still be loaded by the animation handler");
	}

	void TestAssetSchemaOrderAndIsolation()
	{
		YAML::Node catalog = Reflection::ExportEngineTypes();
		const YAML::Node base = FindAsset(catalog, "Sailor::AssetInfo");
		Require(base.IsDefined() && base["extensions"].IsSequence() && base["extensions"].size() == 0,
			"default asset metadata must have an explicit empty extension sequence");
		YAML::Node texture = FindAsset(catalog, "Sailor::TextureAssetInfo");
		const std::array<const char*, 10> names = { "fileId", "filename", "clamping", "reduction", "filtration",
			"bShouldGenerateMips", "bShouldSupportStorageBinding", "bShouldKeepCpuBuffers", "format", "glbTextureIndex" };
		Require(texture["properties"].size() == names.size(), "texture schema must contain each inherited field once");
		for (size_t i = 0; i < names.size(); ++i)
		{
			Require(texture["properties"][i]["name"].as<std::string>() == names[i],
				"asset properties must retain reflection declaration order");
		}
		Require(texture["properties"][2]["type"].as<std::string>() == "enum Sailor::RHI::ETextureClamping",
			"asset enum fields must retain qualified RHI names");
		texture["extensions"][0] = "mutated";
		const YAML::Node fresh = FindAsset(Reflection::ExportEngineTypes(), "Sailor::TextureAssetInfo");
		Require(fresh["extensions"][0].as<std::string>() == "png", "exported YAML must not mutate the registered descriptor");
	}

	void TestNewRegisteredAsset()
	{
		constexpr const char* name = "AssetTypeFixture::CustomAsset";
		Require(!FindAsset(Reflection::ExportEngineTypes(), name).IsDefined(), "the fixture should start unregistered");
		const TypeInfo& type = TypeInfo::Get<AssetTypeFixture::CustomAsset>();
		Reflection::RegisterType(type.Name(), &type);
		const YAML::Node asset = FindAsset(Reflection::ExportEngineTypes(), name);
		Require(asset.IsDefined() && asset["extensions"][0].as<std::string>() == "custom-asset",
			"a new registered reflected asset must appear without a second catalog registration");
		Require(asset["properties"][0]["name"].as<std::string>() == "gain" &&
			asset["properties"][0]["type"].as<std::string>() == "float", "new asset schema must follow its reflected fields");
	}

	void TestQualifiedEnumName()
	{
		Require(TypeInfo::GetReflectedEnumTypeName<AssetTypeFixture::EImportMode>() ==
			"enum AssetTypeFixture::EImportMode", "enum names must retain the declaring namespace");
	}
}

int main()
{
	int failures = 0;
	for (const auto test : { &TestColdVegetationCatalog, &TestQualifiedEnumName, &TestHandlerCatalogParity,
		&TestAnimationDoesNotClaimModelFiles, &TestAssetSchemaOrderAndIsolation, &TestNewRegisteredAsset })
	{
		try
		{
			test();
		}
		catch (const std::exception& error)
		{
			std::cerr << error.what() << '\n';
			++failures;
		}
	}
	return failures == 0 ? 0 : 1;
}
