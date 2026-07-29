#pragma once

#include "Containers/Vector.h"
#include "Engine/InstanceId.h"
#include "Math/Bounds.h"
#include "Math/Transform.h"
#include "Memory/ObjectPtr.hpp"

#include <string>

namespace Sailor
{
	class GameObject;
	class World;

	namespace EditorViewport
	{
		enum class ETransformOperation : uint8_t
		{
			Select = 0,
			Translate,
			Rotate,
			Scale
		};

		enum class ETransformSpace : uint8_t
		{
			World = 0,
			Local
		};

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

		SAILOR_API bool TryResolveDropPosition(
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
			bool& outUsesMeshBounds);

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

		class EditorViewportController final
		{
		public:
			SAILOR_API void Tick(World& world);
			SAILOR_API void Reset();
			SAILOR_API void CancelPointerInteraction();
			SAILOR_API void CancelInteraction(World& world);
			SAILOR_API bool PullEvent(std::string& outEvent);
			SAILOR_API bool QueueAssetDropEvent(
				const std::string& fileId,
				float normalizedX,
				float normalizedY);
			SAILOR_API bool QueueToolShortcutEvent(uint32_t keyCode);
			SAILOR_API bool TryResolveDropPosition(
				World& world,
				float normalizedX,
				float normalizedY,
				glm::vec3& outPosition) const;
			SAILOR_API bool FocusCameraOnObject(
				World& world,
				const InstanceId& instanceId);
			SAILOR_API bool SetTransformToolState(
				ETransformOperation operation,
				ETransformSpace space);
			void SetManagedMutationRevisions(uint64_t selectionRevision, uint64_t selectedObjectRevision)
			{
				m_managedSelectionMutationRevision = selectionRevision;
				m_selectedObjectMutationRevision = selectedObjectRevision;
			}

			SAILOR_API ETransformOperation GetOperation() const { return m_operation; }
			SAILOR_API ETransformSpace GetSpace() const { return m_space; }

		private:
			TObjectPtr<GameObject> ResolveSelectedObject(World& world) const;
			void CompleteActiveTransform(World& world);
			void TickTransformGizmo(World& world, TObjectPtr<GameObject> selectedObject);
			void TickSelection(World& world);
			void QueueSelectionEvent(const InstanceId& selectedInstanceId);
			void QueueTransformEvent(
				const InstanceId& instanceId,
				const Math::Transform& beforeTransform,
				const Math::Transform& afterTransform,
				ETransformOperation operation,
				ETransformSpace space);

			ETransformOperation m_operation = ETransformOperation::Translate;
			ETransformSpace m_space = ETransformSpace::World;

			bool m_wasUsingGizmo = false;
			bool m_gizmoSubmittedThisFrame = false;
			bool m_selectionClickArmed = false;
			bool m_gizmoConsumedClick = false;
			glm::vec2 m_selectionClickStart{};

			InstanceId m_dragInstanceId{};
			Math::Transform m_dragStartTransform{};
			ETransformOperation m_dragOperation = ETransformOperation::Translate;
			ETransformSpace m_dragSpace = ETransformSpace::World;

			uint64_t m_eventRevision = 0;
			uint64_t m_managedSelectionMutationRevision = 0;
			uint64_t m_selectedObjectMutationRevision = 0;
			uint64_t m_dragManagedObjectMutationRevision = 0;
			int32_t m_lastTickFrame = -1;
			TVector<std::string> m_pendingEvents{};
		};
	}
}
