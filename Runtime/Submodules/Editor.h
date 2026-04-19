#pragma once
#include "Core/Submodule.h"
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
	namespace Win32 
	{
		class Window;
	}

	class Editor : public TSubmodule<Editor>
	{
	public:

		Editor(HWND editorHwnd, uint32_t editorPort, Win32::Window* pMainWindow);

		void SetWorld(class World* world) { m_world = world; }
		class World* GetWorld() const { return m_world; }

		void PushMessage(const std::string& msg);
		bool PullMessage(std::string& msg);

		__forceinline size_t NumMessages() const { return m_messagesQueue.unsafe_size(); }

		YAML::Node SerializeWorld() const;


		void ShowMainWindow(bool bShow);

		void SetViewport(RECT window) { m_windowRect = window; }
		RECT GetViewport() const { return m_windowRect; }

		bool UpdateObject(const class InstanceId& instanceId, const std::string& strYamlNode);
		bool ReparentObject(const class InstanceId& instanceId, const class InstanceId& parentInstanceId, bool bKeepWorldTransform);
		bool CreateGameObject(const class InstanceId& parentInstanceId);
		bool DestroyObject(const class InstanceId& instanceId);
		bool ResetComponentToDefaults(const class InstanceId& instanceId);
		bool AddComponent(const class InstanceId& instanceId, const std::string& componentTypeName);
		bool RemoveComponent(const class InstanceId& instanceId);
		bool InstantiatePrefab(const class FileId& prefabId, const class InstanceId& parentInstanceId);
		bool RenderPathTracedImage(const class InstanceId& instanceId, const std::string& outputPath, uint32_t height, uint32_t samplesPerPixel, uint32_t maxBounces);

	protected:

		concurrency::concurrent_queue<std::string> m_messagesQueue;

		RECT m_windowRect{};
		uint32_t m_editorPort;
		HWND m_editorHwnd;

		class Win32::Window* m_pMainWindow = nullptr;

		class World* m_world;
	};
}
