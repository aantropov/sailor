#include "Editor/EditorViewportController.h"

#include "AssetRegistry/Model/ModelImporter.h"
#include "Components/EditorComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Core/YamlSerializable.h"
#include "ECS/CameraECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Sailor;
using namespace Sailor::EditorViewport;

namespace
{
	constexpr float c_clickSlop = 4.0f;
	constexpr float c_originFallbackExtent = 25.0f;
	constexpr float c_matrixTolerance = 0.001f;

	bool IsFiniteMatrix(const glm::mat4& matrix)
	{
		for (glm::length_t column = 0; column < matrix.length(); ++column)
		{
			for (glm::length_t row = 0; row < matrix[column].length(); ++row)
			{
				if (!std::isfinite(matrix[column][row]))
				{
					return false;
				}
			}
		}

		return true;
	}

	bool AreMatricesNear(const glm::mat4& lhs, const glm::mat4& rhs, float tolerance = c_matrixTolerance)
	{
		for (glm::length_t column = 0; column < lhs.length(); ++column)
		{
			for (glm::length_t row = 0; row < lhs[column].length(); ++row)
			{
				const float scale = std::max({ 1.0f, std::abs(lhs[column][row]), std::abs(rhs[column][row]) });
				if (std::abs(lhs[column][row] - rhs[column][row]) > tolerance * scale)
				{
					return false;
				}
			}
		}

		return true;
	}

	bool AreTransformsNear(const Math::Transform& lhs, const Math::Transform& rhs)
	{
		return AreMatricesNear(lhs.Matrix(), rhs.Matrix());
	}

	bool TryInvertTransformMatrix(const glm::mat4& matrix, glm::mat4& outInverse)
	{
		if (!IsFiniteMatrix(matrix))
		{
			return false;
		}

		const glm::vec3 axisX(matrix[0]);
		const glm::vec3 axisY(matrix[1]);
		const glm::vec3 axisZ(matrix[2]);
		const float axisLengthProduct = glm::length(axisX) * glm::length(axisY) * glm::length(axisZ);
		const float normalizedVolume = axisLengthProduct > 0.0f
			? std::abs(glm::dot(glm::cross(axisX, axisY), axisZ)) / axisLengthProduct
			: 0.0f;
		if (!std::isfinite(normalizedVolume) || normalizedVolume <= 0.000001f)
		{
			return false;
		}

		outInverse = glm::inverse(matrix);
		return IsFiniteMatrix(outInverse) &&
			AreMatricesNear(matrix * outInverse, glm::identity<glm::mat4>());
	}

	glm::mat4 CalculateCurrentWorldMatrix(const TObjectPtr<GameObject>& gameObject)
	{
		glm::mat4 worldMatrix = glm::identity<glm::mat4>();
		for (auto current = gameObject; current.IsValid(); current = current->GetParent())
		{
			worldMatrix = current->GetTransformComponent().GetTransform().Matrix() * worldMatrix;
		}

		return worldMatrix;
	}

	const char* ToString(ETransformOperation operation)
	{
		switch (operation)
		{
		case ETransformOperation::Select: return "Select";
		case ETransformOperation::Translate: return "Translate";
		case ETransformOperation::Rotate: return "Rotate";
		case ETransformOperation::Scale: return "Scale";
		default: return "Select";
		}
	}

	const char* ToString(ETransformSpace space)
	{
		return space == ETransformSpace::Local ? "Local" : "World";
	}

	ImGuizmo::OPERATION ToImGuizmoOperation(ETransformOperation operation)
	{
		switch (operation)
		{
		case ETransformOperation::Rotate: return ImGuizmo::ROTATE;
		case ETransformOperation::Scale: return ImGuizmo::SCALE;
		case ETransformOperation::Translate:
		default: return ImGuizmo::TRANSLATE;
		}
	}

	void DrawOperationButton(const char* label, ETransformOperation value, ETransformOperation& operation)
	{
		const bool bSelected = operation == value;
		if (bSelected)
		{
			ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.75f, 1.0f));
		}

		if (ImGui::Button(label))
		{
			operation = value;
		}

		if (bSelected)
		{
			ImGui::PopStyleColor();
		}
	}

	void ResetImGuizmoInteractionState()
	{
		ImGuizmo::Enable(false);
		ImGuizmo::Enable(true);
	}
}

