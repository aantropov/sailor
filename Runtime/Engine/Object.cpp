#include "Engine/Object.h"

using namespace Sailor;
using namespace Sailor::JobSystem;

#ifdef SAILOR_EDITOR

ITaskPtr Object::OnHotReload()
{
	return ITaskPtr(nullptr);
}

void Object::TraceHotReload(ITaskPtr previousTask)
{
	ITaskPtr hotReload = OnHotReload();

	if (hotReload)
	{
		if (previousTask != nullptr)
		{
			hotReload->Join(previousTask);
		}

		App::GetSubmodule<Scheduler>()->Run(hotReload);
	}
		
	for (auto& ptr : m_hotReloadDeps)
	{
		ptr->TraceHotReload(hotReload != nullptr ? hotReload : previousTask);
	}
}

void Object::AddHotReloadDependentObject(ObjectPtr object)
{
	m_hotReloadDeps.Insert(object);
}

void Object::RemoveHotReloadDependentObject(ObjectPtr object)
{
	m_hotReloadDeps.Remove(object);
}

void Object::ClearHotReloadDependentObjects()
{
	m_hotReloadDeps.Clear();
}

#endif
