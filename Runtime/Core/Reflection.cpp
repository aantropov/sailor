#include "Reflection.h"
#include "Components/Component.h"
#include "Containers/ConcurrentMap.h"
#include "RHI/Types.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Animation/AnimationAssetInfo.h"
#include "AssetRegistry/AssetInfo.h"
#include "AssetRegistry/FrameGraph/FrameGraphAssetInfo.h"
#include "AssetRegistry/Material/MaterialAssetInfo.h"
#include "AssetRegistry/Model/ModelAssetInfo.h"
#include "AssetRegistry/Prefab/PrefabAssetInfo.h"
#include "AssetRegistry/Shader/ShaderAssetInfo.h"
#include "AssetRegistry/Texture/TextureAssetInfo.h"
#include "AssetRegistry/World/WorldPrefabAssetInfo.h"
#include "RHI/SceneView.h"

using namespace Sailor;

namespace
{
	template<typename TProperty>
	std::string GetAssetPropertyTypeName()
	{
		using PropertyType = ::refl::trait::remove_qualifiers_t<TProperty>;

		return TypeInfo::GetReflectedPropertyTypeName<PropertyType>();
	}

	YAML::Node MakeStringSequence(std::initializer_list<const char*> values)
	{
		YAML::Node node;
		for (const char* value : values)
		{
			node.push_back(value);
		}
		return node;
	}

	template<typename TAssetInfo>
	YAML::Node ExportAssetInfoType(const std::string& typeName, std::initializer_list<const char*> extensions)
	{
		YAML::Node node;
		node["typename"] = typeName;
		node["extensions"] = MakeStringSequence(extensions);

		YAML::Node properties(YAML::NodeType::Sequence);
		TVector<std::string> exportedPropertyNames;
		TAssetInfo* empty = nullptr;
		for_each(refl::reflect<TAssetInfo>().members, [&](auto member)
			{
				if constexpr (is_writable(member))
				{
					using PropertyType = ::refl::trait::remove_qualifiers_t<decltype(get_reader(member)(*empty))>;
					const std::string propertyName = NormalizeAssetInfoFieldName(get_display_name(member));

					if (exportedPropertyNames.Contains(propertyName))
					{
						return;
					}

					exportedPropertyNames.Add(propertyName);

					YAML::Node propertyNode;
					propertyNode["name"] = propertyName;
					propertyNode["type"] = GetAssetPropertyTypeName<PropertyType>();
					properties.push_back(propertyNode);
				}
			});

		node["properties"] = properties;

		return node;
	}

	TVector<YAML::Node> ExportAssetInfoTypes()
	{
		TVector<YAML::Node> nodes;

		nodes.Add(ExportAssetInfoType<AssetInfo>("Sailor::AssetInfo", {}));
		nodes.Add(ExportAssetInfoType<TextureAssetInfo>("Sailor::TextureAssetInfo", { "png", "bmp", "tga", "jpg", "gif", "psd", "dds", "hdr" }));
		nodes.Add(ExportAssetInfoType<ModelAssetInfo>("Sailor::ModelAssetInfo", { "glb", "gltf" }));
		nodes.Add(ExportAssetInfoType<AnimationAssetInfo>("Sailor::AnimationAssetInfo", { "anim" }));
		nodes.Add(ExportAssetInfoType<MaterialAssetInfo>("Sailor::MaterialAssetInfo", { "mat" }));
		nodes.Add(ExportAssetInfoType<ShaderAssetInfo>("Sailor::ShaderAssetInfo", { "shader" }));
		nodes.Add(ExportAssetInfoType<FrameGraphAssetInfo>("Sailor::FrameGraphAssetInfo", { "renderer" }));
		nodes.Add(ExportAssetInfoType<PrefabAssetInfo>("Sailor::PrefabAssetInfo", { "prefab" }));
		nodes.Add(ExportAssetInfoType<WorldPrefabAssetInfo>("Sailor::WorldPrefabAssetInfo", { "world" }));

		return nodes;
	}
}

namespace Sailor::Internal
{
	TUniquePtr<TConcurrentMap<std::string, ReflectedData, 32u, ERehashPolicy::Never>> g_pCdos;
	TUniquePtr<TConcurrentMap<std::string, Reflection::TPlacementFactoryMethod>> g_pPlacementFactoryMethods;
	TUniquePtr<TConcurrentMap<std::string, const TypeInfo*>> g_pReflectionTypes;
	Memory::ObjectAllocatorPtr g_cdoAllocator;
}