bool EditorViewport::BuildWorldRay(
	float pointerX,
	float pointerY,
	float viewportWidth,
	float viewportHeight,
	const glm::mat4& view,
	const glm::mat4& projection,
	Math::Ray& outRay)
{
	if (!std::isfinite(pointerX) || !std::isfinite(pointerY) ||
		!std::isfinite(viewportWidth) || !std::isfinite(viewportHeight) ||
		viewportWidth <= 0.0f || viewportHeight <= 0.0f ||
		pointerX < 0.0f || pointerY < 0.0f ||
		pointerX > viewportWidth || pointerY > viewportHeight ||
		!IsFiniteMatrix(view) || !IsFiniteMatrix(projection))
	{
		return false;
	}

	const glm::mat4 viewProjection = projection * view;
	glm::mat4 inverseViewProjection{};
	if (!TryInvertTransformMatrix(viewProjection, inverseViewProjection))
	{
		return false;
	}

	const float ndcX = (2.0f * pointerX / viewportWidth) - 1.0f;
	const float ndcY = 1.0f - (2.0f * pointerY / viewportHeight);

	// Sailor uses a Vulkan-style [0, 1] reversed-Z projection: near=1, far=0.
	glm::vec4 nearPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
	glm::vec4 farPoint = inverseViewProjection * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
	if (std::abs(nearPoint.w) <= std::numeric_limits<float>::epsilon() ||
		std::abs(farPoint.w) <= std::numeric_limits<float>::epsilon())
	{
		return false;
	}

	nearPoint /= nearPoint.w;
	farPoint /= farPoint.w;
	const glm::vec3 direction = glm::vec3(farPoint - nearPoint);
	const float directionLength = glm::length(direction);
	if (!Math::AllFinite(nearPoint) || !Math::AllFinite(farPoint) ||
		!std::isfinite(directionLength) || directionLength <= std::numeric_limits<float>::epsilon())
	{
		return false;
	}

	outRay = Math::Ray(glm::vec3(nearPoint), direction / directionLength);
	return true;
}

bool EditorViewport::TryPickNearest(
	const Math::Ray& ray,
	const TVector<PickCandidate>& candidates,
	InstanceId& outInstanceId,
	float& outDistance)
{
	outInstanceId = InstanceId::Invalid;
	outDistance = std::numeric_limits<float>::max();

	for (const auto& candidate : candidates)
	{
		if (!candidate.m_instanceId.IsGameObjectId() || !candidate.m_worldBounds.IsValid())
		{
			continue;
		}

		const float rawDistance = Math::IntersectRayAABB(
			ray,
			candidate.m_worldBounds.m_min,
			candidate.m_worldBounds.m_max);
		if (rawDistance == std::numeric_limits<float>::max())
		{
			continue;
		}

		const float distance = std::max(0.0f, rawDistance);
		const bool bCloser = distance + 0.0001f < outDistance;
		const bool bTie = std::abs(distance - outDistance) <= 0.0001f;
		if (bCloser || (bTie && (!outInstanceId || candidate.m_instanceId.ToString() < outInstanceId.ToString())))
		{
			outDistance = distance;
			outInstanceId = candidate.m_instanceId;
		}
	}

	return outInstanceId.IsGameObjectId();
}

bool EditorViewport::CanBeginSelectionGesture(
	bool bHasModifiers,
	bool bNavigatingViewport,
	bool bGizmoOwnsPointer,
	bool bEditorUiOwnsPointer)
{
	return !bHasModifiers &&
		!bNavigatingViewport &&
		!bGizmoOwnsPointer &&
		!bEditorUiOwnsPointer;
}

bool EditorViewport::ShouldCancelSelectionGesture(
	bool bGestureArmed,
	bool bHasModifiers,
	bool bNavigatingViewport)
{
	return bGestureArmed && (bHasModifiers || bNavigatingViewport);
}

bool EditorViewport::DoesSubmittedGizmoOwnPointer(
	bool bGizmoSubmitted,
	bool bIsOver,
	bool bIsUsing)
{
	return bGizmoSubmitted && (bIsOver || bIsUsing);
}

