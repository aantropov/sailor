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