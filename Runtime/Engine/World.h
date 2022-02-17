#pragma once
#include "Sailor.h"
#include "GameObject.h"
#include "Memory/SharedPtr.hpp"
#include "Memory/ObjectAllocator.hpp"

namespace Sailor
{
	using WorldPtr = TWeakPtr<class World>;

	class World
	{
	public:

		World(std::string name);

		virtual ~World() = default;

		World(const World&) = delete;
		World& operator=(const World&) = delete;

		World(World&&) = default;
		World& operator=(World&&) = default;

		template<typename... TArgs >
		GameObjectPtr Instantiate()
		{
			auto newObject = GameObjectPtr::Make(m_allocator, this);
			assert(newObject);

			newObject->m_world = this;
			m_objects.Add(newObject);

			return newObject;
		}

		void Destroy(GameObjectPtr object)
		{
			if (object && !object->bPendingDestroy)
			{
				object->bPendingDestroy = true;
				m_pendingDestroyObjects.PushBack(std::move(object));
			}
		}

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
		TMap<size_t, ECS::TBaseSystemPtr> m_ecs;

		Memory::ObjectAllocatorPtr m_allocator;
	};
}
