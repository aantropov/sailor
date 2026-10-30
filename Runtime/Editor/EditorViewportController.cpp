#include "Editor/EditorViewportController.h"

#include "AssetRegistry/Model/ModelImporter.h"
#include "Components/CameraComponent.h"
#include "Components/EditorComponent.h"
#include "Components/CollisionShapeComponent.h"
#include "Components/MeshRendererComponent.h"
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
#include <utility>

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

	bool AreTransformsNear(const Math::Transform& lhs, const Math::Transform& rhs)
	{
		// Compare authored locals, including rotations hidden by a zero scale.
		// Position.w is storage metadata, not a transform degree of freedom.
		constexpr float tolerance = 0.000001f;
		if (!Math::AllFinite(lhs.m_position) || !Math::AllFinite(rhs.m_position) ||
			!Math::AllFinite(lhs.m_scale) || !Math::AllFinite(rhs.m_scale) ||
			!Math::AllFinite(lhs.GetRotation()) || !Math::AllFinite(rhs.GetRotation()))
		{
			return false;
		}
		for (int axis = 0; axis != 3; ++axis)
		{
			if (std::abs(lhs.m_position[axis] - rhs.m_position[axis]) > tolerance ||
				std::abs(lhs.m_scale[axis] - rhs.m_scale[axis]) > tolerance)
			{
				return false;
			}
		}
		return std::min(glm::length(lhs.GetRotation() - rhs.GetRotation()),
			glm::length(lhs.GetRotation() + rhs.GetRotation())) <= tolerance;
	}

	bool TryInvertTransformMatrix(const glm::mat4& matrix, glm::mat4& outInverse)
	{
		if (!Math::AllFinite(matrix))
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
		return Math::AllFinite(outInverse) &&
			Math::AreNearlyEqual(
				matrix * outInverse,
				glm::identity<glm::mat4>(),
				c_matrixTolerance);
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
		!Math::AllFinite(view) || !Math::AllFinite(projection))
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

	const glm::vec3 forward = cameraWorldTransform.GetForward();
	if (!Math::AllFinite(cameraWorldTransform.m_position))
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
		bounds.GetCenter() - forward * distance;
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
	if (!Math::AllFinite(worldMatrix))
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

	if (!Math::AllFinite(localMatrix))
	{
		return false;
	}

	Math::Transform localTransform = Math::Transform::FromMatrix(localMatrix);
	if (!Math::AllFinite(localTransform.m_position) ||
		!Math::AllFinite(localTransform.m_scale) ||
		!Math::AreNearlyEqual(
			localTransform.Matrix(),
			localMatrix,
			c_matrixTolerance))
	{
		return false;
	}

	outLocalTransform = localTransform;
	return true;
}

bool EditorViewport::ResolveGameObjectBounds(
	const TObjectPtr<GameObject>& gameObject,
	Math::AABB& outWorldBounds,
	bool& outUsesSelectableGeometry)
{
	outWorldBounds = {};
	outUsesSelectableGeometry = false;
	if (!gameObject || gameObject->GetComponent<EditorComponent>())
	{
		return false;
	}

	const glm::mat4 worldMatrix = CalculateCurrentWorldMatrix(gameObject);
	for (const auto& component : gameObject->GetComponents())
	{
		Math::AABB componentBounds{};
		if (auto meshRenderer = component.DynamicCast<MeshRendererComponent>())
		{
			const ModelPtr model = meshRenderer->GetModel();
			if (model && model->IsReady())
			{
				componentBounds = model->GetBoundsAABB(
					meshRenderer->GetMeshIndex());
				componentBounds.Apply(worldMatrix);
			}
		}
		else if (auto collisionShape =
			component.DynamicCast<CollisionShapeComponent>())
		{
			collisionShape->TryGetWorldBounds(worldMatrix, componentBounds);
		}

		if (!componentBounds.IsValid())
		{
			continue;
		}

		if (outUsesSelectableGeometry)
		{
			outWorldBounds.Extend(componentBounds);
		}
		else
		{
			outWorldBounds = componentBounds;
			outUsesSelectableGeometry = true;
		}
	}

	if (outUsesSelectableGeometry)
	{
		return true;
	}

	const glm::vec3 worldPosition(worldMatrix[3]);
	outWorldBounds = Math::AABB(worldPosition, glm::vec3(c_originFallbackExtent));
	return outWorldBounds.IsValid();
}

