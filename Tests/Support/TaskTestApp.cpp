#include "TaskTestApp.h"
#include "Sailor.h"
#include "Tasks/Scheduler.h"

#include <stdexcept>

using namespace Sailor;

Tests::TaskTestApp::TaskTestApp()
{
	if (App::GetInstance())
	{
		throw std::logic_error("A task test must own its App");
	}
	App::s_pInstance = new App();
	App::AddSubmodule(TSubmodule<Tasks::Scheduler>::Make());
}

Tests::TaskTestApp::~TaskTestApp()
{
	App::RemoveSubmodule<Tasks::Scheduler>();
	delete App::s_pInstance;
	App::s_pInstance = nullptr;
}

Tasks::Scheduler& Tests::TaskTestApp::GetScheduler() const
{
	return *App::GetSubmodule<Tasks::Scheduler>();
}
