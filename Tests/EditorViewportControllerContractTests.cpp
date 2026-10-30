#include "Editor/EditorViewportController.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "Components/CollisionShapeComponent.h"
#include "Components/EditorComponent.h"
#include "Components/MeshRendererComponent.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "Math/Math.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using namespace Sailor;

namespace
{
	constexpr float c_tolerance = 0.001f;

	void Require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			throw std::runtime_error(std::string(message));
		}
	}

	bool AreVectorsNear(const glm::vec3& lhs, const glm::vec3& rhs, float tolerance = c_tolerance)
	{
		return glm::distance(lhs, rhs) <= tolerance;
	}

	bool AreMatricesNear(const glm::mat4& lhs, const glm::mat4& rhs, float tolerance = c_tolerance)
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

	InstanceId ParseInstanceId(const char* value)
	{
		InstanceId result;
		result.Deserialize(YAML::Node(value));
		return result;
	}

	Math::AABB MakeBounds(const glm::vec3& min, const glm::vec3& max)
	{
		Math::AABB result;
		result.m_min = min;
		result.m_max = max;
		return result;
	}

	class BoundsTestModel final : public Model
	{
	public:
		BoundsTestModel() : Model(FileId()) {}

		void SetReadyBounds(const Math::AABB& bounds)
		{
			m_boundsAabb = bounds;
			m_bIsReady = true;
		}
	};

	class BoundsTestWorld final : public World
	{
	public:
		BoundsTestWorld() : World("EditorViewportBoundsTests", 0, CreateEcs()) {}
		using World::DestroyPendingGameObjects;

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			return systems;
		}
	};

	void TestPrimarySelectionKeepsWorldOrder()
	{
		BoundsTestWorld world;
		Require(!world.GetPrimaryEditorSelection(), "an empty world has no primary selection");
		auto editorOnly = world.Instantiate("Editor only");
		editorOnly->AddComponent<EditorComponent>();
		auto first = world.Instantiate("First", ParseInstanceId("0000000000000020"));
		auto last = world.Instantiate("Last", ParseInstanceId("0000000000000010"));
		const auto componentId = InstanceId::GenerateNewComponentId(first->GetInstanceId());
		world.SetEditorSelection({ last->GetInstanceId(), InstanceId::Invalid, componentId,
			editorOnly->GetInstanceId(), first->GetInstanceId(), ParseInstanceId("0000000000000099") });
		Require(world.GetPrimaryEditorSelection() == first,
			"primary selection uses world order, not click order, ID sorting or duplicates");
		first->SetParent(last);
		Require(world.GetPrimaryEditorSelection() == first, "reparenting must not change primary selection");
		first->SetParent({});
		auto editorMarker = first->AddComponent<EditorComponent>();
		Require(world.GetPrimaryEditorSelection() == last, "adding an editor-only component must change eligibility immediately");
		Require(first->RemoveComponent(editorMarker) && world.GetPrimaryEditorSelection() == first,
			"removing an editor-only component must restore the original world order");
		world.SetEditorSelection({ editorOnly->GetInstanceId() });
		Require(!world.GetPrimaryEditorSelection(), "editor infrastructure alone is not an editable selection");
		world.SetEditorSelection({});
		Require(!world.GetPrimaryEditorSelection(), "clearing selection must not retain a primary object");
		world.Clear();
	}

	void TestPrimarySelectionTracksObjectLifetime()
	{
		BoundsTestWorld world;
		auto first = world.Instantiate("First");
		auto last = world.Instantiate("Last");
		const auto firstId = first->GetInstanceId();
		const auto lastId = last->GetInstanceId();
		world.SetEditorSelection({ lastId, firstId });
		world.Destroy(first);
		Require(world.GetPrimaryEditorSelection() == first,
			"deferred destruction must preserve the existing selection until the world removes the object");
		world.DestroyPendingGameObjects();
		Require(world.GetPrimaryEditorSelection() == last, "removing the primary object must advance to the next selection");
		auto restored = world.Instantiate("Restored", firstId);
		world.SetEditorSelection({ firstId, lastId });
		Require(world.GetPrimaryEditorSelection() == last, "a restored ID must use its new world insertion order");
		world.DestroyImmediate(last);
		Require(world.GetPrimaryEditorSelection() == restored, "immediate deletion must not leave a stale primary");
		world.Clear();
		Require(!world.GetPrimaryEditorSelection(), "clearing the world must clear selection");
		world.SetEditorSelection({ lastId });
		Require(!world.GetPrimaryEditorSelection(), "a not-yet-created selected ID must not resolve");
		auto late = world.Instantiate("Late", lastId);
		Require(world.GetPrimaryEditorSelection() == late, "creation after selection must resolve without a stale cache");

		BoundsTestWorld other;
		auto sameId = other.Instantiate("Other world", lastId);
		other.SetEditorSelection({ lastId });
		Require(other.GetPrimaryEditorSelection() == sameId && world.GetPrimaryEditorSelection() == late && sameId != late,
			"worlds sharing a serialized ID must resolve their own selected object");
		other.Clear();
		world.Clear();
	}

	void TestBuildWorldRayUsesReversedZAtViewportCenter()
	{
		const glm::mat4 view = glm::identity<glm::mat4>();
		const glm::mat4 projection = Math::PerspectiveRH(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
		Math::Ray ray;

		Require(EditorViewport::BuildWorldRay(50.0f, 50.0f, 100.0f, 100.0f, view, projection, ray),
			"viewport center must produce a world ray for a valid reversed-Z camera");
		Require(AreVectorsNear(ray.GetOrigin(), glm::vec3(0.0f, 0.0f, -0.1f)),
			"reversed-Z center ray must originate on the near plane");
		Require(AreVectorsNear(ray.GetDirection(), Math::vec3_Forward),
			"reversed-Z center ray must point from near depth 1 toward far depth 0");
	}

	void TestBuildWorldRayMapsViewportCorners()
	{
		const glm::mat4 view = glm::identity<glm::mat4>();
		const glm::mat4 projection = Math::PerspectiveRH(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
		Math::Ray topLeftRay;
		Math::Ray bottomRightRay;

		Require(EditorViewport::BuildWorldRay(0.0f, 0.0f, 100.0f, 100.0f, view, projection, topLeftRay),
			"top-left viewport corner must be accepted");
		Require(EditorViewport::BuildWorldRay(100.0f, 100.0f, 100.0f, 100.0f, view, projection, bottomRightRay),
			"bottom-right viewport corner must be accepted");

		Require(AreVectorsNear(topLeftRay.GetOrigin(), glm::vec3(-0.1f, 0.1f, -0.1f)),
			"top-left ray must preserve top-left input orientation");
		Require(AreVectorsNear(topLeftRay.GetDirection(), glm::normalize(glm::vec3(-1.0f, 1.0f, -1.0f))),
			"top-left ray direction must match the perspective frustum corner");
		Require(AreVectorsNear(bottomRightRay.GetOrigin(), glm::vec3(0.1f, -0.1f, -0.1f)),
			"bottom-right ray must preserve bottom-right input orientation");
		Require(AreVectorsNear(bottomRightRay.GetDirection(), glm::normalize(glm::vec3(1.0f, -1.0f, -1.0f))),
			"bottom-right ray direction must match the perspective frustum corner");
	}

	void TestBuildWorldRayRejectsInvalidViewport()
	{
		const glm::mat4 view = glm::identity<glm::mat4>();
		const glm::mat4 projection = Math::PerspectiveRH(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
		Math::Ray ray;

		Require(!EditorViewport::BuildWorldRay(0.0f, 0.0f, 0.0f, 100.0f, view, projection, ray),
			"zero viewport width must be rejected");
		Require(!EditorViewport::BuildWorldRay(0.0f, 0.0f, 100.0f, -1.0f, view, projection, ray),
			"negative viewport height must be rejected");
		Require(!EditorViewport::BuildWorldRay(-1.0f, 50.0f, 100.0f, 100.0f, view, projection, ray),
			"pointer coordinates outside the viewport must be rejected");
		Require(!EditorViewport::BuildWorldRay(101.0f, 50.0f, 100.0f, 100.0f, view, projection, ray),
			"pointer coordinates beyond the viewport must be rejected");
		Require(!EditorViewport::BuildWorldRay(50.0f, 50.0f, std::numeric_limits<float>::quiet_NaN(), 100.0f, view, projection, ray),
			"non-finite viewport dimensions must be rejected");
	}

	void TestPickNearestUsesDistance()
	{
		const InstanceId nearId = ParseInstanceId("0000000000000010");
		const InstanceId farId = ParseInstanceId("0000000000000020");
		const Math::Ray ray(glm::vec3(0.0f), Math::vec3_Forward);
		const TVector<EditorViewport::PickCandidate> candidates = {
			{ farId, MakeBounds(glm::vec3(-1.0f, -1.0f, -8.0f), glm::vec3(1.0f, 1.0f, -7.0f)) },
			{ nearId, MakeBounds(glm::vec3(-1.0f, -1.0f, -3.0f), glm::vec3(1.0f, 1.0f, -2.0f)) },
		};
		InstanceId pickedId;
		float distance = 0.0f;

		Require(EditorViewport::TryPickNearest(ray, candidates, pickedId, distance),
			"a ray crossing valid candidates must produce a pick");
		Require(pickedId == nearId, "the nearest candidate must win regardless of input order");
		Require(std::abs(distance - 2.0f) <= c_tolerance, "the pick distance must be the nearest AABB entry distance");
	}

	void TestPickNearestBreaksTiesDeterministically()
	{
		const InstanceId smallerId = ParseInstanceId("0000000000000001");
		const InstanceId largerId = ParseInstanceId("0000000000000002");
		const Math::Ray ray(glm::vec3(0.0f), Math::vec3_Forward);
		const Math::AABB sharedBounds = MakeBounds(
			glm::vec3(-1.0f, -1.0f, -4.0f),
			glm::vec3(1.0f, 1.0f, -3.0f));
		const TVector<EditorViewport::PickCandidate> largerFirst = {
			{ largerId, sharedBounds },
			{ smallerId, sharedBounds },
		};
		const TVector<EditorViewport::PickCandidate> smallerFirst = {
			{ smallerId, sharedBounds },
			{ largerId, sharedBounds },
		};

		for (const auto& candidates : { largerFirst, smallerFirst })
		{
			InstanceId pickedId;
			float distance = 0.0f;
			Require(EditorViewport::TryPickNearest(ray, candidates, pickedId, distance),
				"equal-distance candidates must produce a pick");
			Require(pickedId == smallerId,
				"equal-distance candidates must use the lexicographically smaller InstanceId independent of input order");
			Require(std::abs(distance - 3.0f) <= c_tolerance,
				"tie-breaking must preserve the shared hit distance");
		}
	}

	void TestPickNearestTreatsInsideBoundsAsZeroDistance()
	{
		const InstanceId insideId = ParseInstanceId("0000000000000003");
		const InstanceId outsideId = ParseInstanceId("0000000000000004");
		const Math::Ray ray(glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f));
		const TVector<EditorViewport::PickCandidate> candidates = {
			{ outsideId, MakeBounds(glm::vec3(3.0f, -1.0f, -1.0f), glm::vec3(4.0f, 1.0f, 1.0f)) },
			{ insideId, MakeBounds(glm::vec3(-1.0f), glm::vec3(1.0f)) },
		};
		InstanceId pickedId;
		float distance = 1.0f;

		Require(EditorViewport::TryPickNearest(ray, candidates, pickedId, distance),
			"a ray starting inside an AABB must produce a pick");
		Require(pickedId == insideId, "the containing AABB must beat candidates entered farther along the ray");
		Require(std::abs(distance) <= c_tolerance, "a ray starting inside an AABB must report zero distance");
	}

	void TestResolveRayTargetUsesNearestMeshBounds()
	{
		const InstanceId nearId = ParseInstanceId("0000000000000010");
		const InstanceId farId = ParseInstanceId("0000000000000020");
		const Math::Ray ray(glm::vec3(0.0f, 2.0f, 0.0f), Math::vec3_Forward);
		const TVector<EditorViewport::PickCandidate> candidates = {
			{ farId, MakeBounds(glm::vec3(-1.0f, 1.0f, -9.0f), glm::vec3(1.0f, 3.0f, -8.0f)) },
			{ nearId, MakeBounds(glm::vec3(-1.0f, 1.0f, -4.0f), glm::vec3(1.0f, 3.0f, -3.0f)) },
		};
		glm::vec3 position{};

		Require(EditorViewport::TryResolveRayTarget(ray, candidates, position),
			"a viewport ray crossing mesh bounds must resolve successfully");
		Require(AreVectorsNear(position, glm::vec3(0.0f, 2.0f, -3.0f)),
			"drop placement must use the nearest mesh AABB before fallback surfaces");
	}

	void TestResolveRayTargetUsesGroundPlane()
	{
		const glm::vec3 origin(2.0f, 4.0f, 3.0f);
		const glm::vec3 direction = glm::normalize(glm::vec3(1.0f, -2.0f, -1.0f));
		const Math::Ray ray(origin, direction);
		const TVector<EditorViewport::PickCandidate> candidates{};
		glm::vec3 position{};
		const float planeDistance = -origin.y / direction.y;

		Require(EditorViewport::TryResolveRayTarget(ray, candidates, position),
			"a viewport ray facing the ground plane must resolve successfully");
		Require(AreVectorsNear(position, origin + direction * planeDistance),
			"drop placement without mesh hits must intersect the y=0 plane");
		Require(std::abs(position.y) <= c_tolerance,
			"ground-plane drop placement must land on y=0");
	}

	void TestResolveRayTargetUsesFiniteForwardFallback()
	{
		const glm::vec3 origin(1.0f, 2.0f, 3.0f);
		const glm::vec3 direction = glm::normalize(glm::vec3(1.0f, 0.0f, -1.0f));
		const Math::Ray ray(origin, direction);
		const TVector<EditorViewport::PickCandidate> candidates{};
		glm::vec3 position{};

		Require(EditorViewport::TryResolveRayTarget(ray, candidates, position),
			"a viewport ray parallel to the ground plane must still resolve");
		Require(Math::AllFinite(position),
			"the forward fallback must remain finite");
		Require(AreVectorsNear(position, origin + direction * 1000.0f),
			"the final drop fallback must use a finite forward distance");

		const Math::Ray upwardRay(
			origin,
			glm::normalize(glm::vec3(0.0f, 1.0f, -1.0f)));
		Require(EditorViewport::TryResolveRayTarget(
			upwardRay,
			candidates,
			position),
			"a ground-plane intersection behind the camera must use the forward fallback");
		Require(position.y > origin.y,
			"drop placement must never follow the ray backwards to reach the ground plane");
	}

	void TestResolveRayTargetRejectsInvalidRay()
	{
		const TVector<EditorViewport::PickCandidate> candidates{};
		glm::vec3 position(4.0f, 5.0f, 6.0f);

		Require(!EditorViewport::TryResolveRayTarget(
			Math::Ray(glm::vec3(0.0f), glm::vec3(0.0f)),
			candidates,
			position),
			"a zero-length drop ray must be rejected");
		Require(AreVectorsNear(position, glm::vec3(0.0f)),
			"a rejected drop ray must not leak a stale position");

		const float nan = std::numeric_limits<float>::quiet_NaN();
		Require(!EditorViewport::TryResolveRayTarget(
			Math::Ray(glm::vec3(nan, 0.0f, 0.0f), Math::vec3_Forward),
			candidates,
			position),
			"a non-finite drop ray must be rejected");
	}

	void TestCalculateFramedCameraPositionPreservesViewDirection()
	{
		const Math::AABB bounds(
			glm::vec3(10.0f, 2.0f, -5.0f),
			glm::vec3(2.0f, 1.0f, 3.0f));
		const Math::Transform cameraTransform(
			glm::vec4(-20.0f, 15.0f, 30.0f, 1.0f),
			glm::angleAxis(
				glm::radians(35.0f),
				glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f))),
			glm::vec4(1.0f));
		glm::vec3 position{};

		Require(EditorViewport::TryCalculateFramedCameraPosition(
			bounds,
			cameraTransform,
			70.0f,
			16.0f / 9.0f,
			0.1f,
			position),
			"valid bounds and camera data must produce a framing position");
		const glm::vec3 viewDirection =
			glm::normalize(bounds.GetCenter() - position);
		Require(AreVectorsNear(
			viewDirection,
			glm::normalize(cameraTransform.GetForward())),
			"camera framing must preserve the current camera view direction");
		Require(glm::distance(position, bounds.GetCenter()) >
			glm::length(bounds.GetExtents()),
			"camera framing must place the camera outside the target bounds");
	}

	void TestCalculateFramedCameraPositionAccountsForViewportAspect()
	{
		const Math::AABB bounds(glm::vec3(0.0f), glm::vec3(1.0f));
		const Math::Transform cameraTransform(
			glm::vec4(0.0f),
			glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(1.0f));
		glm::vec3 landscapePosition{};
		glm::vec3 portraitPosition{};

		Require(EditorViewport::TryCalculateFramedCameraPosition(
			bounds,
			cameraTransform,
			60.0f,
			2.0f,
			0.1f,
			landscapePosition),
			"a landscape viewport must produce a framing position");
		Require(EditorViewport::TryCalculateFramedCameraPosition(
			bounds,
			cameraTransform,
			60.0f,
			0.5f,
			0.1f,
			portraitPosition),
			"a portrait viewport must produce a framing position");
		Require(glm::distance(portraitPosition, bounds.GetCenter()) >
			glm::distance(landscapePosition, bounds.GetCenter()),
			"a narrow viewport must frame the same bounds from farther away");
	}

	void TestCalculateFramedCameraPositionRejectsInvalidInput()
	{
		const Math::AABB bounds(glm::vec3(0.0f), glm::vec3(1.0f));
		const Math::Transform cameraTransform{};
		const glm::vec3 sentinel(4.0f, 5.0f, 6.0f);
		glm::vec3 position = sentinel;

		Require(!EditorViewport::TryCalculateFramedCameraPosition(
			bounds,
			cameraTransform,
			0.0f,
			1.0f,
			0.1f,
			position),
			"a zero camera FOV must be rejected");
		Require(AreVectorsNear(position, sentinel),
			"rejected camera framing must leave its output unchanged");
		Require(!EditorViewport::TryCalculateFramedCameraPosition(
			bounds,
			cameraTransform,
			60.0f,
			0.0f,
			0.1f,
			position),
			"a zero camera aspect must be rejected");
		Require(!EditorViewport::TryCalculateFramedCameraPosition(
			Math::AABB{},
			cameraTransform,
			60.0f,
			1.0f,
			0.1f,
			position),
			"invalid target bounds must be rejected");
	}

	void TestCameraFramingUsesCanonicalRotation()
	{
		const Math::AABB bounds(glm::vec3(1, 2, 3), glm::vec3(4));
		const auto rotation = glm::angleAxis(0.7f, glm::normalize(glm::vec3(1, 2, 3)));
		for (float magnitude : { 5.0f, -3.0f, 1.0e30f, 0.0f })
		{
			const Math::Transform authored(glm::vec4(0), rotation * magnitude);
			const Math::Transform reference(glm::vec4(0), magnitude == 0.0f ? Math::quat_Identity : rotation);
			glm::vec3 actualPosition, expectedPosition;
			Require(EditorViewport::TryCalculateFramedCameraPosition(bounds, authored, 60.0f, 1.5f, 0.1f, actualPosition) &&
				EditorViewport::TryCalculateFramedCameraPosition(bounds, reference, 60.0f, 1.5f, 0.1f, expectedPosition),
				"camera framing must accept normalized and recovered authored rotations");
			Require(AreVectorsNear(actualPosition, expectedPosition),
				"camera framing must use the canonical orientation independent of authored magnitude");
		}
	}

	glm::mat4 CurrentWorld(const TObjectPtr<GameObject>& object)
	{
		glm::mat4 result(1.0f);
		for (auto current = object; current; current = current->GetParent())
		{
			result = current->GetTransformComponent().GetTransform().Matrix() * result;
		}
		return result;
	}

	glm::mat4 RotateHandle(const glm::mat4& start, const glm::quat& rotation)
	{
		const auto center = glm::vec3(start[3]);
		return glm::translate(glm::mat4(1.0f), center) * glm::mat4_cast(rotation) *
			glm::translate(glm::mat4(1.0f), -center) * start;
	}

	void TestGroupTranslationTrimsRootsAndRetainsActiveChild()
	{
		using namespace EditorViewport;
		BoundsTestWorld world;
		auto parent = world.Instantiate("Selected parent");
		auto child = world.Instantiate("Active selected child");
		auto peer = world.Instantiate("Other root");
		parent->GetTransformComponent().SetPosition({ 2, 0, 0 });
		child->SetParent(parent);
		child->GetTransformComponent().SetPosition({ 3, 0, 0 });
		const TVector<InstanceId> selection{ peer->GetInstanceId(), parent->GetInstanceId(), child->GetInstanceId() };
		world.SetEditorSelection(selection);
		SelectionTransformGesture gesture;
		Require(gesture.Begin(world, selection, ETransformOperation::Translate, ETransformSpace::World, ETransformPivotMode::Pivot, 31,
			[&](const InstanceId& id) { return id == parent->GetInstanceId() ? 12 : 17; }), "group gesture must capture roots");
		const auto start = gesture.GetGizmoMatrix();
		Require(AreVectorsNear(glm::vec3(start[3]), { 5, 0, 0 }), "active child controls the pivot without being transformed twice");
		const glm::vec3 delta(4, 2, -1);
		Require(gesture.Apply(world, glm::translate(glm::mat4(1.0f), delta) * start), "translation must apply to every root");
		Require(gesture.Apply(world, glm::translate(glm::mat4(1.0f), delta * 2.0f) * start), "later frames must derive from the captured start");
		Require(AreVectorsNear(glm::vec3(CurrentWorld(parent)[3]), glm::vec3(2, 0, 0) + delta * 2.0f) &&
			AreVectorsNear(glm::vec3(CurrentWorld(peer)[3]), delta * 2.0f) &&
			AreVectorsNear(glm::vec3(CurrentWorld(child)[3]), glm::vec3(5, 0, 0) + delta * 2.0f) &&
			AreVectorsNear(glm::vec3(child->GetTransformComponent().GetPosition()), { 3, 0, 0 }),
			"roots share the world delta without drift or double-applying to selected descendants");
		TransformEvent event;
		Require(gesture.Complete(event) && event.m_changes.Num() == 2 && event.m_changes[0].m_instanceId == peer->GetInstanceId() &&
			event.m_changes[1].m_instanceId == parent->GetInstanceId() && event.m_changes[1].m_managedMutationRevision == 12 &&
			event.m_changes[0].m_after.m_position == peer->GetTransformComponent().GetTransform().m_position,
			"one completed event must contain ordered roots and their captured object revisions");
	}

	void TestGroupCenterUsesHierarchyGeometryAndEmptyOrigins()
	{
		using namespace EditorViewport;
		BoundsTestWorld world;
		auto parent = world.Instantiate("Empty parent");
		auto child = world.Instantiate("Offset child geometry");
		auto peer = world.Instantiate("Peer geometry");
		auto empty = world.Instantiate("Empty selection origin");
		child->SetParent(parent);
		child->GetTransformComponent().SetPosition({ 10, 0, 0 });
		peer->GetTransformComponent().SetPosition({ 20, 0, 0 });
		empty->GetTransformComponent().SetPosition({ 30, 0, 0 });
		for (auto object : { child, peer })
		{
			auto shape = object->AddComponent<CollisionShapeComponent>();
			shape->SetSize(glm::vec3(2));
		}
		world.SetEditorSelection({ parent->GetInstanceId(), peer->GetInstanceId() });
		SelectionTransformGesture first;
		Require(first.Begin(world, { parent->GetInstanceId(), peer->GetInstanceId() }, ETransformOperation::Rotate,
			ETransformSpace::World, ETransformPivotMode::Center, 0) && AreVectorsNear(glm::vec3(first.GetGizmoMatrix()[3]), { 15, 0, 0 }),
			"Center must union child geometry, excluding the empty parent's pivot when its hierarchy has geometry");
		world.SetEditorSelection({ parent->GetInstanceId(), peer->GetInstanceId(), empty->GetInstanceId() });
		SelectionTransformGesture second;
		Require(second.Begin(world, { parent->GetInstanceId(), peer->GetInstanceId(), empty->GetInstanceId() }, ETransformOperation::Scale,
			ETransformSpace::World, ETransformPivotMode::Center, 0) && AreVectorsNear(glm::vec3(second.GetGizmoMatrix()[3]), { 19.5f, 0, 0 }),
			"an entirely empty selected hierarchy must contribute its origin, not a padded picking fallback");
	}

	void TestGroupRotationPivotCenterLocalWorld()
	{
		using namespace EditorViewport;
		for (const auto pivot : { ETransformPivotMode::Pivot, ETransformPivotMode::Center })
		{
			for (const auto space : { ETransformSpace::World, ETransformSpace::Local })
			{
				BoundsTestWorld world;
				auto a = world.Instantiate("A");
				auto b = world.Instantiate("B active");
				a->GetTransformComponent().SetPosition({ -4, 0, 0 });
				b->GetTransformComponent().SetPosition({ 4, 0, 0 });
				const auto qa = glm::angleAxis(0.6f, glm::vec3(0, 1, 0));
				const auto qb = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
				a->GetTransformComponent().SetRotation(qa);
				b->GetTransformComponent().SetRotation(qb);
				world.SetEditorSelection({ a->GetInstanceId(), b->GetInstanceId() });
				SelectionTransformGesture gesture;
				Require(gesture.Begin(world, { a->GetInstanceId(), b->GetInstanceId() }, ETransformOperation::Rotate, space, pivot, 0),
					"rotation must capture a group in every pivot/space mode");
				const auto start = gesture.GetGizmoMatrix();
				const auto basis = space == ETransformSpace::Local ? qb : glm::identity<glm::quat>();
				const auto localDelta = glm::angleAxis(0.7f, glm::vec3(1, 0, 0));
				const auto worldDelta = basis * localDelta * glm::inverse(basis);
				Require(gesture.Apply(world, RotateHandle(start, worldDelta)), "group rotation must apply in the chosen basis");
				for (auto object : { a, b })
				{
					const auto initial = object == a ? qa : qb;
					const auto expected = pivot == ETransformPivotMode::Pivot && space == ETransformSpace::Local
						? initial * localDelta : worldDelta * initial;
					Require(AreMatricesNear(glm::mat4_cast(object->GetTransformComponent().GetTransform().GetRotation()), glm::mat4_cast(expected)),
						"Local Pivot must rotate around each object's own axes; other modes share the handle basis");
					const glm::vec3 oldPosition = object == a ? glm::vec3(-4, 0, 0) : glm::vec3(4, 0, 0);
					const glm::vec3 expectedPosition = pivot == ETransformPivotMode::Pivot ? oldPosition :
						glm::vec3(start[3]) + worldDelta * (oldPosition - glm::vec3(start[3]));
					Require(AreVectorsNear(glm::vec3(CurrentWorld(object)[3]), expectedPosition),
						"Pivot retains each root position; Center rotates positions around combined bounds");
				}
			}
		}
	}

	void TestGroupScalePivotCenterLocalWorldAndSignedZero()
	{
		using namespace EditorViewport;
		for (const auto pivot : { ETransformPivotMode::Pivot, ETransformPivotMode::Center })
		{
			for (const auto space : { ETransformSpace::World, ETransformSpace::Local })
			{
				BoundsTestWorld world;
				auto a = world.Instantiate("Signed zero scale");
				auto b = world.Instantiate("Active rotated root");
				a->GetTransformComponent().SetPosition({ -4, 0, 0 });
				b->GetTransformComponent().SetPosition({ 4, 0, 0 });
				a->GetTransformComponent().SetScale({ -2, 0, 3, 1 });
				b->GetTransformComponent().SetScale({ 1, 2, 3, 1 });
				const auto qb = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0, 0, 1));
				b->GetTransformComponent().SetRotation(qb);
				world.SetEditorSelection({ a->GetInstanceId(), b->GetInstanceId() });
				SelectionTransformGesture gesture;
				Require(gesture.Begin(world, { a->GetInstanceId(), b->GetInstanceId() }, ETransformOperation::Scale, space, pivot, 0),
					"scale must accept zero and negative root scale without a singular gizmo matrix");
				const auto start = gesture.GetGizmoMatrix();
				const glm::vec3 factors(2, 3, 4);
				Require(gesture.Apply(world, start * glm::scale(glm::mat4(1.0f), factors)), "group scale must preserve representable local TRS");
				const bool individual = pivot == ETransformPivotMode::Pivot && space == ETransformSpace::Local;
				const glm::vec3 factorsA = !individual && space == ETransformSpace::Local ? glm::vec3(3, 2, 4) : factors;
				const glm::vec3 factorsB = !individual && space == ETransformSpace::World ? glm::vec3(3, 2, 4) : factors;
				Require(AreVectorsNear(glm::vec3(a->GetTransformComponent().GetTransform().m_scale), glm::vec3(-2, 0, 3) * factorsA) &&
					AreVectorsNear(glm::vec3(b->GetTransformComponent().GetTransform().m_scale), glm::vec3(1, 2, 3) * factorsB),
					"closest-axis scale mapping must retain signed/zero authored components");
				Require(AreMatricesNear(glm::mat4_cast(b->GetTransformComponent().GetTransform().GetRotation()), glm::mat4_cast(qb)),
					"nonuniform scale must preserve each object's rotation instead of introducing shear");
				const glm::mat3 basis = glm::mat3(start);
				for (auto object : { a, b })
				{
					const glm::vec3 oldPosition = object == a ? glm::vec3(-4, 0, 0) : glm::vec3(4, 0, 0);
					const glm::vec3 origin(start[3]);
					const glm::vec3 expected = pivot == ETransformPivotMode::Pivot ? oldPosition :
						origin + basis * (factors * (glm::transpose(basis) * (oldPosition - origin)));
					Require(AreVectorsNear(glm::vec3(CurrentWorld(object)[3]), expected), "Center scales positions in the common basis; Pivot retains own positions");
				}
				gesture.Cancel(world);
				Require(AreVectorsNear(glm::vec3(a->GetTransformComponent().GetTransform().m_scale), { -2, 0, 3 }), "cancel must restore signed and zero scales");
			}
		}
	}

	void TestZeroScaleRotationStillCompletesAuthoredUndo()
	{
		using namespace EditorViewport;
		BoundsTestWorld world;
		auto object = world.Instantiate("Zero scale");
		object->GetTransformComponent().SetScale({ 0, 0, 0, 1 });
		world.SetEditorSelection({ object->GetInstanceId() });
		SelectionTransformGesture gesture;
		Require(gesture.Begin(world, { object->GetInstanceId() }, ETransformOperation::Rotate, ETransformSpace::Local,
			ETransformPivotMode::Pivot, 0), "zero-scale root must have a usable unit-scale rotation gizmo");
		const auto start = gesture.GetGizmoMatrix();
		Require(gesture.Apply(world, RotateHandle(start, glm::angleAxis(0.5f, glm::vec3(0, 1, 0)))), "rotation must update authored zero-scale TRS");
		TransformEvent completed;
		Require(gesture.Complete(completed) && completed.m_changes.Num() == 1 &&
			std::abs(glm::dot(completed.m_changes[0].m_before.GetRotation(), completed.m_changes[0].m_after.GetRotation())) < 0.99f,
			"a zero-scale matrix cannot hide an authored rotation change from atomic Undo history");
	}

	void TestGroupDifferentParentsAndSingularRejection()
	{
		using namespace EditorViewport;
		BoundsTestWorld world;
		auto parent = world.Instantiate("Nonuniform parent");
		auto otherParent = world.Instantiate("Rotated parent");
		auto a = world.Instantiate("First root");
		auto b = world.Instantiate("Second root");
		parent->GetTransformComponent().SetScale({ 2, 1, 1, 1 });
		otherParent->GetTransformComponent().SetRotation(glm::angleAxis(0.4f, glm::vec3(0, 0, 1)));
		a->SetParent(otherParent);
		b->SetParent(parent);
		a->GetTransformComponent().SetPosition({ -4, 0, 0 });
		b->GetTransformComponent().SetPosition({ 4, 0, 0 });
		const auto aBefore = CurrentWorld(a), bBefore = CurrentWorld(b);
		const TVector<InstanceId> selection{ a->GetInstanceId(), b->GetInstanceId() };
		world.SetEditorSelection(selection);
		SelectionTransformGesture translate;
		Require(translate.Begin(world, selection, ETransformOperation::Translate, ETransformSpace::World, ETransformPivotMode::Center, 0), "different parent roots must capture");
		const auto translation = glm::translate(glm::mat4(1.0f), glm::vec3(2, 3, 4));
		Require(translate.Apply(world, translation * translate.GetGizmoMatrix()) &&
			AreMatricesNear(CurrentWorld(a), translation * aBefore) && AreMatricesNear(CurrentWorld(b), translation * bBefore),
			"world translation must convert through different unchanged parents");
		translate.Cancel(world);
		SelectionTransformGesture rotate;
		Require(rotate.Begin(world, selection, ETransformOperation::Rotate, ETransformSpace::World, ETransformPivotMode::Pivot, 0), "rotation must capture both roots");
		const auto start = rotate.GetGizmoMatrix();
		const auto deltaRotation = glm::angleAxis(0.4f, glm::vec3(0, 0, 1));
		Require(rotate.Apply(world, RotateHandle(start, glm::angleAxis(0.3f, glm::vec3(1, 0, 0)))) &&
			rotate.Apply(world, RotateHandle(start, deltaRotation)), "group rotation must remain usable under nonuniform parents");
		Require(AreVectorsNear(glm::vec3(CurrentWorld(a)[3]), glm::vec3(aBefore[3])) &&
			AreVectorsNear(glm::vec3(CurrentWorld(b)[3]), glm::vec3(bBefore[3])) &&
			AreMatricesNear(glm::mat4_cast(b->GetTransformComponent().GetTransform().GetRotation()), glm::mat4_cast(deltaRotation)) &&
			AreVectorsNear(glm::vec3(b->GetTransformComponent().GetTransform().m_scale), glm::vec3(1)),
			"World Pivot rotation must author local orientation, retain own positions and preserve local scales");
		rotate.Cancel(world);
		Require(AreMatricesNear(CurrentWorld(a), aBefore) && AreMatricesNear(CurrentWorld(b), bBefore), "group cancellation must restore both differently parented roots");
		parent->GetTransformComponent().SetScale({ 0, 1, 1, 1 });
		SelectionTransformGesture singular;
		Require(!singular.Begin(world, selection, ETransformOperation::Scale, ETransformSpace::Local, ETransformPivotMode::Pivot, 0), "singular parent inversion must reject the whole gesture safely");
	}

	void TestGroupCancellationGuardsSelectionParentsAndRevisions()
	{
		using namespace EditorViewport;
		BoundsTestWorld world;
		auto parent = world.Instantiate("Parent");
		auto a = world.Instantiate("A");
		auto b = world.Instantiate("B");
		a->SetParent(parent);
		const TVector<InstanceId> selection{ a->GetInstanceId(), b->GetInstanceId() };
		world.SetEditorSelection(selection);
		uint64_t objectRevision = 7;
		const ObjectMutationRevision revision = [&](const InstanceId&) { return objectRevision; };
		SelectionTransformGesture gesture;
		Require(gesture.Begin(world, selection, ETransformOperation::Translate, ETransformSpace::World, ETransformPivotMode::Pivot, 3, revision), "guarded gesture must capture");
		const auto start = gesture.GetGizmoMatrix();
		Require(gesture.Apply(world, glm::translate(glm::mat4(1.0f), glm::vec3(3, 0, 0)) * start, revision), "guarded gesture must apply");
		Require(!gesture.Validate(world, { b->GetInstanceId(), a->GetInstanceId() }, 3, revision) &&
			!gesture.Validate(world, selection, 4, revision), "selection order and selection revision invalidate an old gesture");
		parent->GetTransformComponent().SetPosition({ 2, 0, 0 });
		Require(!gesture.Validate(world, selection, 3, revision), "an unselected ancestor mutation must invalidate captured parent matrices");
		gesture.Cancel(world, revision);
		Require(AreVectorsNear(glm::vec3(CurrentWorld(a)[3]), { 2, 0, 0 }) && AreVectorsNear(glm::vec3(CurrentWorld(b)[3]), { 0, 0, 0 }),
			"cancel restores gesture locals while preserving the newer ancestor edit");
		Require(gesture.Begin(world, selection, ETransformOperation::Translate, ETransformSpace::World, ETransformPivotMode::Pivot, 3, revision), "a fresh gesture must capture");
		++objectRevision;
		Require(!gesture.Validate(world, selection, 3, revision), "each root managed mutation revision guards a stale gesture");
		gesture.Cancel(world, revision);
	}


	void TestTransformToolStateIsAppliedAtomically()
	{
		EditorViewport::EditorViewportController controller{};
		Require(controller.GetOperation() == EditorViewport::ETransformOperation::Translate,
			"the viewport controller must default to the translate tool");
		Require(controller.GetPivotMode() == EditorViewport::ETransformPivotMode::Pivot,
			"the viewport controller must default to Pivot mode");
		Require(controller.GetSpace() == EditorViewport::ETransformSpace::World,
			"the viewport controller must default to world space");
		Require(controller.SetTransformToolState(
			EditorViewport::ETransformOperation::Rotate,
			EditorViewport::ETransformSpace::Local,
			EditorViewport::ETransformPivotMode::Center),
			"a valid transform tool state must be accepted outside an active drag");
		Require(controller.GetOperation() == EditorViewport::ETransformOperation::Rotate &&
			controller.GetSpace() == EditorViewport::ETransformSpace::Local &&
			controller.GetPivotMode() == EditorViewport::ETransformPivotMode::Center,
			"accepted operation and space values must be applied together");
	}

	void TestAssetDropEventUsesValidatedViewportQueue()
	{
		EditorViewport::EditorViewportController controller{};
		controller.SetManagedSelectionRevision(17);
		{
			std::string fileId = "00000000000000ab:ignored suffix";
			Require(controller.QueueAssetDropEvent(std::string_view(fileId).substr(0, 16), 0.25f, 0.75f),
				"a finite normalized native asset drop must enter the viewport queue");
			fileId.assign(1024, 'x');
		}

		EditorViewport::Event event;
		Require(controller.PullEvent(event),
			"a queued native asset drop must be observable by the protocol bridge");
		const auto* drop = std::get_if<EditorViewport::AssetDropEvent>(&event.m_payload);
		Require(drop != nullptr,
			"the viewport queue must identify native asset-drop events");
		Require(event.m_revision == 1,
			"the first native asset drop must receive the first event revision");
		Require(event.m_managedMutationRevision == 17,
			"native asset drops must carry the current managed mutation revision");
		Require(drop->m_fileId == "00000000000000ab",
			"queued asset drops must own the bounded source FileId after its input buffer is destroyed");
		Require(std::abs(drop->m_position.x - 0.25f) <= c_tolerance &&
			std::abs(drop->m_position.y - 0.75f) <= c_tolerance,
			"native asset drops must preserve normalized viewport coordinates");
		Require(!controller.PullEvent(event),
			"pulling the only native asset drop must empty the viewport queue");

		const float nan = std::numeric_limits<float>::quiet_NaN();
		Require(!controller.QueueAssetDropEvent("", 0.5f, 0.5f),
			"an empty native asset FileId must be rejected");
		Require(!controller.QueueAssetDropEvent("asset", nan, 0.5f),
			"a non-finite native asset-drop coordinate must be rejected");
		Require(!controller.QueueAssetDropEvent("asset", -0.01f, 0.5f) &&
			!controller.QueueAssetDropEvent("asset", 0.5f, 1.01f),
			"native asset-drop coordinates outside the viewport must be rejected");
		Require(!controller.PullEvent(event),
			"rejected native asset drops must not enter the viewport queue");

		Require(controller.QueueAssetDropEvent("asset", 0.0f, 1.0f),
			"native asset drops on inclusive viewport edges must be accepted");
		Require(controller.PullEvent(event),
			"a second valid native asset drop must enter the queue");
		Require(event.m_revision == 2,
			"rejected native asset drops must not consume event revisions");
	}

	void TestToolShortcutEventUsesValidatedViewportQueue()
	{
		EditorViewport::EditorViewportController controller{};
		controller.SetManagedSelectionRevision(23);
		const uint32_t supportedKeys[] = { 'Q', 'W', 'E', 'R', 'T' };
		for (const uint32_t keyCode : supportedKeys)
		{
			Require(controller.QueueToolShortcutEvent(keyCode),
				"supported Win32 viewport shortcuts must enter the viewport queue");
		}
		Require(!controller.QueueToolShortcutEvent('X') &&
			!controller.QueueToolShortcutEvent('w'),
			"unknown and non-canonical viewport shortcuts must be rejected");

		EditorViewport::Event event;
		for (size_t index = 0; index < std::size(supportedKeys); ++index)
		{
			Require(controller.PullEvent(event),
				"every queued viewport shortcut must remain observable");
			const auto* shortcut = std::get_if<EditorViewport::ToolShortcutEvent>(&event.m_payload);
			Require(shortcut != nullptr,
				"viewport shortcut events must use their dedicated event kind");
			Require(event.m_revision == index + 1,
				"viewport shortcuts must share the ordered event revision stream");
			Require(event.m_managedMutationRevision == 23,
				"viewport shortcuts must carry the current managed mutation revision");
			Require(shortcut->m_keyCode == supportedKeys[index],
				"viewport shortcut events must preserve the detected key code");
		}
		Require(!controller.PullEvent(event),
			"rejected viewport shortcuts must not enter the event queue");
	}

	void TestConvertWorldToLocalTransformUnderParent()
	{
		const Math::Transform parentTransform(
			glm::vec4(5.0f, -3.0f, 8.0f, 1.0f),
			glm::angleAxis(glm::radians(35.0f), glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f))),
			glm::vec4(2.0f, 1.5f, 0.75f, 1.0f));
		const Math::Transform expectedLocalTransform(
			glm::vec4(-2.0f, 4.0f, 1.0f, 1.0f),
			glm::angleAxis(glm::radians(-20.0f), glm::normalize(glm::vec3(2.0f, 1.0f, 0.5f))),
			glm::vec4(0.5f, 1.25f, 2.0f, 1.0f));
		const glm::mat4 parentWorldMatrix = parentTransform.Matrix();
		const glm::mat4 worldMatrix = parentWorldMatrix * expectedLocalTransform.Matrix();
		Math::Transform actualLocalTransform;

		Require(EditorViewport::TryConvertWorldToLocalTransform(
			worldMatrix,
			&parentWorldMatrix,
			actualLocalTransform),
			"an exactly representable world transform must convert through an invertible parent");
		Require(AreMatricesNear(actualLocalTransform.Matrix(), expectedLocalTransform.Matrix()),
			"world-to-local conversion must preserve the expected child-local transform");
	}

	void TestConvertWorldToLocalTransformRejectsSingularParent()
	{
		const glm::mat4 worldMatrix = glm::translate(glm::identity<glm::mat4>(), glm::vec3(2.0f, 3.0f, 4.0f));
		const glm::mat4 singularParentMatrix = glm::scale(
			glm::translate(glm::identity<glm::mat4>(), glm::vec3(1.0f, 0.0f, 0.0f)),
			glm::vec3(1.0f, 0.0f, 1.0f));
		Math::Transform output(
			glm::vec4(7.0f, 8.0f, 9.0f, 1.0f),
			glm::quat(1.0f, 0.0f, 0.0f, 0.0f),
			glm::vec4(2.0f, 2.0f, 2.0f, 1.0f));
		const glm::mat4 outputBefore = output.Matrix();

		Require(!EditorViewport::TryConvertWorldToLocalTransform(
			worldMatrix,
			&singularParentMatrix,
			output),
			"world-to-local conversion must reject a singular parent transform");
		Require(AreMatricesNear(output.Matrix(), outputBefore),
			"a rejected conversion must leave the output transform unchanged");
	}

	void TestSelectionGesturePolicyRejectsNavigationModifiersAndUiOwnership()
	{
		Require(EditorViewport::CanBeginSelectionGesture(false, false, false, false),
			"an unmodified viewport click must be eligible for selection");
		Require(!EditorViewport::CanBeginSelectionGesture(true, false, false, false),
			"modifier-assisted gestures must not start selection");
		Require(!EditorViewport::CanBeginSelectionGesture(false, true, false, false),
			"LMB while RMB navigation is held must not start selection");
		Require(!EditorViewport::CanBeginSelectionGesture(false, false, true, false),
			"a gizmo-owned click must not start selection");
		Require(!EditorViewport::CanBeginSelectionGesture(false, false, false, true),
			"an editor UI-owned click must not reach scene selection");
		Require(!EditorViewport::ShouldCancelSelectionGesture(false, true, false),
			"an unarmed pointer gesture has no selection state to cancel");
		Require(EditorViewport::ShouldCancelSelectionGesture(true, true, false),
			"pressing a modifier after LMB must cancel the armed selection gesture");
		Require(EditorViewport::ShouldCancelSelectionGesture(true, false, true),
			"starting RMB navigation after LMB must cancel the armed selection gesture");
	}

	void TestOnlySubmittedGizmoOwnsPointerForCurrentFrame()
	{
		Require(!EditorViewport::DoesSubmittedGizmoOwnPointer(false, true, false),
			"stale hover state from a gizmo that was not submitted this frame must not block selection");
		Require(!EditorViewport::DoesSubmittedGizmoOwnPointer(false, false, true),
			"stale drag state from a gizmo that was not submitted this frame must not block selection");
		Require(EditorViewport::DoesSubmittedGizmoOwnPointer(true, true, false),
			"a gizmo submitted and hovered this frame must own the pointer");
		Require(EditorViewport::DoesSubmittedGizmoOwnPointer(true, false, true),
			"a gizmo submitted and dragged this frame must own the pointer");
	}

	void TestResolveGameObjectBoundsAggregatesSelectableGeometry()
	{
		BoundsTestWorld world;
		auto parent = world.Instantiate("Parent");
		auto meshObject = world.Instantiate("MeshObject");
		meshObject->SetParent(parent);

		parent->GetTransformComponent().SetPosition(glm::vec4(10.0f, -4.0f, 7.0f, 1.0f));
		parent->GetTransformComponent().SetRotation(glm::angleAxis(glm::radians(25.0f), glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f))));
		parent->GetTransformComponent().SetScale(glm::vec4(1.5f, 0.75f, 2.0f, 1.0f));
		meshObject->GetTransformComponent().SetPosition(glm::vec4(-3.0f, 2.0f, 5.0f, 1.0f));
		meshObject->GetTransformComponent().SetRotation(glm::angleAxis(glm::radians(-35.0f), glm::normalize(glm::vec3(1.0f, 2.0f, 0.5f))));
		meshObject->GetTransformComponent().SetScale(glm::vec4(-2.0f, 1.25f, 0.5f, 1.0f));

		const Math::AABB firstLocalBounds = MakeBounds(
			glm::vec3(-1.0f, -2.0f, -0.5f),
			glm::vec3(2.0f, 1.0f, 3.0f));
		const Math::AABB secondLocalBounds = MakeBounds(
			glm::vec3(-4.0f, -0.25f, -1.0f),
			glm::vec3(-2.0f, 0.75f, 1.5f));
		auto firstModel = TObjectPtr<BoundsTestModel>::Make(world.GetAllocator());
		auto secondModel = TObjectPtr<BoundsTestModel>::Make(world.GetAllocator());
		firstModel->SetReadyBounds(firstLocalBounds);
		secondModel->SetReadyBounds(secondLocalBounds);
		meshObject->AddComponent<MeshRendererComponent>()->SetModel(firstModel);
		meshObject->AddComponent<MeshRendererComponent>()->SetModel(secondModel);

		const glm::mat4 worldMatrix =
			parent->GetTransformComponent().GetTransform().Matrix() *
			meshObject->GetTransformComponent().GetTransform().Matrix();
		Math::AABB expectedFirst = firstLocalBounds;
		Math::AABB expectedSecond = secondLocalBounds;
		expectedFirst.Apply(worldMatrix);
		expectedSecond.Apply(worldMatrix);
		expectedFirst.Extend(expectedSecond);

		Math::AABB actualBounds;
		bool bUsesSelectableGeometry = false;
		Require(EditorViewport::ResolveGameObjectBounds(meshObject, actualBounds, bUsesSelectableGeometry),
			"a hierarchy object with ready mesh renderers must expose selectable world bounds");
		Require(bUsesSelectableGeometry,
			"ready mesh renderers must be distinguished from the origin fallback");
		Require(AreVectorsNear(actualBounds.m_min, expectedFirst.m_min) &&
			AreVectorsNear(actualBounds.m_max, expectedFirst.m_max),
			"mesh bounds must aggregate every renderer after hierarchy, rotation, and negative-scale transforms");

		auto collisionObject = world.Instantiate("CollisionObject");
		collisionObject->GetTransformComponent().SetPosition(
			glm::vec4(4.0f, 5.0f, 6.0f, 1.0f));
		collisionObject->GetTransformComponent().SetScale(
			glm::vec4(-2.0f, 3.0f, 4.0f, 1.0f));
		auto boxShape = collisionObject->AddComponent<CollisionShapeComponent>();
		boxShape->SetShapeType(Physics::ECollisionShapeType::Box);
		boxShape->SetCenter(glm::vec3(1.0f, -1.0f, 0.5f));
		boxShape->SetSize(glm::vec3(2.0f));
		auto sphereShape = collisionObject->AddComponent<CollisionShapeComponent>();
		sphereShape->SetShapeType(Physics::ECollisionShapeType::Sphere);
		sphereShape->SetCenter(glm::vec3(-2.0f, 0.0f, 0.0f));
		sphereShape->SetRadius(0.5f);
		auto capsuleShape = collisionObject->AddComponent<CollisionShapeComponent>();
		capsuleShape->SetShapeType(Physics::ECollisionShapeType::Capsule);
		capsuleShape->SetCenter(glm::vec3(0.0f, 0.0f, -2.0f));
		capsuleShape->SetRadius(0.25f);
		capsuleShape->SetHeight(2.0f);

		bUsesSelectableGeometry = false;
		Require(EditorViewport::ResolveGameObjectBounds(
				collisionObject,
				actualBounds,
				bUsesSelectableGeometry),
			"an object with collision primitives but no mesh must expose selectable bounds");
		Require(bUsesSelectableGeometry,
			"collision primitives must participate in viewport picking");
		Require(AreVectorsNear(actualBounds.m_min, glm::vec3(0.0f, -1.0f, -3.0f)) &&
			AreVectorsNear(actualBounds.m_max, glm::vec3(10.0f, 8.0f, 12.0f)),
			"selection bounds must aggregate every collision primitive with signed owner scale and local offsets");

		auto fallbackObject = world.Instantiate("FallbackObject");
		fallbackObject->SetParent(parent);
		fallbackObject->GetTransformComponent().SetPosition(glm::vec4(1.0f, 3.0f, -2.0f, 1.0f));
		const glm::mat4 fallbackWorldMatrix =
			parent->GetTransformComponent().GetTransform().Matrix() *
			fallbackObject->GetTransformComponent().GetTransform().Matrix();
		const glm::vec3 fallbackPosition(fallbackWorldMatrix[3]);
		bUsesSelectableGeometry = true;
		Require(EditorViewport::ResolveGameObjectBounds(fallbackObject, actualBounds, bUsesSelectableGeometry),
			"an object without a ready mesh must expose its center for the selected-object marker");
		Require(!bUsesSelectableGeometry,
			"selection fallback bounds must be excluded from viewport mesh picking");
		Require(AreVectorsNear(actualBounds.GetCenter(), fallbackPosition),
			"selection fallback must use the fully composed world position");

		auto editorOnlyObject = world.Instantiate("EditorOnlyObject");
		editorOnlyObject->AddComponent<EditorComponent>();
		bUsesSelectableGeometry = true;
		Require(!EditorViewport::ResolveGameObjectBounds(editorOnlyObject, actualBounds, bUsesSelectableGeometry),
			"editor-only infrastructure must never enter scene picking candidates");
		Require(!bUsesSelectableGeometry,
			"rejected editor-only objects must clear the mesh-bound result flag");

		world.Clear();
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "GroupTranslationTrimsRootsAndRetainsActiveChild", TestGroupTranslationTrimsRootsAndRetainsActiveChild },
		{ "GroupCenterUsesHierarchyGeometryAndEmptyOrigins", TestGroupCenterUsesHierarchyGeometryAndEmptyOrigins },
		{ "GroupRotationPivotCenterLocalWorld", TestGroupRotationPivotCenterLocalWorld },
		{ "GroupScalePivotCenterLocalWorldAndSignedZero", TestGroupScalePivotCenterLocalWorldAndSignedZero },
		{ "ZeroScaleRotationStillCompletesAuthoredUndo", TestZeroScaleRotationStillCompletesAuthoredUndo },
		{ "GroupDifferentParentsAndSingularRejection", TestGroupDifferentParentsAndSingularRejection },
		{ "GroupCancellationGuardsSelectionParentsAndRevisions", TestGroupCancellationGuardsSelectionParentsAndRevisions },
		{ "PrimarySelectionKeepsWorldOrder", TestPrimarySelectionKeepsWorldOrder },
		{ "PrimarySelectionTracksObjectLifetime", TestPrimarySelectionTracksObjectLifetime },
		{ "BuildWorldRayUsesReversedZAtViewportCenter", TestBuildWorldRayUsesReversedZAtViewportCenter },
		{ "BuildWorldRayMapsViewportCorners", TestBuildWorldRayMapsViewportCorners },
		{ "BuildWorldRayRejectsInvalidViewport", TestBuildWorldRayRejectsInvalidViewport },
		{ "PickNearestUsesDistance", TestPickNearestUsesDistance },
		{ "PickNearestTreatsInsideBoundsAsZeroDistance", TestPickNearestTreatsInsideBoundsAsZeroDistance },
		{ "ResolveRayTargetUsesNearestMeshBounds", TestResolveRayTargetUsesNearestMeshBounds },
		{ "ResolveRayTargetUsesGroundPlane", TestResolveRayTargetUsesGroundPlane },
		{ "ResolveRayTargetUsesFiniteForwardFallback", TestResolveRayTargetUsesFiniteForwardFallback },
		{ "ResolveRayTargetRejectsInvalidRay", TestResolveRayTargetRejectsInvalidRay },
		{ "CalculateFramedCameraPositionPreservesViewDirection", TestCalculateFramedCameraPositionPreservesViewDirection },
		{ "CalculateFramedCameraPositionAccountsForViewportAspect", TestCalculateFramedCameraPositionAccountsForViewportAspect },
		{ "CalculateFramedCameraPositionRejectsInvalidInput", TestCalculateFramedCameraPositionRejectsInvalidInput },
		{ "CameraFramingUsesCanonicalRotation", TestCameraFramingUsesCanonicalRotation },
			{ "TransformToolStateIsAppliedAtomically", TestTransformToolStateIsAppliedAtomically },
			{ "AssetDropEventUsesValidatedViewportQueue", TestAssetDropEventUsesValidatedViewportQueue },
			{ "ToolShortcutEventUsesValidatedViewportQueue", TestToolShortcutEventUsesValidatedViewportQueue },
		{ "ConvertWorldToLocalTransformUnderParent", TestConvertWorldToLocalTransformUnderParent },
		{ "ConvertWorldToLocalTransformRejectsSingularParent", TestConvertWorldToLocalTransformRejectsSingularParent },
		{ "PickNearestBreaksTiesDeterministically", TestPickNearestBreaksTiesDeterministically },
		{ "SelectionGesturePolicyRejectsNavigationModifiersAndUiOwnership", TestSelectionGesturePolicyRejectsNavigationModifiersAndUiOwnership },
		{ "OnlySubmittedGizmoOwnsPointerForCurrentFrame", TestOnlySubmittedGizmoOwnsPointerForCurrentFrame },
		{ "ResolveGameObjectBoundsAggregatesSelectableGeometry", TestResolveGameObjectBoundsAggregatesSelectableGeometry },
	};

	for (const auto& test : tests)
	{
		try
		{
			test.second();
			std::cout << "[PASS] " << test.first << std::endl;
		}
		catch (const std::exception& error)
		{
			std::cerr << "[FAIL] " << test.first << ": " << error.what() << std::endl;
			return 1;
		}
	}

	return 0;
}