namespace
{
	glm::quat CalculateWorldRotation(const TObjectPtr<GameObject>& object)
	{
		glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
		for (auto current = object; current; current = current->GetParent())
		{
			rotation = current->GetTransformComponent().GetTransform().GetRotation() * rotation;
		}
		return glm::normalize(rotation);
	}

	InstanceId ParentId(const TObjectPtr<GameObject>& object)
	{
		return object->GetParent() ? object->GetParent()->GetInstanceId() : InstanceId::Invalid;
	}

	void ApplyLocalTransform(TObjectPtr<GameObject> object, const Math::Transform& local)
	{
		auto& component = object->GetTransformComponent();
		component.SetPosition(glm::vec3(local.m_position));
		component.SetRotation(local.GetRotation());
		component.SetScale(local.m_scale);
	}

	bool ExtendHierarchyGeometryBounds(const TObjectPtr<GameObject>& object, Math::AABB& bounds, bool& hasBounds)
	{
		if (!object || object->GetComponent<EditorComponent>())
		{
			return false;
		}
		Math::AABB ownBounds{};
		bool hasGeometry = false;
		ResolveGameObjectBounds(object, ownBounds, hasGeometry);
		if (hasGeometry)
		{
			if (hasBounds)
			{
				bounds.Extend(ownBounds);
			}
			else
			{
				bounds = ownBounds;
				hasBounds = true;
			}
		}
		for (const auto& child : object->GetChildren())
		{
			hasGeometry = ExtendHierarchyGeometryBounds(child, bounds, hasBounds) || hasGeometry;
		}
		return hasGeometry;
	}

	glm::ivec3 FindScaleAxes(const glm::quat& handleRotation, const glm::quat& objectRotation)
	{
		// Unity's six principal reference alignments. Signs cancel for scale;
		// the weakest aligned axis decides, with stable ordering at ties.
		const glm::mat3 handle = glm::mat3_cast(handleRotation);
		const glm::mat3 object = glm::mat3_cast(objectRotation);
		const glm::ivec3 permutations[] = {
			{ 2, 1, 0 }, { 1, 2, 0 }, { 0, 2, 1 },
			{ 2, 0, 1 }, { 1, 0, 2 }, { 0, 1, 2 } };
		glm::ivec3 best{ 0, 1, 2 };
		float bestScore = -1.0f;
		for (const auto& axes : permutations)
		{
			float score = 1.0f;
			for (int axis = 0; axis != 3; ++axis)
			{
				score = std::min(score, std::abs(glm::dot(handle[axis], object[axes[axis]])));
			}
			if (score > bestScore)
			{
				best = axes;
				bestScore = score;
			}
		}
		return best;
	}

}

bool SelectionTransformGesture::Begin(World& world, const TVector<InstanceId>& selection,
	ETransformOperation operation, ETransformSpace space, ETransformPivotMode pivotMode,
	uint64_t selectionRevision, const ObjectMutationRevision& revision)
{
	if (IsActive() || selection.IsEmpty() || operation == ETransformOperation::Select)
	{
		return false;
	}
	m_targets.Clear();
	m_selectedParents.Clear();
	m_selectedLocals.Clear();
	m_selectedRevisions.Clear();
	m_selection = selection;
	m_operation = operation;
	m_space = space;
	m_pivotMode = pivotMode;
	m_selectionRevision = selectionRevision;
	TObjectPtr<GameObject> active{};
	Math::AABB combined{};
	bool hasBounds = false;
	for (const auto& id : selection)
	{
		auto object = world.GetObjectByInstanceId(id).DynamicCast<GameObject>();
		if (!object || !world.IsEditorSelected(id))
		{
			return false;
		}
		m_selectedParents.Add(ParentId(object));
		m_selectedLocals.Add(object->GetTransformComponent().GetTransform());
		m_selectedRevisions.Add(revision ? revision(id) : 0);
		active = object;
		if (!ExtendHierarchyGeometryBounds(object, combined, hasBounds))
		{
			const Math::AABB point(glm::vec3(CalculateCurrentWorldMatrix(object)[3]), glm::vec3(0));
			if (hasBounds)
			{
				combined.Extend(point);
			}
			else
			{
				combined = point;
				hasBounds = true;
			}
		}
		bool hasSelectedAncestor = false;
		for (auto parent = object->GetParent(); parent; parent = parent->GetParent())
		{
			if (selection.Contains(parent->GetInstanceId()))
			{
				hasSelectedAncestor = true;
				break;
			}
		}
		if (hasSelectedAncestor)
		{
			continue;
		}
		Target target{};
		target.m_id = id;
		target.m_parentId = ParentId(object);
		target.m_before = target.m_applied = object->GetTransformComponent().GetTransform();
		target.m_world = CalculateCurrentWorldMatrix(object);
		target.m_worldRotation = CalculateWorldRotation(object);
		target.m_parentRotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		if (const auto parent = object->GetParent())
		{
			target.m_parentWorld = CalculateCurrentWorldMatrix(parent);
			target.m_parentRotation = CalculateWorldRotation(parent);
		}
		if (!Math::AllFinite(target.m_world) ||
			!TryInvertTransformMatrix(target.m_parentWorld, target.m_inverseParent))
		{
			return false;
		}
		target.m_revision = revision ? revision(id) : 0;
		m_targets.Add(target);
	}
	if (m_targets.IsEmpty() || !active)
	{
		return false;
	}
	m_basis = space == ETransformSpace::Local ? CalculateWorldRotation(active) : glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
	const glm::vec3 origin = pivotMode == ETransformPivotMode::Center && hasBounds
		? combined.GetCenter() : glm::vec3(CalculateCurrentWorldMatrix(active)[3]);
	m_startGizmo = m_gizmoMatrix = glm::translate(glm::mat4(1.0f), origin) * glm::mat4_cast(m_basis);
	if (!Math::AllFinite(m_startGizmo))
	{
		return false;
	}
	for (auto& target : m_targets)
	{
		target.m_scaleAxes = pivotMode == ETransformPivotMode::Pivot && space == ETransformSpace::Local
			? glm::ivec3(0, 1, 2) : FindScaleAxes(m_basis, target.m_worldRotation);
	}
	m_world = &world;
	return true;
}

