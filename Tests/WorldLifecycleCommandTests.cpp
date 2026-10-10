#include "Support/LifecyclePrefab.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "Engine/EngineLoop.h"
#include "RHI/Renderer.h"
#include "Tasks/Tasks.h"

#include <iostream>
#include <stdexcept>
#include <string_view>

using namespace Sailor;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	void TestWorldLifecycle(bool bEditor, bool bLatePrefab)
	{
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto* renderer = App::GetSubmodule<RHI::Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto world = engine->CreateEmptyWorld("Lifecycle frame test",
			bEditor ? EngineLoop::EditorWorldMask : EngineLoop::DefaultWorldMask);
		auto source = PrefabPtr::Make(world->GetAllocator(), FileId::CreateNewFileId());
		const auto cleanup = [&]
		{
			renderer->WaitIdle();
			engine->ExitWorld(world.GetRawPtr());
			engine->ProcessPendingWorldExits();
			source.DestroyObject(world->GetAllocator());
		};
		try
		{
			FrameState previous;
			const auto tick = [&]
			{
				const auto before = world->GetCurrentFrame();
				FrameState frame(world.GetRawPtr(), previous.GetTime() + 16, {}, { 32, 24 }, before ? &previous : nullptr);
				engine->ProcessCpuFrame(frame);
				frame.GetDrawImGuiTask()->Wait();
				Require(world->GetCurrentFrame() == before + 1 && frame.GetCommandBuffer(0) == world->GetCommandList(),
					"the lifecycle test must run a real world frame and retain its recorded commands");
				Require(renderer->PushFrame(frame), "the renderer must consume the lifecycle frame's commands");
				scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
				scheduler->ProcessTasksOnMainThread();
				renderer->WaitIdle();
				previous = std::move(frame);
			};
			auto lateOwner = world->Instantiate("Late component owner");
			if (bLatePrefab) tick();
			source->Deserialize(Tests::MakeLifecyclePrefabDocument());
			std::string diagnostic;
			Require(source->ValidateForInstantiation(diagnostic), diagnostic);
			const auto sourceBefore = YAML::Dump(source->Serialize());
			auto root = world->Instantiate(source);
			Require(root && root->GetChildren().Num() == 1, "the actual world must instantiate the whole prefab");
			auto child = root->GetChildren()[0];
			auto rootComponent = root->GetComponent<LifecycleTestComponent>();
			auto childComponent = child->GetComponent<LifecycleTestComponent>();
			Require(rootComponent && childComponent && rootComponent->m_bPublishedAtInitialize &&
				childComponent->m_bPublishedAtInitialize && rootComponent->GetSlotValue() == 31 &&
				childComponent->GetSlotValue() == 47 && rootComponent->m_begins == 0 && childComponent->m_begins == 0,
				"reflection must use initialized slots without starting gameplay before the world tick");
			Require(world->IsPrefabLinked(child->GetInstanceId()) == bEditor,
				"only editor worlds may retain prefab authoring links");
			tick();
			if (bEditor)
			{
				Require(rootComponent->m_begins == 0 && childComponent->m_begins == 0 &&
					rootComponent->m_ticks == 0 && childComponent->m_ticks == 0 &&
					rootComponent->m_editorTicks == 1 && childComponent->m_editorTicks == 1,
					"the real editor frame must call EditorTick without starting gameplay");
			}
			else
			{
				Require(rootComponent->m_begins == 1 && childComponent->m_begins == 1 &&
					rootComponent->m_ticks == 0 && childComponent->m_ticks == 0 &&
					rootComponent->m_bValidAtBegin && childComponent->m_bValidAtBegin,
					"the first real gameplay frame must begin each component once, before its first Tick");
				Require(rootComponent->m_valueAtBegin == 31 && childComponent->m_valueAtBegin == 47 &&
					rootComponent->m_dependencyAtBegin == childComponent && childComponent->m_dependencyAtBegin == rootComponent &&
					!rootComponent->m_parentAtBegin && childComponent->m_parentAtBegin == root &&
					rootComponent->m_positionAtBegin == glm::vec4(10, 2, 3, 1) &&
					childComponent->m_positionAtBegin == glm::vec4(11, 2, 3, 1),
					"BeginPlay must see hydrated properties, both internal references, transforms and parent");
			}
			tick();
			Require(rootComponent->m_begins == (bEditor ? 0u : 1u) &&
				childComponent->m_begins == (bEditor ? 0u : 1u) &&
				rootComponent->m_ticks == (bEditor ? 0u : 1u) && childComponent->m_ticks == (bEditor ? 0u : 1u),
				"the next world frame must not repeat BeginPlay or tick gameplay in the editor");

			auto typed = lateOwner->AddComponent<LifecycleTestComponent>();
			auto raw = TObjectPtr<LifecycleTestComponent>::Make(world->GetAllocator());
			Require(static_cast<bool>(lateOwner->AddComponentRaw(raw)), "the raw component must attach to its owner");
			typed->SetValue(59);
			raw->SetValue(61);
			typed->m_dependency = raw;
			raw->m_dependency = typed;
			Require(typed->m_bPublishedAtInitialize && raw->m_bPublishedAtInitialize &&
				typed->GetSlotValue() == 59 && raw->GetSlotValue() == 61 && typed->m_begins == 0 && raw->m_begins == 0,
				"late typed/raw components must initialize immediately and wait for the next frame to begin");
			tick();
			Require(typed->m_begins == (bEditor ? 0u : 1u) && raw->m_begins == (bEditor ? 0u : 1u) &&
				typed->m_ticks == 0 && raw->m_ticks == 0 &&
				typed->m_editorTicks == (bEditor ? 1u : 0u) && raw->m_editorTicks == (bEditor ? 1u : 0u),
				"late typed/raw components must follow the same first-frame lifecycle as prefab components");
			if (!bEditor)
				Require(typed->m_valueAtBegin == 59 && raw->m_valueAtBegin == 61 &&
					typed->m_dependencyAtBegin == raw && raw->m_dependencyAtBegin == typed &&
					typed->m_bValidAtBegin && raw->m_bValidAtBegin,
					"late BeginPlay must see assigned values and both references");
			tick();
			Require(typed->m_begins == (bEditor ? 0u : 1u) && raw->m_begins == (bEditor ? 0u : 1u) &&
				typed->m_ticks == (bEditor ? 0u : 1u) && raw->m_ticks == (bEditor ? 0u : 1u),
				"late components must begin only once and tick on the following gameplay frame");

			auto* transforms = world->GetECS<TransformECS>();
			const auto childId = child->GetInstanceId();
			const auto childIndex = transforms->GetComponentIndex(&child->GetTransformComponent());
			const auto ended = LifecycleTestComponent::s_ended;
			if (bEditor)
			{
				Require(!root->AddComponent<LifecycleTestComponent>() && !child->RemoveComponent(childComponent),
					"editor-linked prefab components must reject structural edits");
				child->SetParent(lateOwner);
				world->DestroyImmediate(child);
				world->Destroy(child);
				tick();
				Require(child && childComponent && child->GetParent() == root && root->GetChildren().Num() == 1 &&
					world->GetObjectByInstanceId(childId) == child && transforms->IsComponentRegistered(childIndex) &&
					root->GetTransformComponent().GetChildren().Num() == 1 && LifecycleTestComponent::s_ended == ended,
					"an actual editor frame must preserve a linked child after rejected reparent and delete requests");
			}
			else
			{
				bool bRequested = false;
				rootComponent->m_onTick = [&]
				{
					if (bRequested) return;
					bRequested = true;
					world->Destroy(child);
					world->Destroy(child);
					Require(child && childComponent && child->GetParent() == root && root->GetChildren().Num() == 1 &&
						world->GetObjectByInstanceId(childId) == child && LifecycleTestComponent::s_ended == ended,
						"deferred destruction must not remove the child inside its parent's Tick callback");
				};
				const auto rootTicks = rootComponent->m_ticks;
				tick();
				Require(bRequested && !child && !childComponent && !world->GetObjectByInstanceId(childId) &&
					root->GetChildren().IsEmpty() && root->GetTransformComponent().GetChildren().IsEmpty() &&
					!transforms->IsComponentRegistered(childIndex) && LifecycleTestComponent::s_ended == ended + 1,
					"World::Tick must drain deferred child destruction and unlink both surviving parent hierarchies");
				rootComponent->m_onTick = {};
				tick();
				Require(rootComponent->m_ticks == rootTicks + 2 && rootComponent->m_begins == 1 &&
					LifecycleTestComponent::s_ended == ended + 1,
					"the surviving parent must tick normally without repeated child cleanup");
			}
			Require(YAML::Dump(source->Serialize()) == sourceBefore,
				"world lifecycle and gameplay destruction must leave the source prefab unchanged");
			std::cout << "World lifecycle editor=" << bEditor << " latePrefab=" << bLatePrefab
				<< ": real frames, hydrated activation, typed/raw late components, prefab policy and deferred hierarchy passed\n";
		}
		catch (...)
		{
			cleanup();
			throw;
		}
		cleanup();
	}
}

namespace Sailor::Tests
{
	void RunWorldLifecycleCommandTests()
	{
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto* renderer = App::GetSubmodule<RHI::Renderer>();
		Require(engine->GetWorlds().IsEmpty() && renderer->EnsureFrameGraph(),
			"the world lifecycle fixture requires the completed preceding engine-loop tests");
		auto graph = renderer->GetFrameGraph()->GetRHI();
		const auto originalNodes = graph->GetGraph();
		graph->GetGraph().Clear();
		try
		{
			for (bool bEditor : { false, true })
				for (bool bLatePrefab : { false, true }) TestWorldLifecycle(bEditor, bLatePrefab);
		}
		catch (...)
		{
			graph->GetGraph() = originalNodes;
			throw;
		}
		graph->GetGraph() = originalNodes;
	}
}
