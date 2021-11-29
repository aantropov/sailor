#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "JobSystem/JobSystem.h"
#include <unordered_set>

namespace Sailor
{
	class Object
	{
	public:
#ifdef SAILOR_EDITOR

		virtual JobSystem::TaskPtr OnHotReload();

		void TraceHotReload(JobSystem::TaskPtr previousTask);
		void AddHotReloadDependentObject(TWeakPtr<Object> object);
		void RemoveHotReloadDependentObject(TWeakPtr<Object> object);
		void ClearHotReloadDependentObjects();
#endif
		Object() noexcept = default;
		virtual ~Object() = default;

	protected:
#ifdef SAILOR_EDITOR

		std::unordered_set<TWeakPtr<Object>> m_hotReloadDeps;
#endif
	};

	using ObjectPtr = TWeakPtr<Object>;
}
