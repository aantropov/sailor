#include "Sailor.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Components/CameraComponent.h"
#include "Components/CollisionShapeComponent.h"
#include "Editor/EditorViewportEvent.h"
#include "EditorEngineProtocolLifecycle.h"
#include "ECS/TransformECS.h"
#include "Engine/EngineLoop.h"
#include "Engine/Frame.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "RHI/Renderer.h"
#include "Submodules/Editor.h"
#include "Submodules/ImGuiApi.h"
#include "Support/EditorProtocolWire.h"

#include <iostream>
#include <stdexcept>
#include <string_view>

extern "C" SAILOR_SHARED_API void SailorProtocolFreeBuffer(uint8_t* buffer) noexcept;

using namespace Sailor;

namespace
{
	void Require(bool condition, std::string_view message)
	{
		if (!condition) throw std::runtime_error(std::string(message));
	}

	void SelectThroughProtocol(const TVector<InstanceId>& selection)
	{
		std::string payload;
		for (const auto& id : selection) Tests::ProtocolWire::AppendBytesField(payload, 1, id.ToString());
		const auto request = Tests::ProtocolWire::MakeRequest(1, 43, payload);
		Protocol::TEditorEngineProtocolLifecycleGate gate;
		std::string error;
		Require(gate.TryBeginInitialization(error), "selection protocol must admit the initialized native App");
		gate.CompleteInitialization(true);
		Protocol::EditorEngineProtocolDependencies dependencies{};
		dependencies.m_lifecycleGate = &gate;
		uint8_t* data = nullptr;
		uint32_t size = 0;
		const auto status = Protocol::InvokeEditorEngineProtocol(reinterpret_cast<const uint8_t*>(request.data()),
			static_cast<uint32_t>(request.size()), &data, &size, dependencies);
		const std::string bytes(reinterpret_cast<const char*>(data), size);
		SailorProtocolFreeBuffer(data);
		Tests::ProtocolWire::TProtocolResponseWire response;
		Require(status == static_cast<int32_t>(Protocol::EEditorEngineTransportStatus::Ok) &&
			Tests::ProtocolWire::ParseResponse(bytes, response) && response.m_bSuccess &&
			response.m_resultField == 11 && response.m_resultPayload == std::string("\x08\x01", 2),
			"selection must pass through protobuf, App and the actual editor world");
	}
}

