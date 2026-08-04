#include "Editor/EditorViewportController.h"

#include "AssetRegistry/Model/ModelImporter.h"
#include "Components/CameraComponent.h"
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
	constexpr float c_dropFallbackDistance = 1000.0f;
	constexpr float c_dropPlaneEpsilon = 0.000001f;
	constexpr float c_cameraFramePadding = 1.2f;
	constexpr float c_minimumCameraFrameRadius = 1.0f;

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

	bool IsValidTransformOperation(ETransformOperation operation)
	{
		switch (operation)
		{
		case ETransformOperation::Select:
		case ETransformOperation::Translate:
		case ETransformOperation::Rotate:
		case ETransformOperation::Scale:
			return true;
		default:
			return false;
		}
	}

	bool IsValidTransformSpace(ETransformSpace space)
	{
		switch (space)
		{
		case ETransformSpace::World:
		case ETransformSpace::Local:
			return true;
		default:
			return false;
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

bool EditorViewport::TryResolveRayTarget(
	const Math::Ray& ray,
	const TVector<PickCandidate>& candidates,
	glm::vec3& outPosition)
{
	outPosition = {};
	const float directionLength = glm::length(ray.GetDirection());
	if (!Math::AllFinite(ray.GetOrigin()) ||
		!Math::AllFinite(ray.GetDirection()) ||
		!std::isfinite(directionLength) ||
		directionLength <= std::numeric_limits<float>::epsilon())
	{
		return false;
	}

	InstanceId pickedId{};
	float distance = std::numeric_limits<float>::max();
	if (TryPickNearest(ray, candidates, pickedId, distance))
	{
		outPosition = ray.GetOrigin() + ray.GetDirection() * distance;
		return Math::AllFinite(outPosition);
	}

	if (std::abs(ray.GetDirection().y) > c_dropPlaneEpsilon)
	{
		const float planeDistance = -ray.GetOrigin().y / ray.GetDirection().y;
		if (std::isfinite(planeDistance) && planeDistance >= 0.0f)
		{
			outPosition = ray.GetOrigin() + ray.GetDirection() * planeDistance;
			return Math::AllFinite(outPosition);
		}
	}

	outPosition = ray.GetOrigin() + ray.GetDirection() * c_dropFallbackDistance;
	return Math::AllFinite(outPosition);
}

bool EditorViewport::TryCalculateFramedCameraPosition(
	const Math::AABB& bounds,
	const Math::Transform& cameraWorldTransform,
	float verticalFovDegrees,
	float aspect,
	float zNear,
	glm::vec3& outPosition)
{
	if (!bounds.IsValid() ||
		!std::isfinite(verticalFovDegrees) ||
		verticalFovDegrees <= 0.0f ||
		verticalFovDegrees >= 179.0f ||
		!std::isfinite(aspect) ||
		aspect <= 0.0f ||
		!std::isfinite(zNear) ||
		zNear < 0.0f)
	{
		return false;
	}

	const glm::vec4 cameraRotation(
		cameraWorldTransform.m_rotation.x,
		cameraWorldTransform.m_rotation.y,
		cameraWorldTransform.m_rotation.z,
		cameraWorldTransform.m_rotation.w);
	const glm::vec3 forward = cameraWorldTransform.GetForward();
	const float forwardLength = glm::length(forward);
	if (!Math::AllFinite(cameraWorldTransform.m_position) ||
		!Math::AllFinite(cameraRotation) ||
		!Math::AllFinite(forward) ||
		!std::isfinite(forwardLength) ||
		forwardLength <= std::numeric_limits<float>::epsilon())
	{
		return false;
	}

	const float verticalHalfFov = glm::radians(verticalFovDegrees) * 0.5f;
	const float horizontalHalfFov = std::atan(std::tan(verticalHalfFov) * aspect);
	const float limitingHalfFov = std::min(verticalHalfFov, horizontalHalfFov);
	const float sineHalfFov = std::sin(limitingHalfFov);
	if (!std::isfinite(sineHalfFov) ||
		sineHalfFov <= std::numeric_limits<float>::epsilon())
	{
		return false;
	}

	const float radius = std::max(
		glm::length(bounds.GetExtents()),
		c_minimumCameraFrameRadius);
	const float distance = std::max(
		radius * c_cameraFramePadding / sineHalfFov,
		radius + zNear * 2.0f);
	const glm::vec3 position =
		bounds.GetCenter() - (forward / forwardLength) * distance;
	if (!std::isfinite(radius) ||
		!std::isfinite(distance) ||
		!Math::AllFinite(position))
	{
		return false;
	}

	outPosition = position;
	return true;
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
		if (!model || !model->IsReady())
		{
			continue;
		}

		Math::AABB meshBounds =
			model->GetBoundsAABB(meshRenderer->GetMeshIndex());
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

bool EditorViewportController::QueueAssetDropEvent(
	const std::string& fileId,
	float normalizedX,
	float normalizedY)
{
	if (fileId.empty() ||
		!std::isfinite(normalizedX) ||
		!std::isfinite(normalizedY) ||
		normalizedX < 0.0f ||
		normalizedX > 1.0f ||
		normalizedY < 0.0f ||
		normalizedY > 1.0f)
	{
		return false;
	}

	YAML::Node event{};
	event["kind"] = "assetDrop";
	event["revision"] = ++m_eventRevision;
	event["managedMutationRevision"] =
		m_managedSelectionMutationRevision;
	event["fileId"] = fileId;
	event["normalizedX"] = normalizedX;
	event["normalizedY"] = normalizedY;
	m_pendingEvents.Add(YAML::Dump(event));
	return true;
}

bool EditorViewportController::QueueToolShortcutEvent(uint32_t keyCode)
{
	if (keyCode != 'Q' &&
		keyCode != 'W' &&
		keyCode != 'E' &&
		keyCode != 'R' &&
		keyCode != 'T')
	{
		return false;
	}

	YAML::Node event{};
	event["kind"] = "toolShortcut";
	event["revision"] = ++m_eventRevision;
	event["managedMutationRevision"] =
		m_managedSelectionMutationRevision;
	event["keyCode"] = keyCode;
	m_pendingEvents.Add(YAML::Dump(event));
	return true;
}

bool EditorViewportController::TraceViewportRay(
	World& world,
	float normalizedX,
	float normalizedY,
	glm::vec3& outPosition) const
{
	outPosition = {};
	if (!std::isfinite(normalizedX) ||
		!std::isfinite(normalizedY) ||
		normalizedX < 0.0f ||
		normalizedX > 1.0f ||
		normalizedY < 0.0f ||
		normalizedY > 1.0f)
	{
		return false;
	}

	Math::Transform cameraTransform{};
	CameraData cameraData{};
	auto* cameraEcs = world.GetECS<CameraECS>();
	Math::Ray ray{};
	if (!cameraEcs ||
		!cameraEcs->TryGetActiveCamera(cameraTransform, cameraData) ||
		!BuildWorldRay(
			normalizedX,
			normalizedY,
			1.0f,
			1.0f,
			cameraData.GetViewMatrix(),
			cameraData.GetProjectionMatrix(),
			ray))
	{
		return false;
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

	return EditorViewport::TryResolveRayTarget(
		ray,
		candidates,
		outPosition);
}

bool EditorViewportController::FocusCameraOnObject(
	World& world,
	const InstanceId& instanceId)
{
	if (!instanceId.IsGameObjectId())
	{
		return false;
	}

	const auto targetObject =
		world.GetObjectByInstanceId(instanceId).DynamicCast<GameObject>();
	Math::AABB targetBounds{};
	bool bUsesMeshBounds = false;
	if (!targetObject ||
		!ResolveGameObjectBounds(
			targetObject,
			targetBounds,
			bUsesMeshBounds))
	{
		return false;
	}

	if (!bUsesMeshBounds)
	{
		targetBounds = Math::AABB(
			targetBounds.GetCenter(),
			glm::vec3(c_minimumCameraFrameRadius));
	}

	TObjectPtr<GameObject> cameraObject{};
	TObjectPtr<CameraComponent> cameraComponent{};
	for (const auto& gameObject : world.GetGameObjects())
	{
		if (!gameObject || !gameObject->GetComponent<EditorComponent>())
		{
			continue;
		}

		auto candidateCamera = gameObject->GetComponent<CameraComponent>();
		if (candidateCamera)
		{
			cameraObject = gameObject;
			cameraComponent = candidateCamera;
			break;
		}
	}

	if (!cameraObject || !cameraComponent)
	{
		return false;
	}

	const glm::mat4 cameraWorldMatrix =
		CalculateCurrentWorldMatrix(cameraObject);
	if (!IsFiniteMatrix(cameraWorldMatrix))
	{
		return false;
	}

	Math::Transform cameraWorldTransform =
		Math::Transform::FromMatrix(cameraWorldMatrix);
	glm::vec3 framedPosition{};
	if (!TryCalculateFramedCameraPosition(
			targetBounds,
			cameraWorldTransform,
			cameraComponent->GetFov(),
			cameraComponent->GetAspect(),
			cameraComponent->GetZNear(),
			framedPosition))
	{
		return false;
	}

	const auto cameraParent = cameraObject->GetParent();
	glm::vec4 localPosition(framedPosition, 1.0f);
	if (cameraParent)
	{
		glm::mat4 inverseParentWorldMatrix{};
		if (!TryInvertTransformMatrix(
				CalculateCurrentWorldMatrix(cameraParent),
				inverseParentWorldMatrix))
		{
			return false;
		}

		localPosition = inverseParentWorldMatrix * localPosition;
		if (!Math::AllFinite(localPosition) ||
			std::abs(localPosition.w) <= std::numeric_limits<float>::epsilon())
		{
			return false;
		}
		localPosition /= localPosition.w;
	}

	cameraObject->GetTransformComponent().SetPosition(glm::vec3(localPosition));
	return true;
}

bool EditorViewportController::SetTransformToolState(
	ETransformOperation operation,
	ETransformSpace space)
{
	if (m_wasUsingGizmo ||
		!IsValidTransformOperation(operation) ||
		!IsValidTransformSpace(space))
	{
		return false;
	}

	m_operation = operation;
	m_space = space;
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
