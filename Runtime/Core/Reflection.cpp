#include "Reflection.h"
#include "Components/Component.h"
#include "Containers/ConcurrentMap.h"
#include "RHI/Types.h"

using namespace Sailor;

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
	::Serialize(res, "properties", m_props);

	return res;
};

void TypeInfo::Deserialize(const YAML::Node& inData)
{
	::Deserialize(inData, "typename", m_name);
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

void Reflection::ExportReflectionData()
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
	nodes.Add(ReflectEnumValues<RHI::EFormat>());
	nodes.Add(ReflectEnumValues<RHI::ETextureFiltration>());
	nodes.Add(ReflectEnumValues<RHI::ETextureClamping>());
	nodes.Add(ReflectEnumValues<RHI::EFillMode>());
	nodes.Add(ReflectEnumValues<RHI::ECullMode>());
	nodes.Add(ReflectEnumValues<RHI::EBlendMode>());
	nodes.Add(ReflectEnumValues<RHI::EDepthCompare>());

	yamlTypes["enums"] = nodes;

	std::ofstream file(AssetRegistry::GetCacheFolder() + "EngineTypes.yaml");
	file << yamlTypes;
	file.close();
}