bool SelectionTransformGesture::Validate(World& world, const TVector<InstanceId>& selection,
	uint64_t selectionRevision, const ObjectMutationRevision& revision) const
{
	if (m_world != &world || m_selectionRevision != selectionRevision || selection.Num() != m_selection.Num())
	{
		return false;
	}
	for (size_t i = 0; i < selection.Num(); ++i)
	{
		auto object = world.GetObjectByInstanceId(m_selection[i]).DynamicCast<GameObject>();
		if (selection[i] != m_selection[i] || !object || !world.IsEditorSelected(m_selection[i]) ||
			ParentId(object) != m_selectedParents[i])
		{
			return false;
		}
		bool isRoot = false;
		for (const auto& target : m_targets)
		{
			isRoot = isRoot || target.m_id == m_selection[i];
		}
		if (!isRoot && ((revision && revision(m_selection[i]) != m_selectedRevisions[i]) ||
			!AreTransformsNear(object->GetTransformComponent().GetTransform(), m_selectedLocals[i])))
		{
			return false;
		}
	}
	for (const auto& target : m_targets)
	{
		auto object = world.GetObjectByInstanceId(target.m_id).DynamicCast<GameObject>();
		if (!object || ParentId(object) != target.m_parentId ||
			(revision && revision(target.m_id) != target.m_revision) ||
			!AreTransformsNear(object->GetTransformComponent().GetTransform(), target.m_applied))
		{
			return false;
		}
		const auto parentWorld = object->GetParent() ? CalculateCurrentWorldMatrix(object->GetParent()) : glm::mat4(1.0f);
		if (!Math::AreNearlyEqual(parentWorld, target.m_parentWorld, 0.000001f))
		{
			return false;
		}
	}
	return true;
}

