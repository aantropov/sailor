#include "Framework/Object.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

#ifdef SAILOR_EDITOR

TaskPtr Object::OnHotReload()
{
	return TaskPtr(nullptr);
}

void Object::TraceHotReload(TaskPtr previousTask)
{
	TaskPtr hotReload = OnHotReload();

	if (hotReload)
	{
		App::GetSubmodule<Scheduler>()->Run(hotReload);
	}

	for (auto& obj : m_hotReloadDeps)
	{
		if (auto ptr = std::move(obj.Lock()))
		{
			ptr->TraceHotReload(hotReload);
		}
	}
}

void Object::AddHotReloadDependentObject(ObjectPtr object)
{
	m_hotReloadDeps.insert(object);
}

void Object::RemoveHotReloadDependentObject(ObjectPtr object)
{
	m_hotReloadDeps.erase(object);
}

void Object::ClearHotReloadDependentObjects()
{
	m_hotReloadDeps.clear();
}

#endif
