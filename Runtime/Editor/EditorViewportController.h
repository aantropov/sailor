#pragma once

#include "Containers/Vector.h"
#include "Editor/EditorViewportEvent.h"
#include "Math/Bounds.h"
#include "Math/Transform.h"
#include "Memory/ObjectPtr.hpp"

#include <functional>
#include <string>
#include <string_view>

namespace Sailor
{
	class GameObject;
	class World;

	namespace EditorViewport
	{
		struct PickCandidate
		{
			InstanceId m_instanceId{};
			Math::AABB m_worldBounds{};
		};

		SAILOR_API bool BuildWorldRay(
			float pointerX,
			float pointerY,
			float viewportWidth,
			float viewportHeight,
			const glm::mat4& view,
			const glm::mat4& projection,
			Math::Ray& outRay);

		SAILOR_API bool TryPickNearest(
			const Math::Ray& ray,
			const TVector<PickCandidate>& candidates,
			InstanceId& outInstanceId,
			float& outDistance);

		SAILOR_API bool TryResolveRayTarget(
			const Math::Ray& ray,
			const TVector<PickCandidate>& candidates,
			glm::vec3& outPosition);

		SAILOR_API bool TryCalculateFramedCameraPosition(
			const Math::AABB& bounds,
			const Math::Transform& cameraWorldTransform,
			float verticalFovDegrees,
			float aspect,
			float zNear,
			glm::vec3& outPosition);

		SAILOR_API bool TryConvertWorldToLocalTransform(
			const glm::mat4& worldMatrix,
			const glm::mat4* parentWorldMatrix,
			Math::Transform& outLocalTransform);

		SAILOR_API bool ResolveGameObjectBounds(
			const TObjectPtr<GameObject>& gameObject,
			Math::AABB& outWorldBounds,
			bool& outUsesSelectableGeometry);

		SAILOR_API bool CanBeginSelectionGesture(
			bool bHasModifiers,
			bool bNavigatingViewport,
			bool bGizmoOwnsPointer,
			bool bEditorUiOwnsPointer);
		SAILOR_API bool ShouldCancelSelectionGesture(
			bool bGestureArmed,
			bool bHasModifiers,
			bool bNavigatingViewport);
		SAILOR_API bool DoesSubmittedGizmoOwnPointer(
			bool bGizmoSubmitted,
			bool bIsOver,
			bool bIsUsing);

		using ObjectMutationRevision = std::function<uint64_t(const InstanceId&)>;

		// One immutable start state drives the entire selection gesture, including
		// roots under different parents and signed or zero authored scales.
		class SelectionTransformGesture final
		{
		public:
			SAILOR_API bool Begin(World& world, const TVector<InstanceId>& selection,
				ETransformOperation operation, ETransformSpace space, ETransformPivotMode pivotMode,
				uint64_t selectionRevision, const ObjectMutationRevision& revision = {});
			SAILOR_API bool Validate(World& world, const TVector<InstanceId>& selection,
				uint64_t selectionRevision, const ObjectMutationRevision& revision = {}) const;
			SAILOR_API bool Apply(World& world, const glm::mat4& gizmoMatrix,
				const ObjectMutationRevision& revision = {});
			SAILOR_API void Cancel(World& world, const ObjectMutationRevision& revision = {});
			SAILOR_API bool Complete(TransformEvent& outEvent);
			bool IsActive() const { return m_world != nullptr; }
			const glm::mat4& GetGizmoMatrix() const { return m_gizmoMatrix; }
			uint64_t GetSelectionRevision() const { return m_selectionRevision; }

		private:
			struct Target
			{
				InstanceId m_id{}, m_parentId{};
				Math::Transform m_before{}, m_applied{};
				glm::mat4 m_world{ 1.0f }, m_parentWorld{ 1.0f }, m_inverseParent{ 1.0f };
				glm::quat m_worldRotation{}, m_parentRotation{};
				glm::ivec3 m_scaleAxes{ 0, 1, 2 };
				uint64_t m_revision = 0;
			};
			World* m_world = nullptr;
			TVector<InstanceId> m_selection{}, m_selectedParents{};
			TVector<Math::Transform> m_selectedLocals{};
			TVector<uint64_t> m_selectedRevisions{};
			TVector<Target> m_targets{};
			glm::mat4 m_startGizmo{ 1.0f }, m_gizmoMatrix{ 1.0f };
			glm::quat m_basis{ 1.0f, 0.0f, 0.0f, 0.0f };
			ETransformOperation m_operation = ETransformOperation::Translate;
			ETransformSpace m_space = ETransformSpace::World;
			ETransformPivotMode m_pivotMode = ETransformPivotMode::Pivot;
			uint64_t m_selectionRevision = 0;
		};

		class EditorViewportController final
		{
		public:
			SAILOR_API void Tick(World& world, const ObjectMutationRevision& revision = {});
			SAILOR_API void Reset();
			SAILOR_API void CancelPointerInteraction();
			SAILOR_API void CancelInteraction(World& world, const ObjectMutationRevision& revision = {});
			SAILOR_API void SetManagedSelection(const TVector<InstanceId>& selection);
			SAILOR_API TVector<InstanceId> GetSelectionOrder(World& world);
			SAILOR_API bool PullEvent(Event& outEvent);
			SAILOR_API bool QueueAssetDropEvent(
				std::string_view fileId,
				float normalizedX,
				float normalizedY);
			SAILOR_API bool QueueToolShortcutEvent(uint32_t keyCode);
			SAILOR_API bool TraceViewportRay(
				World& world,
				float normalizedX,
				float normalizedY,
				glm::vec3& outPosition) const;
			SAILOR_API bool FocusCameraOnObject(
				World& world,
				const InstanceId& instanceId);
			SAILOR_API bool SetTransformToolState(
				ETransformOperation operation,
				ETransformSpace space,
				ETransformPivotMode pivotMode = ETransformPivotMode::Pivot);
			void SetManagedSelectionRevision(uint64_t selectionRevision)
			{
				m_managedSelectionMutationRevision = selectionRevision;
			}

			SAILOR_API ETransformOperation GetOperation() const { return m_operation; }
			SAILOR_API ETransformSpace GetSpace() const { return m_space; }
			SAILOR_API ETransformPivotMode GetPivotMode() const { return m_pivotMode; }

		private:
			void CompleteActiveTransform();
			void TickTransformGizmo(World& world, const ObjectMutationRevision& revision);
			void SynchronizeSelectionOrder(World& world);
			void TickSelection(World& world);
			void ApplySelectionPick(World& world, const InstanceId& pickedInstanceId, bool bAdditive);
			void QueueSelectionEvent(const InstanceId& selectedInstanceId, bool bAdditive);
			ETransformOperation m_operation = ETransformOperation::Translate;
			ETransformSpace m_space = ETransformSpace::World;
			ETransformPivotMode m_pivotMode = ETransformPivotMode::Pivot;
			SelectionTransformGesture m_gesture{};

			bool m_wasUsingGizmo = false;
			bool m_transformBlockedUntilRelease = false;
			bool m_gizmoSubmittedThisFrame = false;
			bool m_selectionClickArmed = false;
			bool m_selectionClickAdditive = false;
			bool m_gizmoConsumedClick = false;
			glm::vec2 m_selectionClickStart{};

			uint64_t m_eventRevision = 0;
			uint64_t m_managedSelectionMutationRevision = 0;
			int32_t m_lastTickFrame = -1;
			TVector<Event> m_pendingEvents{};
			TVector<InstanceId> m_selectionOrder{};
		};
	}
}
