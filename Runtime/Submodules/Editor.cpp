#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "Engine/Types.h"
#include "Engine/World.h"
#include "Editor.h"
#include "AssetRegistry/World/WorldPrefabImporter.h"
#include <libloaderapi.h>
#include <queue>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace Sailor;

Editor::Editor(HWND editorHwnd, uint32_t editorPort) :
	m_editorPort(editorPort),
	m_editorHwnd(editorHwnd)
{

}

bool Editor::UpdateObject(const std::string& strInstanceId, const std::string& strYamlNode)
{
	Sailor::InstanceId instanceId;
	ReflectedData overrideData;

	YAML::Node instanceIdYaml = YAML::Load(strInstanceId);
	YAML::Node objectYaml = YAML::Load(strYamlNode);

	instanceId.Deserialize(instanceIdYaml);
	overrideData.Deserialize(objectYaml);

	auto objPtr = m_world->GetObjectByInstanceId(instanceId);

	if (instanceId.ComponentId() != Sailor::InstanceId::Invalid)
	{
		objPtr.DynamicCast<Component>()->ApplyReflection(overrideData);
	}
	else if (instanceId.GameObjectId() != Sailor::InstanceId::Invalid)
	{
		// TODO
	}

	return true;
}

void Editor::PushMessage(const std::string& msg)
{
	if (NumMessages() < 1024)
	{
		std::time_t now = std::time(nullptr);
		std::tm localTime;

		errno_t err = localtime_s(&localTime, &now);

		if (err != 0)
		{
			// TODO: Handle error
			//throw std::runtime_error("Failed to get local time");
		}

		std::ostringstream oss;
		oss << '[' << std::put_time(&localTime, "%H:%M:%S") << "] " << msg;

		m_messagesQueue.push(oss.str());
	}
}

bool Editor::PullMessage(std::string& msg)
{
	if (m_messagesQueue.try_pop(msg))
	{
		return true;
	}

	return false;
}

YAML::Node Editor::SerializeWorld() const
{
	auto prefab = WorldPrefab::FromWorld(m_world);
	auto node = prefab->Serialize();
	return node;
}