bool EditorViewport::TryConvertWorldToLocalTransform(
	const glm::mat4& worldMatrix,
	const glm::mat4* parentWorldMatrix,
	Math::Transform& outLocalTransform)
{
	if (!IsFiniteMatrix(worldMatrix))
	{
		return false;
	}

	glm::mat4 localMatrix = worldMatrix;
	if (parentWorldMatrix)
	{
		glm::mat4 inverseParent{};
		if (!TryInvertTransformMatrix(*parentWorldMatrix, inverseParent))
		{
			return false;
		}

		localMatrix = inverseParent * worldMatrix;
	}

	if (!IsFiniteMatrix(localMatrix))
	{
		return false;
	}

	Math::Transform localTransform = Math::Transform::FromMatrix(localMatrix);
	const glm::vec4 rotation(
		localTransform.m_rotation.x,
		localTransform.m_rotation.y,
		localTransform.m_rotation.z,
		localTransform.m_rotation.w);
	if (!Math::AllFinite(localTransform.m_position) ||
		!Math::AllFinite(rotation) ||
		!Math::AllFinite(localTransform.m_scale) ||
		!AreMatricesNear(localTransform.Matrix(), localMatrix))
	{
		return false;
	}

	outLocalTransform = localTransform;
	return true;
}

bool EditorViewport::ResolveGameObjectBounds(
	const TObjectPtr<GameObject>& gameObject,
	Math::AABB& outWorldBounds,
	bool& outUsesMeshBounds)
{
	outWorldBounds = {};
	outUsesMeshBounds = false;
	if (!gameObject || gameObject->GetComponent<EditorComponent>())
	{
		return false;
	}

	const glm::mat4 worldMatrix = CalculateCurrentWorldMatrix(gameObject);
	for (const auto& component : gameObject->GetComponents())
	{
		auto meshRenderer = component.DynamicCast<MeshRendererComponent>();
		const ModelPtr model = meshRenderer ? meshRenderer->GetModel() : ModelPtr{};
		if (!model || !model->IsReady() || !model->GetBoundsAABB().IsValid())
		{
			continue;
		}

		Math::AABB meshBounds = model->GetBoundsAABB();
		meshBounds.Apply(worldMatrix);
		if (!meshBounds.IsValid())
		{
			continue;
		}

		if (outUsesMeshBounds)
		{
			outWorldBounds.Extend(meshBounds);
		}
		else
		{
			outWorldBounds = meshBounds;
			outUsesMeshBounds = true;
		}
	}

	if (outUsesMeshBounds)
	{
		return true;
	}

	const glm::vec3 worldPosition(worldMatrix[3]);
	outWorldBounds = Math::AABB(worldPosition, glm::vec3(c_originFallbackExtent));
	return outWorldBounds.IsValid();
}

void EditorViewportController::Tick(World& world)
{
	const int32_t frame = ImGui::GetFrameCount();
	if (m_lastTickFrame == frame)
	{
		return;
	}
	m_lastTickFrame = frame;
	m_gizmoSubmittedThisFrame = false;
	if (ImGui::GetIO().AppFocusLost)
	{
		CancelInteraction(world);
		return;
	}

	DrawToolbar();

	const auto selectedObject = ResolveSelectedObject(world);
	TickTransformGizmo(world, selectedObject);
	if (!m_gizmoSubmittedThisFrame)
	{
		ResetImGuizmoInteractionState();
	}
	TickSelection(world);
}

void EditorViewportController::Reset()
{
	m_wasUsingGizmo = false;
	m_gizmoSubmittedThisFrame = false;
	m_selectionClickArmed = false;
	m_gizmoConsumedClick = false;
	m_selectionClickStart = {};
	m_dragInstanceId = InstanceId::Invalid;
	m_managedSelectionMutationRevision = 0;
	m_selectedObjectMutationRevision = 0;
	m_dragManagedObjectMutationRevision = 0;
	m_lastTickFrame = -1;
	m_pendingEvents.Clear();
	ResetImGuizmoInteractionState();
}

void EditorViewportController::CancelPointerInteraction()
{
	m_selectionClickArmed = false;
	m_gizmoConsumedClick = false;
}

