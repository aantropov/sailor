#include "Core/LogMacros.h"
#include "Tasks/Scheduler.h"
#include "Editor.h"
#include <libloaderapi.h>

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
		m_messages.push(msg);
	}
}

bool Editor::PullMessage(std::string& msg)
{
	if (m_messages.try_pop(msg))
	{
		return true;
	}

	return false;
}