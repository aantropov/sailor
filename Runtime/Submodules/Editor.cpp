#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "Editor.h"
#include <libloaderapi.h>
#include <queue>
#include <string>
#include <ctime>
#include <iomanip>
#include <sstream>

using namespace Sailor;

Editor::Editor(HWND editorHwnd, uint32_t editorPort) :
	m_editorHwnd(editorHwnd),
	m_editorPort(editorPort)
{

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