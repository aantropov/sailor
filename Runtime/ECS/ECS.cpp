#include <typeindex>
#include "ECS.h"
#include "Sailor.h"

using namespace Sailor;
using namespace Sailor::JobSystem;
using namespace Sailor::ECS;

void ECSFactory::RegisterECS(size_t typeInfo, std::function<TBaseSystemPtr()> factoryMethod)
{
	m_factoryMethods[typeInfo] = factoryMethod;
}

TVector<TBaseSystemPtr> ECSFactory::CreateECS() const
{
	TVector<TBaseSystemPtr> res;

	for (auto& ecs : m_factoryMethods)
	{
		res.Emplace(ecs.m_second());
	}

	return res;
}
