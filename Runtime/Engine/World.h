#pragma once
#include "Sailor.h"
#include "Memory/ObjectPtr.hpp"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectAllocator.hpp"
#include "ECS/ECS.h"

namespace Sailor
{
	using GameObjectPtr = TObjectPtr<class GameObject>;
	using WorldPtr = class World*;

	class World
	{
	public:

		World(std::string name);

		virtual ~World() = default;

		World(const World&) = delete;
		World& operator=(const World&) = delete;

		World(World&&) = default;
		World& operator=(World&&) = default;

		GameObjectPtr Instantiate(const std::string& name = "Untitled");
		void Destroy(GameObjectPtr object);

		void Tick(float deltaTime);

		Memory::ObjectAllocatorPtr GetAllocator() { return m_allocator; }
		const Memory::ObjectAllocatorPtr& GetAllocator() const { return m_allocator; }

		template<typename T>
		T* GetECS()
		{
			const size_t typeId = T::GetComponentStaticType();
			return m_ecs[typeId].StaticCast<T>();
		}

	protected:

		std::string m_name;
		TVector<GameObjectPtr> m_objects;
		TList<GameObjectPtr, Memory::TInlineAllocator<sizeof(GameObjectPtr) * 32>> m_pendingDestroyObjects;
		TMap<size_t, Sailor::ECS::TBaseSystemPtr> m_ecs;

		Memory::ObjectAllocatorPtr m_allocator;
	};
}
