#include <typeindex>
#include "ECS.h"
#include "ECSAutoRegistration.h"
#include "Sailor.h"
#include "Engine/GameObject.h"

#include <atomic>

using namespace Sailor;
using namespace Sailor::Tasks;
using namespace Sailor::ECS;

namespace Sailor::Internal
{
	TUniquePtr<TMap<size_t, std::function<TBaseSystemPtr(void)>, Memory::MallocAllocator>> g_factoryMethods;
	std::atomic<bool> g_bSuppressEcsAutoRegistration{ false };
}

void TBaseSystem::UpdateGameObject(GameObjectPtr gameObject, size_t lastFrameChanges)
{
	gameObject->m_frameLastChange = std::max(lastFrameChanges, gameObject->m_frameLastChange);
}

void ECSFactory::RegisterECS(size_t typeInfo, std::function<TBaseSystemPtr(void)> factoryMethod)
{
	if (IsAutoRegistrationSuppressed())
	{
		return;
	}

	if (!Sailor::Internal::g_factoryMethods)
	{
		Sailor::Internal::g_factoryMethods = TUniquePtr< TMap<size_t, std::function<TBaseSystemPtr(void)>, Memory::MallocAllocator>>::Make();
	}

	// Static template instantiations can appear in more than one shared library.
	// The first registration belongs to the engine image and must not be replaced
	// with a callable whose code may disappear when a workspace module unloads.
	Sailor::Internal::g_factoryMethods->Insert(typeInfo, std::move(factoryMethod));
}

void Sailor::ECS::SetAutoRegistrationSuppressed(bool suppressed)
{
	Sailor::Internal::g_bSuppressEcsAutoRegistration.store(
		suppressed,
		std::memory_order_release);
}

bool Sailor::ECS::IsAutoRegistrationSuppressed()
{
	return Sailor::Internal::g_bSuppressEcsAutoRegistration.load(
		std::memory_order_acquire);
}

TVector<TBaseSystemPtr> ECSFactory::CreateECS() const
{
	TVector<TBaseSystemPtr> res;

	for (const auto& ecs : *Sailor::Internal::g_factoryMethods)
	{
		res.Add((*ecs.m_second)());
	}
	
	res.Sort([](const auto& lhs, const auto& rhs) { return lhs->GetOrder() < rhs->GetOrder(); });

	return res;
}
