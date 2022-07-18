#pragma once
#include "EngineLoop.h"
#include "Core/Defines.h"

#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"

#include "Engine/GameObject.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/TestComponent.h"
#include "ECS/TransformECS.h"

using namespace Sailor;

TSharedPtr<World> EngineLoop::CreateWorld(std::string name)
{
	m_worlds.Emplace(TSharedPtr<World>::Make(std::move(name)));	

	auto gameObject = m_worlds[0]->Instantiate();
	auto cameraComponent = gameObject->AddComponent<CameraComponent>();
	auto testComponent = gameObject->AddComponent<TestComponent>();

	return m_worlds[m_worlds.Num() - 1];
}

void EngineLoop::ProcessCpuFrame(FrameState& currentInputState)
{
	SAILOR_PROFILE_FUNCTION();

	static uint32_t totalFramesCount = 0U;
	static Utils::Timer timer;

	timer.Start();

	SAILOR_PROFILE_BLOCK("CPU Frame");

	for (auto& world : m_worlds)
	{
		world->Tick(currentInputState);
	}

	SAILOR_PROFILE_END_BLOCK();

	timer.Stop();

	totalFramesCount++;

	if (timer.ResultAccumulatedMs() > 1000)
	{
		m_pureFps = totalFramesCount;
		totalFramesCount = 0;
		timer.Clear();
	}
}

EngineLoop::~EngineLoop()
{
	for (auto& world : m_worlds)
	{
		world->Clear();
	}
}