ComponentPtr Reflection::CreateCDO(const TypeInfo& pType)
{
	std::string typeName = pType.Name();

	static std::once_flag s_once{};

	std::call_once(s_once, [&]() {
		if (!Internal::g_pCdos)
		{
			Internal::g_pCdos = TUniquePtr<TConcurrentMap<std::string, ReflectedData, 32u, ERehashPolicy::Never>>::Make(128);
			Internal::g_cdoAllocator = Memory::ObjectAllocatorPtr::Make(Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
		}});

		check(Internal::g_pCdos && !Internal::g_pCdos->ContainsKey(typeName));

		auto cdo = CreateObject<Component>(pType, Internal::g_cdoAllocator);

		return cdo;
}

void Reflection::RegisterFactoryMethod(const TypeInfo& type, TPlacementFactoryMethod placementNew)
{
	static std::once_flag s_once{};

	std::call_once(s_once, [&]() {
		if (!Internal::g_pPlacementFactoryMethods)
		{
			Internal::g_pPlacementFactoryMethods = TUniquePtr<TConcurrentMap<std::string, Reflection::TPlacementFactoryMethod>>::Make();
		}});

		check(Internal::g_pPlacementFactoryMethods && !Internal::g_pPlacementFactoryMethods->ContainsKey(type.Name()));

		auto& method = Internal::g_pPlacementFactoryMethods->At_Lock(type.Name());
		method = placementNew;
		Internal::g_pPlacementFactoryMethods->Unlock(type.Name());
}

void Reflection::RegisterType(const std::string& typeName, const TypeInfo* pType)
{
	static std::once_flag s_once{};

	std::call_once(s_once, [&]() {
		if (!Internal::g_pReflectionTypes)
		{
			Internal::g_pReflectionTypes = TUniquePtr<TConcurrentMap<std::string, const TypeInfo*>>::Make();
		}});

		check(Internal::g_pReflectionTypes && !Internal::g_pReflectionTypes->ContainsKey(typeName));

		auto& type = Internal::g_pReflectionTypes->At_Lock(typeName);
		type = pType;
		Internal::g_pReflectionTypes->Unlock(typeName);
}

const TypeInfo& Reflection::GetTypeByName(const std::string& typeName)
{
	check(Internal::g_pReflectionTypes && Internal::g_pReflectionTypes->ContainsKey(typeName));

	return *(*Internal::g_pReflectionTypes)[typeName];
}

YAML::Node TypeInfo::Serialize() const
{
	YAML::Node res{};

	::Serialize(res, "typename", m_name);
	::Serialize(res, "base", m_base);
	::Serialize(res, "properties", m_props);

	return res;
};

void TypeInfo::Deserialize(const YAML::Node& inData)
{
	::Deserialize(inData, "typename", m_name);
	::Deserialize(inData, "base", m_base);
	::Deserialize(inData, "properties", m_props);
}

YAML::Node ReflectedData::Serialize() const
{
	assert(m_typeInfo);

	YAML::Node res{};

	::Serialize(res, "typename", m_typeInfo->Name());
	::Serialize(res, "overrideProperties", m_properties);

	return res;
};

void ReflectedData::Deserialize(const YAML::Node& inData)
{
	std::string typeName;

	::Deserialize(inData, "typename", typeName);
	::Deserialize(inData, "overrideProperties", m_properties);

	m_typeInfo = &(Reflection::GetTypeByName(typeName));
}

bool ReflectedData::operator==(const ReflectedData& rhs) const
{
	return m_typeInfo == rhs.m_typeInfo && m_properties == rhs.m_properties;
}

TMap<std::string, YAML::Node> ReflectedData::GetOverrideProperties() const
{
	const auto& cdo = Reflection::GetCDO(m_typeInfo->Name());
	return DiffTo(cdo).m_properties;
}

ReflectedData ReflectedData::DiffTo(const ReflectedData& rhs) const
{
	ReflectedData res;

	check(rhs.GetTypeInfo() == GetTypeInfo());

	res.m_typeInfo = &rhs.GetTypeInfo();

	for (const auto& prop : GetProperties())
	{
		if (*prop.m_second != rhs.m_properties[prop.m_first])
		{
			res.m_properties[prop.m_first] = prop.m_second;
		}
	}

	return res;
}

YAML::Node Reflection::ExportEngineTypes()
{
	// Write types
	auto types = Internal::g_pReflectionTypes->GetValues();

	YAML::Node yamlTypes;
	yamlTypes["timeStamp"] = std::time(nullptr);

	TVector<YAML::Node> nodes;
	for (auto& type : types)
	{
		nodes.Add(type->Serialize());
	}

	yamlTypes["engineTypes"] = nodes;

	nodes.Clear();
	for (auto& cdo : Internal::g_pCdos->GetValues())
	{
		YAML::Node yamlCdo;
		yamlCdo["typename"] = cdo.m_typeInfo->Name();
		yamlCdo["defaultValues"] = cdo.GetProperties();

		nodes.Add(yamlCdo);
	}

	yamlTypes["cdos"] = nodes;

	nodes.Clear();
	nodes.Add(ReflectEnumValues<EMobilityType>());
	nodes.Add(ReflectEnumValues<ELightType>());
	nodes.Add(ReflectEnumValues<EAnimationPlayMode>());
	nodes.Add(ReflectEnumValues<RHI::EFormat>());
	nodes.Add(ReflectEnumValues<RHI::ETextureFiltration>());
	nodes.Add(ReflectEnumValues<RHI::ETextureClamping>());
	nodes.Add(ReflectEnumValues<RHI::ESamplerReductionMode>());
	nodes.Add(ReflectEnumValues<RHI::EFillMode>());
	nodes.Add(ReflectEnumValues<RHI::ECullMode>());
	nodes.Add(ReflectEnumValues<RHI::EBlendMode>());
	nodes.Add(ReflectEnumValues<RHI::EDepthCompare>());
	nodes.Add(ReflectEnumValues<RHI::EShadowType>());

	yamlTypes["enums"] = nodes;
	yamlTypes["assetTypes"] = ExportAssetInfoTypes();

	return yamlTypes;
}

ObjectPtr IReflectable::ResolveAssetDependency(const FileId& fileId, bool bImmediate)
{
	return App::GetSubmodule<AssetRegistry>()->LoadAssetFromFile<Object>(fileId, bImmediate);
}

ObjectPtr IReflectable::ResolveExternalDependency(const InstanceId& componentInstanceId, const TMap<InstanceId, ObjectPtr>& resolveContext)
{
	if (auto goInstanceId = componentInstanceId.GameObjectId())
	{
		if (resolveContext.ContainsKey(goInstanceId))
		{
			auto go = resolveContext[goInstanceId].DynamicCast<GameObject>();

			const size_t index = go->GetComponents().FindIf([&](const auto& comp) { return comp->GetInstanceId().ComponentId() == componentInstanceId.ComponentId(); });
			if (index != -1)
			{
				return go->GetComponents()[index];
			}
		}
	}

	return ObjectPtr();
}