void EditorViewportController::CancelInteraction(World& world)
{
	CancelPointerInteraction();
	if (m_wasUsingGizmo)
	{
		CompleteActiveTransform(world);
	}
	m_gizmoSubmittedThisFrame = false;
	ResetImGuizmoInteractionState();
}

bool EditorViewportController::PullEvent(std::string& outEvent)
{
	if (m_pendingEvents.IsEmpty())
	{
		return false;
	}

	outEvent = std::move(m_pendingEvents[0]);
	m_pendingEvents.RemoveAt(0);
	return true;
}

TObjectPtr<GameObject> EditorViewportController::ResolveSelectedObject(World& world) const
{
	for (auto gameObject : world.GetGameObjects())
	{
		if (gameObject &&
			world.IsEditorSelected(gameObject->GetInstanceId()) &&
			!gameObject->GetComponent<EditorComponent>())
		{
			return gameObject;
		}
	}

	return {};
}

void EditorViewportController::DrawToolbar()
{
	ImGuiIO& io = ImGui::GetIO();
	const bool bCanChangeTool = !m_wasUsingGizmo;
	const bool bCanUseHotkeys = bCanChangeTool && !io.WantTextInput && !io.MouseDown[1];
	if (bCanUseHotkeys)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Q, false)) m_operation = ETransformOperation::Select;
		if (ImGui::IsKeyPressed(ImGuiKey_W, false)) m_operation = ETransformOperation::Translate;
		if (ImGui::IsKeyPressed(ImGuiKey_E, false)) m_operation = ETransformOperation::Rotate;
		if (ImGui::IsKeyPressed(ImGuiKey_R, false)) m_operation = ETransformOperation::Scale;
		if (ImGui::IsKeyPressed(ImGuiKey_T, false))
		{
			m_space = m_space == ETransformSpace::World ? ETransformSpace::Local : ETransformSpace::World;
		}
	}

	ImGui::SetNextWindowPos(ImVec2(12.0f, 12.0f), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.85f);
	constexpr ImGuiWindowFlags flags =
		ImGuiWindowFlags_NoTitleBar |
		ImGuiWindowFlags_NoResize |
		ImGuiWindowFlags_NoMove |
		ImGuiWindowFlags_NoScrollbar |
		ImGuiWindowFlags_NoSavedSettings |
		ImGuiWindowFlags_AlwaysAutoResize |
		ImGuiWindowFlags_NoFocusOnAppearing |
		ImGuiWindowFlags_NoNav;

	if (ImGui::Begin("Scene transform##SailorSceneTransform", nullptr, flags))
	{
		ImGui::BeginDisabled(!bCanChangeTool);
		DrawOperationButton("Q Select", ETransformOperation::Select, m_operation);
		ImGui::SameLine();
		DrawOperationButton("W Move", ETransformOperation::Translate, m_operation);
		ImGui::SameLine();
		DrawOperationButton("E Rotate", ETransformOperation::Rotate, m_operation);
		ImGui::SameLine();
		DrawOperationButton("R Scale", ETransformOperation::Scale, m_operation);
		ImGui::SameLine();
		if (ImGui::Button(m_space == ETransformSpace::World ? "World (T)" : "Local (T)"))
		{
			m_space = m_space == ETransformSpace::World ? ETransformSpace::Local : ETransformSpace::World;
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered())
		{
			ImGui::SetTooltip("Hold Ctrl while dragging to snap");
		}
	}
	ImGui::End();
}

void EditorViewportController::CompleteActiveTransform(World& world)
{
	if (!m_dragInstanceId)
	{
		m_wasUsingGizmo = false;
		m_dragManagedObjectMutationRevision = 0;
		return;
	}

	for (auto gameObject : world.GetGameObjects())
	{
		if (!gameObject || gameObject->GetInstanceId() != m_dragInstanceId)
		{
			continue;
		}

		const Math::Transform finalTransform = gameObject->GetTransformComponent().GetTransform();
		if (!AreTransformsNear(m_dragStartTransform, finalTransform))
		{
			QueueTransformEvent(
				m_dragInstanceId,
				m_dragStartTransform,
				finalTransform,
				m_dragOperation,
				m_dragSpace);
		}
		break;
	}

	m_dragInstanceId = InstanceId::Invalid;
	m_wasUsingGizmo = false;
	m_dragManagedObjectMutationRevision = 0;
}

