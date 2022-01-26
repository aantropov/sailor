#pragma once
#include "Sailor.h"
#include "GameObject.h"
#include "Memory/SharedPtr.hpp"

namespace Sailor
{
	using WorldPtr = TWeakPtr<class World>;

	class World
	{
	public:

		World() = default;
		virtual ~World() = default;

		World(const World&) = delete;
		World& operator=(const World&) = delete;

		World(World&&) = default;
		World& operator=(World&&) = default;

		template<typename TObject, typename... TArgs >
		TWeakPtr<TObject> Instantiate(TArgs&& ... args)
		{
			auto newObject = TSharedPtr<GameObjectPtr>::Make(std::forward(args));
			AddObject(newObject);
			return newObject;
		}

		void AddObject(GameObjectPtr object)
		{
			assert(object && !object.Lock()->GetWorld());

			object.Lock()->m_world = this;
			m_objects.Add(object.Lock());
		}
				
		void Destroy(GameObjectPtr object)
		{
			object.Lock()->bPendingDestroy = true;
		}

	protected:

		TVector<TSharedPtr<GameObject>> m_objects;
	};
}