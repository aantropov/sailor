#pragma once
#include "Core/Submodule.h"
#include "Memory/UniquePtr.hpp"
#include <atomic>
#include <glm/vec3.hpp>
#include <yaml-cpp/yaml.h>
#if __has_include(<concurrent_queue.h>)
#include <concurrent_queue.h>
#elif __has_include(<tbb/concurrent_queue.h>)
#include <tbb/concurrent_queue.h>
namespace concurrency = tbb;
#else
#error "No concurrent_queue implementation found"
#endif
#if defined(_WIN32)
#include <wtypes.h>
#endif

namespace Sailor
{
	template<typename T>
	class TObjectPtr;
	class InstanceId;
	class Prefab;
	class Model;
	struct EditorManagedMutationState;
	namespace EditorViewport
	{
		enum class ETransformOperation : uint8_t;
		enum class ETransformSpace : uint8_t;
		class EditorViewportController;
	}

	namespace Win32 
	{
		class Window;
	}

	class Editor : public TSubmodule<Editor>
	{
	public:

		SAILOR_API Editor(HWND editorHwnd, uint32_t editorPort, Win32::Window* pMainWindow);
		SAILOR_API ~Editor();

		SAILOR_SHARED_API void SetWorld(class World* world);
		class World* GetWorld() const { return m_world; }
		void TickViewportTools();
		void CancelViewportInteraction();
		bool PullViewportEvent(std::string& outEvent);
		void NotifyManagedSelectionMutation() { ++m_managedSelectionMutationRevision; }
		uint64_t GetManagedSelectionMutationRevision() const { return m_managedSelectionMutationRevision; }
		void NotifyManagedObjectMutation(const InstanceId& instanceId);
		uint64_t GetManagedObjectMutationRevision(const InstanceId& instanceId) const;

		void PushMessage(const std::string& msg);
		bool PullMessage(std::string& msg);

		__forceinline size_t NumMessages() const
		{
			return m_numMessages.load(std::memory_order_relaxed);
		}

		YAML::Node SerializeWorld() const;


		void ShowMainWindow(bool bShow);

		void SetViewport(RECT window) { m_windowRect = window; }
		RECT GetViewport() const { return m_windowRect; }

		SAILOR_SHARED_API bool UpdateObject(const class InstanceId& instanceId, const std::string& strYamlNode);
		SAILOR_API bool ReparentObject(const class InstanceId& instanceId, const class InstanceId& parentInstanceId, bool bKeepWorldTransform);
		bool CreateGameObject(const class InstanceId& parentInstanceId, const class InstanceId& preferredInstanceId, class InstanceId& outInstanceId);
		bool CreateModelInstance(
			const TObjectPtr<Model>& model,
			const std::string& name,
			const class InstanceId& parentInstanceId,
			bool bCreateHierarchy,
			const glm::vec3* worldPosition,
			const class InstanceId& preferredInstanceId,
			class InstanceId& outInstanceId);
		SAILOR_SHARED_API bool DestroyObject(const class InstanceId& instanceId);
		bool ResetComponentToDefaults(const class InstanceId& instanceId);
		bool AddComponent(const class InstanceId& instanceId, const std::string& componentTypeName, const class InstanceId& preferredInstanceId, class InstanceId& outInstanceId);
		SAILOR_SHARED_API bool RemoveComponent(const class InstanceId& instanceId);
		bool InstantiatePrefab(const class FileId& prefabId, const class InstanceId& parentInstanceId);
		SAILOR_API bool InstantiatePrefab(
			const TObjectPtr<Prefab>& prefab,
			const class InstanceId& parentInstanceId);
		SAILOR_API bool InstantiatePrefab(
			const TObjectPtr<Prefab>& prefab,
			const class InstanceId& parentInstanceId,
			bool bStrictInstanceIds);
		bool InstantiatePrefab(
			const class FileId& prefabId,
			const class InstanceId& parentInstanceId,
			const glm::vec3* worldPosition,
			class InstanceId& outInstanceId);
		bool InstantiatePrefab(
			const TObjectPtr<Prefab>& prefab,
			const class InstanceId& parentInstanceId,
			const glm::vec3* worldPosition,
			class InstanceId& outInstanceId,
			bool bStrictInstanceIds = false);
		bool TraceViewportRay(
			uint64_t viewportId,
			float normalizedX,
			float normalizedY,
			glm::vec3& outPosition) const;
		bool FocusEditorCamera(const class InstanceId& instanceId);
		bool SetPrefabLink(
			const class InstanceId& instanceId,
			const class FileId& prefabId);
		bool BreakPrefabLink(const class InstanceId& instanceId);
		bool SetViewportToolState(
			EditorViewport::ETransformOperation operation,
			EditorViewport::ETransformSpace space);
		void GetViewportToolState(
			EditorViewport::ETransformOperation& outOperation,
			EditorViewport::ETransformSpace& outSpace) const;
		bool RenderPathTracedImage(const class InstanceId& instanceId, const std::string& outputPath, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces);

	protected:

		concurrency::concurrent_queue<std::string> m_messagesQueue;
		std::atomic_size_t m_numMessages = 0;

		RECT m_windowRect{};
		uint32_t m_editorPort;
		HWND m_editorHwnd;

		class Win32::Window* m_pMainWindow = nullptr;

		class World* m_world = nullptr;
		TUniquePtr<EditorViewport::EditorViewportController> m_viewportController{};
		uint64_t m_managedSelectionMutationRevision = 0;
		TUniquePtr<EditorManagedMutationState> m_managedMutationState{};
	};
}
