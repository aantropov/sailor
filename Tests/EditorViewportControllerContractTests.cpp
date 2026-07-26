#include "Editor/EditorViewportController.h"
#include "AssetRegistry/Model/ModelImporter.h"
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
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

using namespace Sailor;

namespace
{
	constexpr float c_tolerance = 0.001f;

	void Require(bool condition, const std::string& message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
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

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			return systems;
		}
	};

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

	void TestResolveGameObjectBoundsDistinguishesMeshPickingFromSelectionFallback()
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
		bool bUsesMeshBounds = false;
		Require(EditorViewport::ResolveGameObjectBounds(meshObject, actualBounds, bUsesMeshBounds),
			"a hierarchy object with ready mesh renderers must expose selectable world bounds");
		Require(bUsesMeshBounds,
			"ready mesh renderers must be distinguished from the origin fallback");
		Require(AreVectorsNear(actualBounds.m_min, expectedFirst.m_min) &&
			AreVectorsNear(actualBounds.m_max, expectedFirst.m_max),
			"mesh bounds must aggregate every renderer after hierarchy, rotation, and negative-scale transforms");

		auto fallbackObject = world.Instantiate("FallbackObject");
		fallbackObject->SetParent(parent);
		fallbackObject->GetTransformComponent().SetPosition(glm::vec4(1.0f, 3.0f, -2.0f, 1.0f));
		const glm::mat4 fallbackWorldMatrix =
			parent->GetTransformComponent().GetTransform().Matrix() *
			fallbackObject->GetTransformComponent().GetTransform().Matrix();
		const glm::vec3 fallbackPosition(fallbackWorldMatrix[3]);
		bUsesMeshBounds = true;
		Require(EditorViewport::ResolveGameObjectBounds(fallbackObject, actualBounds, bUsesMeshBounds),
			"an object without a ready mesh must expose its center for the selected-object marker");
		Require(!bUsesMeshBounds,
			"selection fallback bounds must be excluded from viewport mesh picking");
		Require(AreVectorsNear(actualBounds.GetCenter(), fallbackPosition),
			"selection fallback must use the fully composed world position");

		auto editorOnlyObject = world.Instantiate("EditorOnlyObject");
		editorOnlyObject->AddComponent<EditorComponent>();
		bUsesMeshBounds = true;
		Require(!EditorViewport::ResolveGameObjectBounds(editorOnlyObject, actualBounds, bUsesMeshBounds),
			"editor-only infrastructure must never enter scene picking candidates");
		Require(!bUsesMeshBounds,
			"rejected editor-only objects must clear the mesh-bound result flag");

		world.Clear();
	}
}

int main()
{
	const std::pair<const char*, std::function<void()>> tests[] = {
		{ "BuildWorldRayUsesReversedZAtViewportCenter", TestBuildWorldRayUsesReversedZAtViewportCenter },
		{ "BuildWorldRayMapsViewportCorners", TestBuildWorldRayMapsViewportCorners },
		{ "BuildWorldRayRejectsInvalidViewport", TestBuildWorldRayRejectsInvalidViewport },
		{ "PickNearestUsesDistance", TestPickNearestUsesDistance },
		{ "PickNearestTreatsInsideBoundsAsZeroDistance", TestPickNearestTreatsInsideBoundsAsZeroDistance },
		{ "ConvertWorldToLocalTransformUnderParent", TestConvertWorldToLocalTransformUnderParent },
		{ "ConvertWorldToLocalTransformRejectsSingularParent", TestConvertWorldToLocalTransformRejectsSingularParent },
		{ "PickNearestBreaksTiesDeterministically", TestPickNearestBreaksTiesDeterministically },
		{ "SelectionGesturePolicyRejectsNavigationModifiersAndUiOwnership", TestSelectionGesturePolicyRejectsNavigationModifiersAndUiOwnership },
		{ "OnlySubmittedGizmoOwnsPointerForCurrentFrame", TestOnlySubmittedGizmoOwnsPointerForCurrentFrame },
		{ "ResolveGameObjectBoundsDistinguishesMeshPickingFromSelectionFallback", TestResolveGameObjectBoundsDistinguishesMeshPickingFromSelectionFallback },
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
