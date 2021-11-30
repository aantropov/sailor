#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "JobSystem/JobSystem.h"
#include <unordered_set>

namespace Sailor
{
	using ObjectPtr = TWeakPtr<class Object>;

	class Object
	{
	public:
#ifdef SAILOR_EDITOR

		virtual JobSystem::TaskPtr OnHotReload();

		void TraceHotReload(JobSystem::TaskPtr previousTask);
		void AddHotReloadDependentObject(ObjectPtr object);
		void RemoveHotReloadDependentObject(ObjectPtr object);
		void ClearHotReloadDependentObjects();
#endif

		Object() noexcept = default;
		virtual ~Object() = default;

		Object(const Object&) = delete;
		Object& operator=(const Object&) = delete;

		Object(Object&&) = default;
		Object& operator=(Object&&) = default;

	protected:
#ifdef SAILOR_EDITOR

		std::unordered_set<ObjectPtr> m_hotReloadDeps;
#endif
	};
}
