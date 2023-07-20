#pragma once
#include "Sailor.h"
#include "Memory/SharedPtr.hpp"
#include "Tasks/Scheduler.h"
#include "AssetRegistry/FileId.h"
#include "Engine/InstanceId.h"
#include "Containers/ConcurrentSet.h"
#include "Memory/ObjectPtr.hpp"
#include "Engine/Types.h"
#include <typeindex>

namespace Sailor
{
	class Object
	{
	public:

		Object(FileId uid) : m_fileId(std::move(uid)) {}

#ifdef SAILOR_EDITOR

		SAILOR_API virtual Tasks::ITaskPtr OnHotReload();

		SAILOR_API void TraceHotReload(Tasks::ITaskPtr previousTask);
		SAILOR_API void AddHotReloadDependentObject(ObjectPtr object);
		SAILOR_API void RemoveHotReloadDependentObject(ObjectPtr object);
		SAILOR_API void ClearHotReloadDependentObjects();
#endif

		SAILOR_API virtual bool IsReady() const { return true; }

		SAILOR_API Object() = default;
		SAILOR_API virtual ~Object() = default;

		SAILOR_API virtual bool IsValid() const { return true; }

		Object(const Object&) = delete;
		Object& operator=(const Object&) = delete;

		Object(Object&&) = default;
		Object& operator=(Object&&) = default;

		// Object could be related to loaded asset, texture, material, etc..
		SAILOR_API const FileId& GetFileId() const { return m_fileId; }

		// Needs to resolve reflection
		SAILOR_API const InstanceId& GetInstanceId() const { return m_instanceId; }

	protected:

		FileId m_fileId = FileId::Invalid;
		InstanceId m_instanceId = InstanceId::Invalid;

#ifdef SAILOR_EDITOR
		TConcurrentSet<ObjectPtr> m_hotReloadDeps;
#endif
	};
}
