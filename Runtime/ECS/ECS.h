#pragma once
#include "Sailor.h"
#include "JobSystem/JobSystem.h"
#include "Memory/ObjectPtr.hpp"

namespace Sailor::ECS
{
	template<typename TComponentData>
	class BaseSystem
	{
	public:

		size_t RegisterComponent()
		{
			if (m_freeList.Num() == 0)
			{
				return m_components.AddDefault(1);
			}

			size_t res = *(m_freeList.Last());
			m_freeList.PopBack();
			return res;
		}

		void UnregisterComponent(size_t index)
		{
			m_freeList.PushBack(index);
		}

		virtual void BeginPlay() = 0;
		virtual void Tick(float deltaTime) = 0;
		virtual void EndPlay() = 0;

		TComponentData& GetComponentData(size_t index) { return m_components[index]; }

	protected:

		TVector<TComponentData> m_components;
		TList<size_t> m_freeList;
	};
}