bool SelectionTransformGesture::Apply(World& world, const glm::mat4& gizmoMatrix, const ObjectMutationRevision& revision)
{
	if (!Validate(world, m_selection, m_selectionRevision, revision) || !Math::AllFinite(gizmoMatrix))
	{
		Cancel(world, revision);
		return false;
	}
	const glm::vec3 translation = glm::vec3(gizmoMatrix[3] - m_startGizmo[3]);
	const glm::mat4 delta = gizmoMatrix * glm::inverse(m_startGizmo);
	const glm::quat rotation = glm::normalize(glm::quat_cast(glm::mat3(delta)));
	const glm::quat individualRotation = glm::inverse(m_basis) * rotation * m_basis;
	const glm::mat3 basis = glm::mat3_cast(m_basis);
	glm::vec3 factors{};
	for (int axis = 0; axis != 3; ++axis)
	{
		factors[axis] = glm::dot(glm::vec3(gizmoMatrix[axis]), basis[axis]);
	}
	TVector<Math::Transform> candidates{};
	for (const auto& target : m_targets)
	{
		Math::Transform local = target.m_before;
		if (m_operation == ETransformOperation::Scale)
		{
			glm::vec3 localFactors(1.0f);
			for (int axis = 0; axis != 3; ++axis)
			{
				localFactors[target.m_scaleAxes[axis]] = factors[axis];
			}
			local.m_scale = target.m_before.m_scale * glm::vec4(localFactors, 1.0f);
			if (m_pivotMode == ETransformPivotMode::Center)
			{
				const glm::vec3 origin(m_startGizmo[3]);
				const glm::vec3 position = origin + basis * (factors * (glm::transpose(basis) * (glm::vec3(target.m_world[3]) - origin)));
				local.m_position = glm::vec4(glm::vec3(target.m_inverseParent * glm::vec4(position, 1.0f)), target.m_before.m_position.w);
			}
		}
		else if (m_operation == ETransformOperation::Rotate &&
			m_space == ETransformSpace::Local && m_pivotMode == ETransformPivotMode::Pivot)
		{
			local.SetRotation(target.m_before.GetRotation() * individualRotation);
		}
		else if (m_operation == ETransformOperation::Translate)
		{
			const glm::vec4 worldPosition = target.m_world[3] + glm::vec4(translation, 0.0f);
			local.m_position = glm::vec4(glm::vec3(target.m_inverseParent * worldPosition), target.m_before.m_position.w);
		}
		else
		{
			// Author the desired world orientation as local TRS rather than
			// decomposing a sheared affine matrix under nonuniform parents.
			local.SetRotation(glm::inverse(target.m_parentRotation) * rotation * target.m_worldRotation);
			if (m_pivotMode == ETransformPivotMode::Center)
			{
				const glm::vec3 center(m_startGizmo[3]);
				const glm::vec3 worldPosition = center + rotation * (glm::vec3(target.m_world[3]) - center);
				local.m_position = glm::vec4(glm::vec3(target.m_inverseParent * glm::vec4(worldPosition, 1.0f)), target.m_before.m_position.w);
			}
		}
		if (!Math::AllFinite(local.Matrix()) || !Math::AllFinite(local.m_scale) || !Math::AllFinite(local.m_position))
		{
			Cancel(world, revision);
			return false;
		}
		candidates.Add(local);
	}
	// No writes occur until every root has a valid candidate.
	for (size_t i = 0; i < m_targets.Num(); ++i)
	{
		auto& target = m_targets[i];
		auto object = world.GetObjectByInstanceId(target.m_id).DynamicCast<GameObject>();
		ApplyLocalTransform(object, candidates[i]);
		target.m_applied = object->GetTransformComponent().GetTransform();
	}
	m_gizmoMatrix = gizmoMatrix;
	return true;
}

void SelectionTransformGesture::Cancel(World& world, const ObjectMutationRevision& revision)
{
	if (m_world == &world)
	{
		for (const auto& target : m_targets)
		{
			auto object = world.GetObjectByInstanceId(target.m_id).DynamicCast<GameObject>();
			// Do not overwrite a concurrent managed edit or a reparented/deleted root.
			if (object && ParentId(object) == target.m_parentId &&
				(!revision || revision(target.m_id) == target.m_revision) &&
				AreTransformsNear(object->GetTransformComponent().GetTransform(), target.m_applied))
			{
				ApplyLocalTransform(object, target.m_before);
			}
		}
	}
	m_world = nullptr;
	m_targets.Clear();
}

bool SelectionTransformGesture::Complete(TransformEvent& outEvent)
{
	if (!IsActive())
	{
		return false;
	}
	outEvent = TransformEvent{ {}, m_operation, m_space, m_pivotMode };
	bool changed = false;
	for (const auto& target : m_targets)
	{
		outEvent.m_changes.Add(TransformChange{ target.m_id, target.m_revision, target.m_before, target.m_applied });
		changed = changed || !AreTransformsNear(target.m_before, target.m_applied);
	}
	m_world = nullptr;
	m_targets.Clear();
	return changed;
}

void EditorViewportController::Tick(World& world, const ObjectMutationRevision& revision)
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
		CancelInteraction(world, revision);
		return;
	}

	SynchronizeSelectionOrder(world);
	TickTransformGizmo(world, revision);
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
	m_selectionClickAdditive = false;
	m_gizmoConsumedClick = false;
	m_selectionClickStart = {};
	m_gesture = {};
	m_transformBlockedUntilRelease = false;
	m_managedSelectionMutationRevision = 0;
	m_lastTickFrame = -1;
	m_pendingEvents.Clear();
	m_selectionOrder.Clear();
	ResetImGuizmoInteractionState();
}

