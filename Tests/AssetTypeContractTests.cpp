#include "Core/Reflection.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Texture/RenderTextureDescriptor.h"
#include "Components/CameraComponent.h"
#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/Animation/AnimationControllerAssetInfo.h"
#include "AssetRegistry/Audio/AudioAssetInfo.h"
#include "AssetRegistry/FrameGraph/FrameGraphAssetInfo.h"
#include "AssetRegistry/GlobalIllumination/GIProbesAssetInfo.h"
#include "AssetRegistry/Landscape/LandscapeVegetationAsset.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "AssetRegistry/Model/ModelAssetInfo.h"
#include "AssetRegistry/Model/GeneratedModelAssetMetadata.h"
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
#include <string_view>

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
	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
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

	YAML::Node FindAsset(const YAML::Node& catalog, std::string_view name)
	{
		for (const auto& type : catalog["assetTypes"])
		{
			if (type["typename"].as<std::string_view>() == name)
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
		Require(extensions == std::set<std::string>{ "png", "bmp", "tga", "jpg", "gif", "psd", "dds", "hdr", "rendertexture",
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

	void TestBorrowedMetadataNames()
	{
		static_assert(NormalizeAssetInfoFieldName("m_assetFilename") == "filename");
		static_assert(NormalizeAssetInfoFieldName("m_gain") == "gain");
		static_assert(NormalizeAssetInfoFieldName("gain") == "gain");
		const char field[] = { 'm', '_', 'g', 'a', 'i', 'n', 'X' };
		const auto name = NormalizeAssetInfoFieldName(std::string_view(field, 6));
		Require(name == "gain" && name.data() == field + 2,
			"field-name normalization must borrow only the requested characters");

		YAML::Node metadata, texture, animation;
		const FileId id = FileId::CreateNewFileId();
		{
			std::string text = "prefix:Ship.gltf:suffix";
			const std::string_view filename = std::string_view(text).substr(7, 9);
			metadata = CreateAssetInfoMetadata<ModelAssetInfo>(id, filename);
			texture = GeneratedModelAssetMetadata::CreateTexture(id, filename, 2);
			animation = GeneratedModelAssetMetadata::CreateAnimation(id, filename, 3, 1);
			text.assign(1024, 'x');
		}
		for (const auto& node : { metadata, texture, animation })
		{
			Require(node["filename"].Scalar() == "Ship.gltf" && node["fileId"].Scalar() == id.ToString(),
				"metadata must own readable names after the borrowed input is replaced and destroyed");
		}
		Require(texture["glbTextureIndex"].as<uint32_t>() == 2 &&
			animation["animationIndex"].as<uint32_t>() == 3 && animation["skinIndex"].as<uint32_t>() == 1,
			"borrowed filename input must preserve generated-asset metadata");
	}

	void TestDescriptorSerializeDeserialize()
	{
		const std::array<RHI::ETextureFormat, 3> formats = { RHI::ETextureFormat::R8G8B8A8_UNORM,
			RHI::ETextureFormat::R8G8B8A8_SRGB, RHI::ETextureFormat::R16G16B16A16_SFLOAT };
		for (const auto fmt : formats)
		{
			RenderTextureDescriptor src;
			src.m_width = 1920;
			src.m_height = 1080;
			src.m_format = fmt;
			const YAML::Node node = src.Serialize();
			Require(node["version"].as<uint32_t>() == 1, "serialized version must be 1");
			Require(node["width"].as<uint32_t>() == 1920 && node["height"].as<uint32_t>() == 1080,
				"non-square extents must roundtrip");
			RenderTextureDescriptor dst;
			dst.Deserialize(node);
			Require(dst.m_width == 1920 && dst.m_height == 1080 && dst.m_format == fmt,
				"roundtrip must preserve all fields");
		}
		{
			RenderTextureDescriptor src;
			src.m_width = 640;
			src.m_height = 480;
			YAML::Node node = src.Serialize();
			node.remove("format");
			RenderTextureDescriptor dst;
			dst.Deserialize(node);
			Require(dst.m_format == RHI::ETextureFormat::R8G8B8A8_UNORM,
				"omitted format must default to R8G8B8A8_UNORM");
		}
		RenderTextureDescriptor sentinel;
		sentinel.m_width = 256;
		sentinel.m_height = 256;
		sentinel.m_format = RHI::ETextureFormat::R8G8B8A8_SRGB;
		const char* invalidYaml[] = {
			"width: 1\nheight: 1\n",
			"version: 2\nwidth: 1\nheight: 1\n",
			"version: 1\nheight: 1\n",
			"version: 1\nwidth: 1\n",
			"version: 1\nwidth: 0\nheight: 1\n",
			"version: 1\nwidth: 1\nheight: 0\n",
			"version: 1\nwidth: -1\nheight: 1\n",
			"version: 1\nwidth: 1\nheight: -1\n",
			"version: 1\nwidth: 16385\nheight: 1\n",
			"version: 1\nwidth: 1\nheight: 16385\n",
			"version: 1\nwidth: abc\nheight: 1\n",
			"version: 1\nwidth: 1\nheight: xyz\n",
			"version: 1\nwidth: [1,2]\nheight: 1\n",
			"version: 1\nwidth: 1\nheight: 1\nformat: D24_UNORM_S8_UINT\n",
			"version: 1\nwidth: 1\nheight: 1\nformat: BC7_UNORM\n",
			"version: 1\nwidth: 1\nheight: 1\nformat: UNKNOWN_FMT\n",
			"[]",
		};
		for (const char* doc : invalidYaml)
		{
			const YAML::Node bad = YAML::Load(doc);
			RenderTextureDescriptor copy = sentinel;
			bool threw = false;
			try
			{
				copy.Deserialize(bad);
			}
			catch (const YAML::Exception&)
			{
				threw = true;
			}
			Require(threw, "invalid YAML must throw YAML::Exception");
			Require(copy.m_width == 256 && copy.m_height == 256 &&
				copy.m_format == RHI::ETextureFormat::R8G8B8A8_SRGB,
				"failed Deserialize must leave descriptor unchanged");
		}
		struct BadSer { uint32_t w; uint32_t h; RHI::ETextureFormat fmt; uint32_t ver; };
		const BadSer badSerialize[] = {
			{1, 1, RHI::ETextureFormat::R8G8B8A8_UNORM, 99},
			{0, 1, RHI::ETextureFormat::R8G8B8A8_UNORM, 1},
			{1, 20000, RHI::ETextureFormat::R8G8B8A8_UNORM, 1},
			{1, 1, static_cast<RHI::ETextureFormat>(999), 1},
		};
		for (const auto& b : badSerialize)
		{
			RenderTextureDescriptor bad;
			bad.m_width = b.w;
			bad.m_height = b.h;
			bad.m_format = b.fmt;
			bad.m_version = b.ver;
			bool threw = false;
			try
			{
				bad.Serialize();
			}
			catch (const YAML::Exception&)
			{
				threw = true;
			}
			Require(threw, "Serialize must reject invalid descriptor");
		}
	}

	void TestCameraDataDefaults()
	{
		CameraData data;
		Require(!static_cast<bool>(data.GetRendererConfig()),
			"default rendererConfig must be invalid");
		Require(!static_cast<bool>(data.GetRenderTarget()),
			"default renderTarget must be null");
		Require(data.GetQualityPreset() == Settings::EGraphicsQualitySelection::ProjectDefault,
			"default qualityPreset must be ProjectDefault");
		const InstanceId owner = InstanceId::GenerateNewInstanceId();
		const InstanceId id1 = InstanceId::GenerateNewComponentId(owner);
		const FileId cfg = FileId::CreateNewFileId();
		data.SetInstanceId(id1);
		data.SetRendererConfig(cfg);
		data.SetQualityPreset(Settings::EGraphicsQualitySelection::High);
		CameraData copy = data;
		data.SetRendererConfig(FileId::CreateNewFileId());
		data.SetQualityPreset(Settings::EGraphicsQualitySelection::ProjectDefault);
		data.SetInstanceId(InstanceId::GenerateNewComponentId(owner));
		Require(copy.GetRendererConfig() == cfg, "copy must retain original config");
		Require(copy.GetQualityPreset() == Settings::EGraphicsQualitySelection::High,
			"copy must retain original quality");
		Require(copy.GetInstanceId() == id1, "copy must retain original instance id");
		const InstanceId id2 = InstanceId::GenerateNewComponentId(owner);
		Require(id1 != id2, "two component IDs of same owner must differ");
	}

	void TestCameraComponentReflection()
	{
		const YAML::Node catalog = Reflection::ExportEngineTypes();
		bool foundType = false, foundCdo = false;
		for (const auto& t : catalog["engineTypes"])
		{
			if (t["typename"].as<std::string>() == "Sailor::CameraComponent")
			{
				foundType = true;
				const YAML::Node props = t["properties"];
				Require(props["rendererConfig"].IsDefined() &&
					props["renderTarget"].IsDefined() &&
					props["qualityPreset"].IsDefined(),
					"CameraComponent must export rendererConfig/renderTarget/qualityPreset");
			}
		}
		for (const auto& c : catalog["cdos"])
		{
			if (c["typename"].as<std::string>() == "Sailor::CameraComponent")
			{
				foundCdo = true;
				Require(c["defaultValues"]["qualityPreset"].Scalar() == "ProjectDefault",
					"CDO qualityPreset must be ProjectDefault");
			}
		}
		Require(foundType && foundCdo, "CameraComponent must appear in engineTypes and cdos");
	}

}

int main()
{
	int failures = 0;
	for (const auto test : { &TestColdVegetationCatalog, &TestQualifiedEnumName, &TestHandlerCatalogParity,
		&TestAnimationDoesNotClaimModelFiles, &TestAssetSchemaOrderAndIsolation, &TestNewRegisteredAsset,
		&TestBorrowedMetadataNames, &TestDescriptorSerializeDeserialize,
		&TestCameraDataDefaults, &TestCameraComponentReflection })
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
