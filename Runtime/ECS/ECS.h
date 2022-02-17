#pragma once
#include "Sailor.h"
#include "JobSystem/JobSystem.h"
#include "Memory/ObjectPtr.hpp"
#include "Containers/Concepts.h"
#include "Core/Submodule.h"

namespace Sailor::ECS
{
	template<typename TCustomData>
	struct TComponentData
	{
		size_t m_lastProcessedFrame = 0;
		TCustomData m_data;
	};

	class ECSFactory;

	class TBaseSystem
	{
	public:

		virtual size_t RegisterComponent() = 0;
		virtual void UnregisterComponent(size_t index) = 0;

		virtual void BeginPlay() = 0;
		virtual void Tick(float deltaTime) = 0;
		virtual void EndPlay() = 0;

		size_t GetType() const { return std::type_index(typeid(*this)).hash_code(); }
	};

	template<typename TData>
	class TSystem : public TBaseSystem
	{
		static_assert(IsBaseOf<TData, TComponentData>::value, "TData must inherit from TComponentData");

	public:

		virtual size_t RegisterComponent() override
		{
			if (m_freeList.Num() == 0)
			{
				return m_components.AddDefault(1);
			}

			size_t res = *(m_freeList.Last());
			m_freeList.PopBack();
			return res;
		}

		virtual void UnregisterComponent(size_t index) override
		{
			m_freeList.PushBack(index);
		}

		TData& GetComponentData(size_t index) { return m_components[index]; }

		void RegisterECSFactoryMethod()
		{
			App::GetSubmodule<ECSFactory>()->RegisterECS(GetType(), []() { return TUnique<TSystem>::Make(); });
		}

	protected:

		TVector<TData> m_components;
		TList<size_t> m_freeList;
	};

	using TBaseSystemPtr = TUniquePtr<TBaseSystem>;

	template<typename T>
	using TSystemPtr = TUniquePtr<TSystem<T>>;

	class ECSFactory : public TSubmodule<ECSFactory>
	{
	public:

		void RegisterECS(size_t typeInfo, std::function<TBaseSystemPtr(void)> factoryMethod);

		TVector<TBaseSystemPtr> CreateECS() const;

	protected:

		TMap<size_t, std::function<TBaseSystemPtr(void)>> m_factoryMethods;
	};
}