void EditorViewportController::CancelPointerInteraction()
{
	m_selectionClickArmed = false;
	m_selectionClickAdditive = false;
	m_gizmoConsumedClick = false;
}

void EditorViewportController::CancelInteraction(World& world, const ObjectMutationRevision& revision)
{
	CancelPointerInteraction();
	m_gesture.Cancel(world, revision);
	m_wasUsingGizmo = false;
	m_transformBlockedUntilRelease = ImGui::GetCurrentContext() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
	m_gizmoSubmittedThisFrame = false;
	ResetImGuizmoInteractionState();
}

bool EditorViewportController::PullEvent(Event& outEvent)
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
	std::string_view fileId,
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

	m_pendingEvents.Add(Event{ ++m_eventRevision, m_managedSelectionMutationRevision,
		AssetDropEvent{ std::string(fileId), { normalizedX, normalizedY } } });
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

	m_pendingEvents.Add(Event{ ++m_eventRevision, m_managedSelectionMutationRevision,
		ToolShortcutEvent{ keyCode } });
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
	for (const auto& gameObject : std::as_const(world).GetGameObjects())
	{
		Math::AABB bounds{};
		bool bUsesSelectableGeometry = false;
		if (ResolveGameObjectBounds(
				gameObject,
				bounds,
				bUsesSelectableGeometry) &&
			bUsesSelectableGeometry)
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
	bool bUsesSelectableGeometry = false;
	if (!targetObject ||
		!ResolveGameObjectBounds(
			targetObject,
			targetBounds,
			bUsesSelectableGeometry))
	{
		return false;
	}

	if (!bUsesSelectableGeometry)
	{
		targetBounds = Math::AABB(
			targetBounds.GetCenter(),
			glm::vec3(c_minimumCameraFrameRadius));
	}

	TObjectPtr<GameObject> cameraObject{};
	TObjectPtr<CameraComponent> cameraComponent{};
	for (const auto& gameObject : std::as_const(world).GetGameObjects())
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
	if (!Math::AllFinite(cameraWorldMatrix))
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
	ETransformSpace space,
	ETransformPivotMode pivotMode)
{
	if (m_wasUsingGizmo)
	{
		return false;
	}

	m_operation = operation;
	m_space = space;
	m_pivotMode = pivotMode;
	return true;
}

void EditorViewportController::CompleteActiveTransform()
{
	TransformEvent transform{};
	const auto selectionRevision = m_gesture.GetSelectionRevision();
	if (m_gesture.Complete(transform))
	{
		m_pendingEvents.Add(Event{ ++m_eventRevision, selectionRevision, std::move(transform) });
	}
	m_wasUsingGizmo = false;
}

void EditorViewportController::SynchronizeSelectionOrder(World& world)
{
	m_selectionOrder.RemoveAll([&world](const InstanceId& id)
	{
		return !world.IsEditorSelected(id) || !world.GetObjectByInstanceId(id).DynamicCast<GameObject>();
	});
	for (const auto& object : std::as_const(world).GetGameObjects())
	{
		const auto& id = object->GetInstanceId();
		if (world.IsEditorSelected(id) && !m_selectionOrder.Contains(id))
		{
			m_selectionOrder.Add(id);
		}
	}
}

