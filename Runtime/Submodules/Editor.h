#pragma once
#include "Core/Submodule.h"
#include "yaml-cpp/include/yaml-cpp/yaml.h"
#include <concurrent_queue.h>
#include <wtypes.h>

namespace Sailor
{
	class Editor : public TSubmodule<Editor>
	{
	public:

		Editor(HWND editorHwnd, uint32_t editorPort);

		void SetWorld(class World* world) { m_world = world; }

		void PushMessage(const std::string& msg);
		bool PullMessage(std::string& msg);

		__forceinline size_t NumMessages() const { return m_messagesQueue.unsafe_size(); }

		YAML::Node SerializeWorld() const;
		void ApplyChanges(const std::string& yamlNode);

	protected:

		concurrency::concurrent_queue<std::string> m_messagesQueue;

		uint32_t m_editorPort;
		HWND m_editorHwnd;

		class World* m_world;
	};
}