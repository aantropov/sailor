#include <typeindex>
#include "ECS.h"
#include "Sailor.h"

using namespace Sailor;
using namespace Sailor::JobSystem;
using namespace Sailor::ECS;

TMap<size_t, std::function<TBaseSystemPtr(void)>, Memory::MallocAllocator> ECSFactory::s_factoryMethods;

TVector<TBaseSystemPtr> ECSFactory::CreateECS() const
{
	TVector<TBaseSystemPtr> res;

	for (auto& ecs : s_factoryMethods)
	{
		res.Emplace(ecs.m_second());
	}

	return res;
}