void EditorViewportController::TickTransformGizmo(World& world, const ObjectMutationRevision& revision)
{
	if (m_transformBlockedUntilRelease)
	{
		m_transformBlockedUntilRelease = ImGui::IsMouseDown(ImGuiMouseButton_Left);
		return;
	}
	if (m_gesture.IsActive() && !m_gesture.Validate(world, m_selectionOrder, m_managedSelectionMutationRevision, revision))
	{
		CancelInteraction(world, revision);
		return;
	}
	if (m_selectionOrder.IsEmpty() || m_operation == ETransformOperation::Select)
	{
		CancelInteraction(world, revision);
		return;
	}
	Math::Transform cameraTransform{};
	CameraData cameraData{};
	auto* cameraEcs = world.GetECS<CameraECS>();
	ImGuiIO& io = ImGui::GetIO();
	if (!cameraEcs || !cameraEcs->TryGetActiveCamera(cameraTransform, cameraData) ||
		io.DisplaySize.x <= 0.0f || io.DisplaySize.y <= 0.0f)
	{
		CancelInteraction(world, revision);
		return;
	}
	SelectionTransformGesture start{};
	if (!m_gesture.IsActive() && !start.Begin(world, m_selectionOrder, m_operation, m_space, m_pivotMode,
		m_managedSelectionMutationRevision, revision))
	{
		return;
	}
	glm::mat4 gizmoMatrix = m_gesture.IsActive() ? m_gesture.GetGizmoMatrix() : start.GetGizmoMatrix();
	ImGuizmo::SetOrthographic(false);
	ImGuizmo::SetDrawlist(ImGui::GetForegroundDrawList());
	ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
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
	const bool manipulated = ImGuizmo::Manipulate(
		&cameraData.GetViewMatrix()[0][0], &cameraData.GetProjectionMatrix()[0][0],
		ToImGuizmoOperation(m_operation), m_space == ETransformSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD,
		&gizmoMatrix[0][0], nullptr, snap);
	m_gizmoSubmittedThisFrame = true;
	const bool usingGizmo = ImGuizmo::IsUsing();
	if (usingGizmo && !m_wasUsingGizmo)
	{
		m_gesture = std::move(start);
	}
	if (usingGizmo && manipulated && !m_gesture.Apply(world, gizmoMatrix, revision))
	{
		CancelInteraction(world, revision);
		return;
	}
	if (!usingGizmo && m_wasUsingGizmo)
	{
		CompleteActiveTransform();
	}
	else
	{
		m_wasUsingGizmo = usingGizmo;
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
	const bool bHasModifiers = io.KeyCtrl || io.KeyAlt || io.KeySuper;

	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_selectionClickStart = pointer;
		m_selectionClickAdditive = io.KeyShift;
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
	const bool bAdditive = m_selectionClickAdditive;
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
	for (const auto& gameObject : std::as_const(world).GetGameObjects())
	{
		Math::AABB bounds{};
		bool bUsesSelectableGeometry = false;
		if (ResolveGameObjectBounds(
				gameObject,
				bounds,
				bUsesSelectableGeometry) &&
			bUsesSelectableGeometry)
		{
			candidates.Add(PickCandidate{ gameObject->GetInstanceId(), bounds });
		}
	}

	InstanceId selectedInstanceId{};
	float distance = std::numeric_limits<float>::max();
	TryPickNearest(ray, candidates, selectedInstanceId, distance);
	ApplySelectionPick(world, selectedInstanceId, bAdditive);
}

void EditorViewportController::SetManagedSelection(const TVector<InstanceId>& selection)
{
	CancelPointerInteraction();
	m_selectionOrder.Clear();
	for (const auto& instanceId : selection)
	{
		if (instanceId)
		{
			const auto gameObjectId = instanceId.GameObjectId();
			m_selectionOrder.Remove(gameObjectId);
			m_selectionOrder.Add(gameObjectId);
		}
	}
}

TVector<InstanceId> EditorViewportController::GetSelectionOrder(World& world)
{
	SynchronizeSelectionOrder(world);
	return m_selectionOrder;
}

void EditorViewportController::ApplySelectionPick(World& world, const InstanceId& pickedInstanceId, bool bAdditive)
{
	if (!bAdditive)
	{
		m_selectionOrder.Clear();
	}
	else
	{
		m_selectionOrder.RemoveAll([&world](const InstanceId& id) { return !world.IsEditorSelected(id); });
		for (const auto& gameObject : std::as_const(world).GetGameObjects())
		{
			const auto& id = gameObject->GetInstanceId();
			if (world.IsEditorSelected(id) && !m_selectionOrder.Contains(id))
			{
				m_selectionOrder.Add(id);
			}
		}
	}

	if (pickedInstanceId)
	{
		if (!bAdditive || !m_selectionOrder.Remove(pickedInstanceId))
		{
			m_selectionOrder.Add(pickedInstanceId);
		}
	}

	world.SetEditorSelection(m_selectionOrder);
	QueueSelectionEvent(pickedInstanceId, bAdditive);
}

void EditorViewportController::QueueSelectionEvent(const InstanceId& selectedInstanceId, bool bAdditive)
{
	m_pendingEvents.Add(Event{ ++m_eventRevision, m_managedSelectionMutationRevision,
		SelectionEvent{ selectedInstanceId, m_selectionOrder, bAdditive } });
}
