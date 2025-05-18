#pragma once
#include "Core/Submodule.h"
#include <yaml-cpp/yaml.h>
#include <concurrent_queue.h>
#include <wtypes.h>
#include "Platform/Window.h"

namespace Sailor
{



	class Editor : public TSubmodule<Editor>
	{
	public:

               Editor(HWND editorHwnd, uint32_t editorPort, Platform::Window* pMainWindow);

		void SetWorld(class World* world) { m_world = world; }

		void PushMessage(const std::string& msg);
		bool PullMessage(std::string& msg);

		__forceinline size_t NumMessages() const { return m_messagesQueue.unsafe_size(); }

		YAML::Node SerializeWorld() const;


		void ShowMainWindow(bool bShow);

		void SetViewport(RECT window) { m_windowRect = window; }
		RECT GetViewport() const { return m_windowRect; }

		bool UpdateObject(const class InstanceId& instanceId, const std::string& strYamlNode);

	protected:

		concurrency::concurrent_queue<std::string> m_messagesQueue;

		RECT m_windowRect{};
		uint32_t m_editorPort;
		HWND m_editorHwnd;

               class Platform::Window* m_pMainWindow = nullptr;

		class World* m_world;
	};
}