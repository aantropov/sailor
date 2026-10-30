#include "Sailor.h"
#include "Editor/EditorRuntimeBridge.h"
#include "Editor/EditorScene.h"
#include "AssetRegistry/Prefab/PrefabImporter.h"
#include "Components/CameraComponent.h"
#include "Components/CollisionShapeComponent.h"
#include "Editor/EditorViewportEvent.h"
#include "EditorEngineProtocolLifecycle.h"
#include "ECS/TransformECS.h"
#include "ECS/CameraECS.h"
#include "Engine/EngineLoop.h"
#include "Engine/Frame.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "RHI/Renderer.h"
#include "Submodules/Editor.h"
#include "Submodules/ImGuiApi.h"
#include "Support/EditorProtocolWire.h"

#include <ImGuizmo.h>
#include <algorithm>
#include <cmath>
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
			"selection must pass through protobuf, the editor bridge and the actual editor world");
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
		const auto selectionRevision = EditorRuntime::GetEditorManagedMutationRevision(1, nullptr);
		Require(selectionRevision == 2 && EditorRuntime::PullEditorViewportEvents(8).IsEmpty(),
			"managed selection commands must update revisions without echo events");
		Require(EditorRuntime::SetEditorViewportToolState(1, 1), "selection tool must be available");
		frame(center, false);
		frame(center, true);
		frame(center, false);
		Require(EditorRuntime::PullEditorViewportEvents(0).IsEmpty(), "a zero-sized pull must leave the queue intact");
		auto events = EditorRuntime::PullEditorViewportEvents(1);
		Require(events.Num() == 1 && world->IsEditorSelected(id), "a real viewport click must pick the object");
		const auto* selected = std::get_if<SelectionEvent>(&events[0].m_payload);
		Require(selected && selected->m_instanceId == id && events[0].m_managedMutationRevision == selectionRevision,
			"queued selection must retain the selected identity and managed revision");
		const uint64_t selectionEventRevision = events[0].m_revision;

		Require(EditorRuntime::SetEditorViewportToolState(2, 2), "local translation tool must be available");
		const auto before = object->GetTransformComponent().GetTransform();
		const auto objectRevision = EditorRuntime::GetEditorManagedMutationRevision(2, id.ToString().c_str());
		frame(center, false);
		frame(center, true);
		frame(center + glm::vec2(60, 20), true);
		Require(EditorRuntime::PullEditorViewportEvents(8).IsEmpty(), "an active drag must not emit intermediate undo records");
		frame(center + glm::vec2(60, 20), false);
		events = EditorRuntime::PullEditorViewportEvents(8);
		Require(events.Num() == 1, "releasing the gizmo must emit exactly one transform event");
		const auto* transform = std::get_if<TransformEvent>(&events[0].m_payload);
		Require(transform && transform->m_changes.Num() == 1 && transform->m_changes[0].m_instanceId == id && transform->m_operation == ETransformOperation::Translate &&
			transform->m_space == ETransformSpace::Local && events[0].m_revision == selectionEventRevision + 1 &&
			events[0].m_managedMutationRevision == selectionRevision && transform->m_changes[0].m_managedMutationRevision == objectRevision && transform->m_changes[0].m_before.m_position == before.m_position &&
			glm::distance(transform->m_changes[0].m_before.m_position, transform->m_changes[0].m_after.m_position) > 0.01f,
			"the real gizmo must publish its initial/final transforms and drag-start revision");

		for (const auto& value : { transform->m_changes[0].m_before, transform->m_changes[0].m_after })
		{
			Prefab::ReflectedGameObject reflected;
			reflected.m_name = object->GetName();
			reflected.m_mobilityType = object->GetMobilityType();
			reflected.m_position = value.m_position;
			reflected.m_rotation = value.GetRotation();
			reflected.m_scale = value.m_scale;
			const auto yaml = YAML::Dump(reflected.Serialize());
			Require(EditorRuntime::UpdateEditorObject(id.ToString().c_str(), yaml.c_str()), "undo/redo must apply through the editor object command");
			Require(object->GetTransformComponent().GetTransform().m_position == value.m_position,
				"undo/redo must restore the event's before/after position");
		}
		Require(EditorRuntime::GetEditorManagedMutationRevision(2, id.ToString().c_str()) == objectRevision + 2 &&
			EditorRuntime::PullEditorViewportEvents(8).IsEmpty(), "undo/redo must advance object revisions without duplicating the drag event");
		Require(EditorRuntime::SetEditorViewportToolState(1, 1), "selection tool must be restored after a drag");
		frame({ 1, 1 }, false);
		frame({ 1, 1 }, true);
		frame({ 1, 1 }, false);
		events = EditorRuntime::PullEditorViewportEvents(8);
		Require(events.Num() == 1 && !world->IsEditorSelected(id), "a click outside geometry must clear selection");
		selected = std::get_if<SelectionEvent>(&events[0].m_payload);
		Require(selected && !selected->m_instanceId && events[0].m_revision == selectionEventRevision + 2,
			"selection clear must remain an explicit event after the completed drag");
		std::cout << "Viewport events: protobuf selection, real pick/drag, ordered typed queue and undo/redo passed\n";

		object->GetTransformComponent().SetPosition({ 0, 0, -10 });
		auto first = world->Instantiate("Shift selection A");
		auto second = world->Instantiate("Shift selection B");
		const auto firstId = first->GetInstanceId();
		const auto secondId = second->GetInstanceId();
		SelectThroughProtocol({ secondId, firstId });
		frame(center, false);
		for (uint32_t click = 0; click < 3; ++click)
		{
			io.AddKeyEvent(ImGuiMod_Shift, true);
			frame(center, true);
			io.AddKeyEvent(ImGuiMod_Shift, false);
			frame(center, false);
		}
		io.AddKeyEvent(ImGuiMod_Shift, true);
		frame({ 1, 1 }, false);
		frame({ 1, 1 }, true);
		frame({ 1, 1 }, false);
		io.AddKeyEvent(ImGuiMod_Shift, false);
		frame(center, false);
		frame(center, true);
		io.AddKeyEvent(ImGuiMod_Shift, true);
		frame(center, false);
		events = EditorRuntime::PullEditorViewportEvents(8);
		Require(events.Num() == 5, "rapid native Shift clicks must each produce an ordered selection snapshot");
		for (uint32_t click = 0; click < 5; ++click)
		{
			const auto* snapshot = std::get_if<SelectionEvent>(&events[click].m_payload);
			Require(snapshot, "the queued native click must remain a selection event");
			const auto& ids = snapshot->m_selectedInstanceIds;
			if (click < 4)
			{
				const size_t expectedSize = click == 1 ? 2 : 3;
				Require(snapshot->m_bAdditive && ids.Num() == expectedSize && ids[0] == secondId && ids[1] == firstId,
					"Shift selection must preserve managed B,A order and the modifier captured on press after Shift is released");
				if (expectedSize == 3)
				{
					Require(ids[2] == id, "a new or re-added pick must remain the last primary in its retained snapshot");
				}
			}
			else
			{
				Require(!snapshot->m_bAdditive && ids.Num() == 1 && ids[0] == id,
					"a plain press must replace selection even when Shift becomes held before release");
			}
		}
		Require(!std::get<SelectionEvent>(events[3].m_payload).m_instanceId,
			"Shift-empty must retain the full group while reporting that no object was hit");
		Require(world->IsEditorSelected(id) && !world->IsEditorSelected(firstId) && !world->IsEditorSelected(secondId),
			"the last normal native click must replace the actual world selection");
		io.AddKeyEvent(ImGuiMod_Shift, false);
		frame(center, true);
		SelectThroughProtocol({ secondId, firstId });
		frame(center, false);
		Require(EditorRuntime::PullEditorViewportEvents(8).IsEmpty(),
			"a managed selection mutation must cancel an older native click before it can publish a stale pick");
		// Exercise every operation through the real ImGui input queue, rather than
		// calling the matrix helper or manufacturing a completion event.
		for (const uint32_t operation : { 2u, 3u, 4u })
		{
			first->GetTransformComponent().SetPosition({ -2, 0, -10 });
			second->GetTransformComponent().SetPosition({ 2, 0, -10 });
			for (auto target : { first, second })
			{
				target->GetTransformComponent().SetRotation(glm::identity<glm::quat>());
				target->GetTransformComponent().SetScale(glm::vec4(1));
			}
			Require(EditorRuntime::SetEditorSelection({ firstId, secondId }) && EditorRuntime::SetEditorViewportToolState(operation, 1, 2),
				"each group transform tool must accept Center/World state");
			const auto groupSelectionRevision = EditorRuntime::GetEditorManagedMutationRevision(1, nullptr);
			const auto beforeFirst = first->GetTransformComponent().GetTransform();
			const auto beforeSecond = second->GetTransformComponent().GetTransform();
			glm::vec2 press = center;
			glm::vec2 release = center + glm::vec2(60, 20);
			frame(press, false);
			if (operation == 3)
			{
				bool foundRing = false;
				for (float radius = 15; radius < std::min(io.DisplaySize.x, io.DisplaySize.y) * 0.4f; radius += 1.0f)
				{
					press = center + glm::vec2(radius * 0.70710678f);
					frame(press, false);
					if (ImGuizmo::IsOver(ImGuizmo::ROTATE))
					{
						foundRing = true;
						break;
					}
				}
				Require(foundRing, "a real rotation ring must be reachable in the native viewport");
				const glm::vec2 offset = press - center;
				release = center + glm::vec2(-offset.y, offset.x);
			}
			frame(press, true);
			Require(ImGuizmo::IsUsing(), "the pointer press must begin an actual native group gizmo gesture");
			frame(release, true);
			Require(EditorRuntime::PullEditorViewportEvents(8).IsEmpty(), "group gestures must not publish intermediate history");
			frame(release, false);
			events = EditorRuntime::PullEditorViewportEvents(8);
			Require(events.Num() == 1 && events[0].m_managedMutationRevision == groupSelectionRevision,
				"one group completion must retain its captured selection revision");
			const auto* group = std::get_if<TransformEvent>(&events[0].m_payload);
			Require(group && group->m_changes.Num() == 2 && group->m_changes[0].m_instanceId == firstId && group->m_changes[1].m_instanceId == secondId &&
				group->m_pivotMode == ETransformPivotMode::Center && static_cast<uint32_t>(group->m_operation) + 1 == operation,
				"real group completion must contain every ordered root and the chosen tool/pivot");
			Require(group->m_changes[0].m_before.m_position == beforeFirst.m_position && group->m_changes[1].m_before.m_position == beforeSecond.m_position,
				"group events must capture the initial locals exactly once on press");
			for (const auto& change : group->m_changes)
			{
				Require(change.m_managedMutationRevision == EditorRuntime::GetEditorManagedMutationRevision(2, change.m_instanceId.ToString().c_str()),
					"each root must carry its own managed object revision");
				if (operation == 2)
				{
					Require(glm::distance(change.m_before.m_position, change.m_after.m_position) > 0.01f, "real translate must move every root");
				}
				else if (operation == 3)
				{
					Require(std::abs(glm::dot(change.m_before.GetRotation(), change.m_after.GetRotation())) < 0.99f &&
						glm::distance(change.m_before.m_position, change.m_after.m_position) > 0.01f, "real Center rotation must rotate and orbit every root");
				}
				else
				{
					Require(glm::distance(change.m_before.m_scale, change.m_after.m_scale) > 0.01f &&
						glm::distance(change.m_before.m_position, change.m_after.m_position) > 0.01f, "real Center scale must scale and spread every root");
				}
			}
			// A stale managed edit on B must leave B untouched while reclaiming A's
			// unpublished native movement. A later native movement also fences A.
			if (operation == 2)
			{
				const auto changes = group->m_changes;
				const auto laterA = first->GetTransformComponent().GetPosition() + glm::vec4(1, 0, 0, 0);
				first->GetTransformComponent().SetPosition(glm::vec3(laterA));
				const auto laterB = second->GetTransformComponent().GetPosition() + glm::vec4(0, 1, 0, 0);
				second->GetTransformComponent().SetPosition(glm::vec3(laterB));
				App::GetSubmodule<Editor>()->NotifyManagedObjectMutation(secondId);
				Require(EditorRuntime::RestoreEditorViewportTransform(changes) && first->GetTransformComponent().GetPosition() == laterA &&
					second->GetTransformComponent().GetPosition() == laterB, "rollback must fence both newer native transforms and managed object revisions");
				first->GetTransformComponent().SetPosition(glm::vec3(changes[0].m_after.m_position));
				Require(EditorRuntime::RestoreEditorViewportTransform(changes) &&
					first->GetTransformComponent().GetPosition() == beforeFirst.m_position && second->GetTransformComponent().GetPosition() == laterB,
					"conditional rollback must restore only roots that still match the complete expected-after state");
			}
		}
		std::cout << "Viewport group transforms: actual ImGui translate/rotate/scale, one ordered completion, per-root revisions and conditional rollback passed\n";
		// Play must snapshot the restored start locals, and Stop must retain
		// ordered selection so the active Pivot remains the last selected root.
		first->GetTransformComponent().SetPosition({ -2, 0, -10 });
		second->GetTransformComponent().SetPosition({ 2, 0, -10 });
		for (auto target : { first, second })
		{
			target->GetTransformComponent().SetRotation(glm::identity<glm::quat>());
			target->GetTransformComponent().SetScale(glm::vec4(1));
		}
		Require(EditorRuntime::SetEditorSelection({ secondId, firstId }) && EditorRuntime::SetEditorViewportToolState(2, 1, 2),
			"the Start/Stop fixture must select a group in reverse world order");
		frame(center, false);
		frame(center, true);
		frame(center + glm::vec2(60, 20), true);
		Require(glm::distance(glm::vec3(first->GetTransformComponent().GetPosition()), glm::vec3(-2, 0, -10)) > 0.01f &&
			EditorRuntime::PullEditorViewportEvents(8).IsEmpty(), "Start must interrupt an actual unpublished group drag");
		Require(EditorRuntime::SetEditorSimulationEnabled(true) &&
			glm::distance(glm::vec3(first->GetTransformComponent().GetPosition()), glm::vec3(-2, 0, -10)) < 0.001f &&
			glm::distance(glm::vec3(second->GetTransformComponent().GetPosition()), glm::vec3(2, 0, -10)) < 0.001f,
			"Start must restore every root before saving the simulation baseline");
		frame(center + glm::vec2(60, 20), false);
		Require(EditorRuntime::SetEditorSimulationEnabled(false), "Stop must restore the original group baseline");
		world = engine->GetWorld();
		first = world->GetObjectByInstanceId(firstId).DynamicCast<GameObject>();
		second = world->GetObjectByInstanceId(secondId).DynamicCast<GameObject>();
		object = world->GetObjectByInstanceId(id).DynamicCast<GameObject>();
		Require(first && second && object &&
			glm::distance(glm::vec3(first->GetTransformComponent().GetPosition()), glm::vec3(-2, 0, -10)) < 0.001f &&
			glm::distance(glm::vec3(second->GetTransformComponent().GetPosition()), glm::vec3(2, 0, -10)) < 0.001f &&
			EditorRuntime::PullEditorViewportEvents(8).IsEmpty(), "Stop must not resurrect the canceled group drag or its undo event");
		Require(EditorRuntime::SetEditorViewportToolState(2, 1, 1), "the restored group must expose its active Pivot");
		for (auto target : world->GetGameObjects())
		{
			if (auto camera = target->GetComponent<CameraComponent>())
			{
				target->GetTransformComponent().SetPosition(glm::vec3(0));
				target->GetTransformComponent().SetRotation(glm::identity<glm::quat>());
				camera->SetFov(90);
			}
		}
		frame({ 1, 1 }, false);
		Math::Transform restoredCamera;
		CameraData restoredCameraData;
		Require(world->GetECS<CameraECS>()->TryGetActiveCamera(restoredCamera, restoredCameraData), "the restored scene must retain its editor camera");
		const auto activeClip = restoredCameraData.GetProjectionMatrix() * restoredCameraData.GetViewMatrix() * glm::vec4(-2, 0, -10, 1);
		const glm::vec2 activePointer((activeClip.x / activeClip.w + 1.0f) * io.DisplaySize.x * 0.5f,
			(1.0f - activeClip.y / activeClip.w) * io.DisplaySize.y * 0.5f);
		frame(activePointer, false);
		frame(activePointer, true);
		Require(ImGuizmo::IsUsing(), "after Stop the common Pivot must remain at last-selected A, not world-first or hash-first selection");
		frame(activePointer + glm::vec2(30, 10), true);
		frame(activePointer + glm::vec2(30, 10), false);
		events = EditorRuntime::PullEditorViewportEvents(8);
		const auto* orderedAfterStop = events.Num() == 1 ? std::get_if<TransformEvent>(&events[0].m_payload) : nullptr;
		Require(orderedAfterStop && orderedAfterStop->m_changes.Num() == 2 &&
			orderedAfterStop->m_changes[0].m_instanceId == secondId && orderedAfterStop->m_changes[1].m_instanceId == firstId,
			"Stop must preserve B,A selection order in the next actual group completion");
		std::cout << "Viewport group Start/Stop: canceled group baseline, ordered selection and last-selected active Pivot passed\n";
		world->DestroyImmediate(first);
		world->DestroyImmediate(second);
		std::cout << "Viewport Shift selection: captured press modifier, full ordered snapshots, rapid toggle/remove/re-add, empty pick and stale click cancellation passed\n";

		Require(EditorRuntime::SetEditorSelection({ id }) && EditorRuntime::SetEditorViewportToolState(2, 2) &&
			EditorRuntime::SetEditorSimulationEnabled(true), "the real drag fixture must enter simulation");
		const auto savedPosition = object->GetTransformComponent().GetPosition();
		frame(center, false);
		frame(center, true);
		frame(center + glm::vec2(60, 20), true);
		Require(glm::distance(object->GetTransformComponent().GetPosition(), savedPosition) > 0.01f &&
			EditorRuntime::PullEditorViewportEvents(8).IsEmpty(), "simulation must have an active, unpublished gizmo drag before Stop");
		Require(EditorRuntime::SetEditorSimulationEnabled(false), "Stop must replace the world while the mouse is still held");
		Require(!object && world->GetGameObjects().IsEmpty(), "the unfinished drag must not retain live objects from the old world");
		world = engine->GetWorld();
		object = world->GetObjectByInstanceId(id).DynamicCast<GameObject>();
		Require(object && object->GetTransformComponent().GetPosition() == savedPosition && world->IsEditorSelected(id),
			"Stop must restore the pre-simulation transform and selection after an unfinished drag");
		frame(center + glm::vec2(60, 20), false);
		Require(EditorRuntime::PullEditorViewportEvents(8).IsEmpty() &&
			object->GetTransformComponent().GetPosition() == savedPosition,
			"mouse release in the restored world must not commit a stale drag or emit a stale undo event");
		std::cout << "Viewport simulation: active real gizmo drag is discarded on Stop; release cannot mutate the restored world\n";
	}
}
