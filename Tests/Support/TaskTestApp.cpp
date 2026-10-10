#include "TaskTestApp.h"
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "AssetRegistry/Animation/AnimationImporter.h"
#include "Audio/AudioSystem.h"

#include <stdexcept>

using namespace Sailor;

// MSVC also needs the template's static methods emitted by the runtime DLL.
template class Sailor::TSubmodule<Sailor::Tests::GarbageCollectionProbe>;

void Tests::GarbageCollectionProbe::CollectGarbage()
{
	++m_numCollections;
}

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
	App::RemoveSubmodule<AnimationImporter>();
	App::RemoveSubmodule<AudioSystem>();
	App::RemoveSubmodule<Tasks::Scheduler>();
	delete App::s_pInstance;
	App::s_pInstance = nullptr;
}

Tasks::Scheduler& Tests::TaskTestApp::GetScheduler() const
{
	return *App::GetSubmodule<Tasks::Scheduler>();
}

AnimationImporter& Tests::TaskTestApp::AddAnimationImporter(AnimationAssetInfoHandler& handler)
{
	return *App::AddSubmodule(TSubmodule<AnimationImporter>::Make(&handler));
}

AudioSystem& Tests::TaskTestApp::AddAudioSystem()
{
	return *App::AddSubmodule(TSubmodule<AudioSystem>::Make(true));
}
