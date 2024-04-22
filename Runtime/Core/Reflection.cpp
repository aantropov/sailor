#include "Reflection.h"
#include "Components/Component.h"
#include "Containers/ConcurrentMap.h"

using namespace Sailor;

namespace Sailor::Internal
{
	TUniquePtr<TConcurrentMap<std::string, ReflectionInfo, 32u, ERehashPolicy::Never>> g_pCdos;
	TUniquePtr<TConcurrentMap<std::string, Reflection::TPlacementFactoryMethod>> g_pPlacementFactoryMethods;
	TUniquePtr<TConcurrentMap<std::string, const TypeInfo*>> g_pReflectionTypes;
	Memory::ObjectAllocatorPtr g_cdoAllocator;
}

void Reflection::RegisterCDO(const TypeInfo& pType)
{
	//std::string typeName = pType.Name();

	//static std::once_flag s_once{};

	//std::call_once(s_once, [&]() {
	//	if (!Internal::g_pCdos)
	//	{
	//		Internal::g_pCdos = TUniquePtr<TConcurrentMap<std::string, ReflectionInfo, 32u, ERehashPolicy::Never>>::Make(128);
	//		Internal::g_cdoAllocator = Memory::ObjectAllocatorPtr::Make(Memory::EAllocationPolicy::SharedMemory_MultiThreaded);
	//	}});

	//check(Internal::g_pCdos && !Internal::g_pCdos->ContainsKey(typeName));

	//auto& cdoInfo = Internal::g_pCdos->At_Lock(typeName);

	//auto cdo = CreateObject<Component>(pType, Internal::g_cdoAllocator);
	//cdoInfo = cdo->GetReflectionInfo();
	//Internal::g_pCdos->Unlock(typeName);

	//// We don't store CDOs
	//cdo.ForcelyDestroyObject();
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

YAML::Node ReflectionInfo::Serialize() const
{
	assert(m_typeInfo);

	YAML::Node res{};

	::Serialize(res, "typename", m_typeInfo->Name());
	::Serialize(res, "overrideProperties", m_properties);

	return res;
};

void ReflectionInfo::Deserialize(const YAML::Node& inData)
{
	std::string typeName;

	::Deserialize(inData, "typename", typeName);
	::Deserialize(inData, "overrideProperties", m_properties);

	m_typeInfo = &(Reflection::GetTypeByName(typeName));
}

bool ReflectionInfo::operator==(const ReflectionInfo& rhs) const
{
	return m_typeInfo == rhs.m_typeInfo && m_properties == rhs.m_properties;
}

TMap<std::string, YAML::Node> ReflectionInfo::GetOverrideProperties() const
{
	TMap<std::string, YAML::Node> res;
	const auto& cdo = Reflection::GetCDO(m_typeInfo->Name());
	const auto& defaultValues = cdo.GetProperties();

	for (const auto& prop : GetProperties())
	{
		if (defaultValues[prop.m_first] != *prop.m_second)
		{
			res[prop.m_first] = prop.m_second;
		}
	}

	return res;
}