void EditorViewportController::TickTransformGizmo(World& world, TObjectPtr<GameObject> selectedObject)
{
	if (!selectedObject ||
		m_operation == ETransformOperation::Select ||
		(m_wasUsingGizmo && selectedObject->GetInstanceId() != m_dragInstanceId))
	{
		if (m_wasUsingGizmo)
		{
			CompleteActiveTransform(world);
		}
		return;
	}

	Math::Transform cameraTransform{};
	CameraData cameraData{};
	auto* cameraEcs = world.GetECS<CameraECS>();
	if (!cameraEcs || !cameraEcs->TryGetActiveCamera(cameraTransform, cameraData))
	{
		if (m_wasUsingGizmo)
		{
			CompleteActiveTransform(world);
		}
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	if (io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
	{
		if (m_wasUsingGizmo)
		{
			CompleteActiveTransform(world);
		}
		return;
	}

	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
	ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

	glm::mat4 worldMatrix = CalculateCurrentWorldMatrix(selectedObject);
	const Math::Transform originalLocalTransform = selectedObject->GetTransformComponent().GetTransform();

	glm::vec3 snapValues{};
	const float* snap = nullptr;
	if (io.KeyCtrl)
	{
		switch (m_operation)
		{
		case ETransformOperation::Translate: snapValues = glm::vec3(50.0f); break;
		case ETransformOperation::Rotate: snapValues = glm::vec3(15.0f); break;
		case ETransformOperation::Scale: snapValues = glm::vec3(0.1f); break;
		default: break;
		}
		snap = &snapValues.x;
	}

	const bool bManipulated = ImGuizmo::Manipulate(
		&cameraData.GetViewMatrix()[0][0],
		&cameraData.GetProjectionMatrix()[0][0],
		ToImGuizmoOperation(m_operation),
		m_space == ETransformSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
		&worldMatrix[0][0],
		nullptr,
		snap);
	m_gizmoSubmittedThisFrame = true;
	const bool bUsingGizmo = ImGuizmo::IsUsing();

	if (bUsingGizmo && !m_wasUsingGizmo)
	{
		m_dragInstanceId = selectedObject->GetInstanceId();
		m_dragStartTransform = originalLocalTransform;
		m_dragOperation = m_operation;
		m_dragSpace = m_space;
		m_dragManagedObjectMutationRevision = m_selectedObjectMutationRevision;
	}

	if (bUsingGizmo && bManipulated && m_dragInstanceId == selectedObject->GetInstanceId())
	{
		glm::mat4 parentWorldMatrix{};
		const glm::mat4* parentWorldMatrixPtr = nullptr;
		if (selectedObject->GetParent())
		{
			parentWorldMatrix = CalculateCurrentWorldMatrix(selectedObject->GetParent());
			parentWorldMatrixPtr = &parentWorldMatrix;
		}

		Math::Transform localTransform{};
		if (TryConvertWorldToLocalTransform(worldMatrix, parentWorldMatrixPtr, localTransform))
		{
			auto& transform = selectedObject->GetTransformComponent();
			transform.SetPosition(glm::vec3(localTransform.m_position));
			transform.SetRotation(localTransform.m_rotation);
			transform.SetScale(localTransform.m_scale);
		}
	}

	if (!bUsingGizmo && m_wasUsingGizmo)
	{
		CompleteActiveTransform(world);
	}
	else
	{
		m_wasUsingGizmo = bUsingGizmo;
	}
}

void EditorViewportController::TickSelection(World& world)
{
	ImGuiIO& io = ImGui::GetIO();
	if (io.AppFocusLost)
	{
		CancelPointerInteraction();
		return;
	}

	const glm::vec2 pointer(io.MousePos.x, io.MousePos.y);
	const bool bGizmoOwnsPointer = DoesSubmittedGizmoOwnPointer(
		m_gizmoSubmittedThisFrame,
		ImGuizmo::IsOver(),
		ImGuizmo::IsUsing());
	const bool bEditorUiOwnsPointer =
		ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) ||
		ImGui::IsAnyItemHovered() ||
		ImGui::IsAnyItemActive();
	const bool bNavigatingViewport = ImGui::IsMouseDown(ImGuiMouseButton_Right);
	const bool bHasModifiers = io.KeyShift || io.KeyCtrl || io.KeyAlt || io.KeySuper;

	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_selectionClickStart = pointer;
		m_gizmoConsumedClick = bGizmoOwnsPointer || bEditorUiOwnsPointer;
		m_selectionClickArmed = CanBeginSelectionGesture(
			bHasModifiers,
			bNavigatingViewport,
			bGizmoOwnsPointer,
			bEditorUiOwnsPointer);
	}

	if (ShouldCancelSelectionGesture(m_selectionClickArmed, bHasModifiers, bNavigatingViewport))
	{
		CancelPointerInteraction();
	}
	else if (m_selectionClickArmed && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		const glm::vec2 delta = pointer - m_selectionClickStart;
		if (glm::dot(delta, delta) > c_clickSlop * c_clickSlop)
		{
			m_selectionClickArmed = false;
		}
	}

	if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		return;
	}

	const bool bShouldPick = m_selectionClickArmed &&
		!m_gizmoConsumedClick &&
		!bNavigatingViewport &&
		!bHasModifiers;
	CancelPointerInteraction();
	if (!bShouldPick || io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
	{
		return;
	}

	Math::Transform cameraTransform{};
	CameraData cameraData{};
	auto* cameraEcs = world.GetECS<CameraECS>();
	Math::Ray ray{};
	if (!cameraEcs ||
		!cameraEcs->TryGetActiveCamera(cameraTransform, cameraData) ||
		!BuildWorldRay(
			pointer.x,
			pointer.y,
			io.DisplaySize.x,
			io.DisplaySize.y,
			cameraData.GetViewMatrix(),
			cameraData.GetProjectionMatrix(),
			ray))
	{
		return;
	}

	TVector<PickCandidate> candidates{};
	for (const auto& gameObject : world.GetGameObjects())
	{
		Math::AABB bounds{};
		bool bUsesMeshBounds = false;
		if (ResolveGameObjectBounds(gameObject, bounds, bUsesMeshBounds) &&
			bUsesMeshBounds)
		{
			candidates.Add(PickCandidate{ gameObject->GetInstanceId(), bounds });
		}
	}

	InstanceId selectedInstanceId{};
	float distance = std::numeric_limits<float>::max();
	TVector<InstanceId> selection{};
	if (TryPickNearest(ray, candidates, selectedInstanceId, distance))
	{
		selection.Add(selectedInstanceId);
	}

	world.SetEditorSelection(selection);
	QueueSelectionEvent(selectedInstanceId);
}