namespace Sailor::Tests
{
	void RunEditorViewportCommandTests()
	{
		using namespace EditorViewport;
		ImGui::SetCurrentContext(ImGuiApi::GetCurrentContext());
		auto& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.AddFocusEvent(true);
		auto* engine = App::GetSubmodule<EngineLoop>();
		auto world = engine->GetWorld();
		Require(world && world.GetRawPtr() == App::GetSubmodule<Editor>()->GetWorld(),
			"viewport events require the actual active editor world");
		for (auto object : world->GetGameObjects())
		{
			if (auto camera = object->GetComponent<CameraComponent>())
			{
				object->GetTransformComponent().SetPosition(glm::vec3(0));
				object->GetTransformComponent().SetRotation(glm::identity<glm::quat>());
				camera->SetFov(90);
			}
		}
		auto object = world->Instantiate("Viewport event target");
		object->GetTransformComponent().SetPosition({ 0, 0, -10 });
		auto shape = object->AddComponent<CollisionShapeComponent>();
		shape->SetSize(glm::vec3(2));
		const auto id = object->GetInstanceId();
		uint64_t time = 0;
		auto frame = [&](glm::vec2 pointer, bool bPressed)
		{
			io.AddMousePosEvent(pointer.x, pointer.y);
			io.AddMouseButtonEvent(ImGuiMouseButton_Left, bPressed);
			FrameState state(world.GetRawPtr(), time += 16, {}, {});
			engine->ProcessCpuFrame(state);
			state.GetDrawImGuiTask()->Wait();
			for (const uint32_t i : { 0u, 1u })
			{
				Require(RHI::Renderer::GetDriver()->SubmitCommandList_Immediate(state.GetCommandBuffer(i)),
					"the interaction frame must submit its real world and ImGui updates");
			}
		};
		frame({ 1, 1 }, false);
		const glm::vec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
		SelectThroughProtocol({ id, shape->GetInstanceId(), id, InstanceId::Invalid });
		Require(world->IsEditorSelected(id), "typed selection must normalize component IDs and duplicates");
		SelectThroughProtocol({});
		Require(!world->IsEditorSelected(id), "an empty protobuf selection must clear the world selection");
		const auto selectionRevision = App::GetEditorManagedMutationRevision(1, nullptr);
		Require(selectionRevision == 2 && App::PullEditorViewportEvents(8).IsEmpty(),
			"managed selection commands must update revisions without echo events");
		Require(App::SetEditorViewportToolState(1, 1), "selection tool must be available");
		frame(center, false);
		frame(center, true);
		frame(center, false);
		Require(App::PullEditorViewportEvents(0).IsEmpty(), "a zero-sized pull must leave the queue intact");
		auto events = App::PullEditorViewportEvents(1);
		Require(events.Num() == 1 && world->IsEditorSelected(id), "a real viewport click must pick the object");
		const auto* selected = std::get_if<SelectionEvent>(&events[0].m_payload);
		Require(selected && selected->m_instanceId == id && events[0].m_managedMutationRevision == selectionRevision,
			"queued selection must retain the selected identity and managed revision");
		const uint64_t selectionEventRevision = events[0].m_revision;

		Require(App::SetEditorViewportToolState(2, 2), "local translation tool must be available");
		const auto before = object->GetTransformComponent().GetTransform();
		const auto objectRevision = App::GetEditorManagedMutationRevision(2, id.ToString().c_str());
		frame(center, false);
		frame(center, true);
		frame(center + glm::vec2(60, 20), true);
		Require(App::PullEditorViewportEvents(8).IsEmpty(), "an active drag must not emit intermediate undo records");
		frame(center + glm::vec2(60, 20), false);
		events = App::PullEditorViewportEvents(8);
		Require(events.Num() == 1, "releasing the gizmo must emit exactly one transform event");
		const auto* transform = std::get_if<TransformEvent>(&events[0].m_payload);
		Require(transform && transform->m_instanceId == id && transform->m_operation == ETransformOperation::Translate &&
			transform->m_space == ETransformSpace::Local && events[0].m_revision == selectionEventRevision + 1 &&
			events[0].m_managedMutationRevision == objectRevision && transform->m_before.m_position == before.m_position &&
			glm::distance(transform->m_before.m_position, transform->m_after.m_position) > 0.01f,
			"the real gizmo must publish its initial/final transforms and drag-start revision");

		for (const auto& value : { transform->m_before, transform->m_after })
		{
			Prefab::ReflectedGameObject reflected;
			reflected.m_name = object->GetName();
			reflected.m_mobilityType = object->GetMobilityType();
			reflected.m_position = value.m_position;
			reflected.m_rotation = value.GetRotation();
			reflected.m_scale = value.m_scale;
			const auto yaml = YAML::Dump(reflected.Serialize());
			Require(App::UpdateEditorObject(id.ToString().c_str(), yaml.c_str()), "undo/redo must apply through the editor object command");
			Require(object->GetTransformComponent().GetTransform().m_position == value.m_position,
				"undo/redo must restore the event's before/after position");
		}
		Require(App::GetEditorManagedMutationRevision(2, id.ToString().c_str()) == objectRevision + 2 &&
			App::PullEditorViewportEvents(8).IsEmpty(), "undo/redo must advance object revisions without duplicating the drag event");
		Require(App::SetEditorViewportToolState(1, 1), "selection tool must be restored after a drag");
		frame({ 1, 1 }, false);
		frame({ 1, 1 }, true);
		frame({ 1, 1 }, false);
		events = App::PullEditorViewportEvents(8);
		Require(events.Num() == 1 && !world->IsEditorSelected(id), "a click outside geometry must clear selection");
		selected = std::get_if<SelectionEvent>(&events[0].m_payload);
		Require(selected && !selected->m_instanceId && events[0].m_revision == selectionEventRevision + 2,
			"selection clear must remain an explicit event after the completed drag");
		std::cout << "Viewport events: protobuf selection, real pick/drag, ordered typed queue and undo/redo passed\n";

		object->GetTransformComponent().SetPosition({ 0, 0, -10 });
		Require(App::SetEditorSelection({ id }) && App::SetEditorViewportToolState(2, 2) &&
			App::SetEditorSimulationEnabled(true), "the real drag fixture must enter simulation");
		const auto savedPosition = object->GetTransformComponent().GetPosition();
		frame(center, false);
		frame(center, true);
		frame(center + glm::vec2(60, 20), true);
		Require(glm::distance(object->GetTransformComponent().GetPosition(), savedPosition) > 0.01f &&
			App::PullEditorViewportEvents(8).IsEmpty(), "simulation must have an active, unpublished gizmo drag before Stop");
		Require(App::SetEditorSimulationEnabled(false), "Stop must replace the world while the mouse is still held");
		Require(!object && world->GetGameObjects().IsEmpty(), "the unfinished drag must not retain live objects from the old world");
		world = engine->GetWorld();
		object = world->GetObjectByInstanceId(id).DynamicCast<GameObject>();
		Require(object && object->GetTransformComponent().GetPosition() == savedPosition && world->IsEditorSelected(id),
			"Stop must restore the pre-simulation transform and selection after an unfinished drag");
		frame(center + glm::vec2(60, 20), false);
		Require(App::PullEditorViewportEvents(8).IsEmpty() &&
			object->GetTransformComponent().GetPosition() == savedPosition,
			"mouse release in the restored world must not commit a stale drag or emit a stale undo event");
		std::cout << "Viewport simulation: active real gizmo drag is discarded on Stop; release cannot mutate the restored world\n";
	}
}
