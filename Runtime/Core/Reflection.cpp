#include "Reflection.h"
#include "Components/Component.h"
#include "Containers/ConcurrentMap.h"

using namespace Sailor;

namespace Sailor::Internal
{
	TUniquePtr<TConcurrentMap<std::string, Reflection::TPlacementFactoryMethod>> g_pPlacementFactoryMethods;
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