void EditorViewportController::QueueSelectionEvent(const InstanceId& selectedInstanceId)
{
	YAML::Node event{};
	event["kind"] = "selection";
	event["revision"] = ++m_eventRevision;
	event["managedMutationRevision"] = m_managedSelectionMutationRevision;
	event["selectedInstanceId"] = selectedInstanceId ? selectedInstanceId.ToString() : std::string{};
	m_pendingEvents.Add(YAML::Dump(event));
}

void EditorViewportController::QueueTransformEvent(
	const InstanceId& instanceId,
	const Math::Transform& beforeTransform,
	const Math::Transform& afterTransform,
	ETransformOperation operation,
	ETransformSpace space)
{
	YAML::Node event{};
	event["kind"] = "transform";
	event["revision"] = ++m_eventRevision;
	event["managedMutationRevision"] = m_dragManagedObjectMutationRevision;
	event["instanceId"] = instanceId.ToString();
	event["operation"] = ToString(operation);
	event["space"] = ToString(space);
	event["beforePosition"] = beforeTransform.m_position;
	event["beforeRotation"] = beforeTransform.m_rotation;
	event["beforeScale"] = beforeTransform.m_scale;
	event["afterPosition"] = afterTransform.m_position;
	event["afterRotation"] = afterTransform.m_rotation;
	event["afterScale"] = afterTransform.m_scale;
	m_pendingEvents.Add(YAML::Dump(event));
}
