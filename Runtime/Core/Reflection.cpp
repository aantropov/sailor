#include "Reflection.h"
#include "Components/Component.h"
#include "Containers/ConcurrentMap.h"

using namespace Sailor;

namespace Sailor::Internal
{
	TUniquePtr<TConcurrentMap<std::string, Reflection::TPlacementFactoryMethod>> g_pPlacementFactoryMethods;
	TUniquePtr<TConcurrentMap<std::string, const TypeInfo*>> g_pReflectionTypes;
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
	Internal::g_pPlacementFactoryMethods->Unlock(typeName);
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
	res["typename"] = m_typeInfo->Name();
	res["properties"] = m_properties.ToVector();

	return res;
};

void ReflectionInfo::Deserialize(const YAML::Node& inData)
{
	std::string typeName = inData["typename"].as<std::string>();
	m_typeInfo = &(Reflection::GetTypeByName(typeName));

	auto properties = inData["properties"].as<TVector<TPair<std::string, YAML::Node>>>();

	for (const auto& prop : properties)
	{
		m_properties[prop.First()] = prop.Second();
	}
}