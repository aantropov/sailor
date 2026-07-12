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

#if defined(_MSC_VER)
# pragma warning(push)
# pragma warning(disable: 4251)
#endif

namespace Sailor
{
	class SAILOR_SHARED_API Object
	{
	public:

		Object(FileId uid) : m_fileId(std::move(uid)) {}

#ifdef SAILOR_EDITOR

		virtual Tasks::ITaskPtr OnHotReload();

		void TraceHotReload(Tasks::ITaskPtr previousTask);
		void AddHotReloadDependentObject(ObjectPtr object);
		void RemoveHotReloadDependentObject(ObjectPtr object);
		void ClearHotReloadDependentObjects();
#endif

		virtual bool IsReady() const { return true; }

		Object() = default;
		virtual ~Object() = default;

		virtual bool IsValid() const { return true; }

		Object(const Object&) = delete;
		Object& operator=(const Object&) = delete;

		Object(Object&&) = default;
		Object& operator=(Object&&) = default;

		// Object could be related to loaded asset, texture, material, etc..
		const FileId& GetFileId() const { return m_fileId; }

		// Needs to resolve reflection
		const InstanceId& GetInstanceId() const { return m_instanceId; }

	protected:

		FileId m_fileId = FileId::Invalid;
		InstanceId m_instanceId = InstanceId::Invalid;

#ifdef SAILOR_EDITOR
		TConcurrentSet<ObjectPtr> m_hotReloadDeps;
#endif
	};
}

#if defined(_MSC_VER)
# pragma warning(pop)
#endif
