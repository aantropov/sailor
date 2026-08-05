#pragma once
#include "Components/Component.h"
#include "Physics/PhysicsTypes.h"

namespace Sailor
{
	class CollisionShapeComponent final : public Component
	{
		SAILOR_REFLECTABLE(CollisionShapeComponent)

	public:
		SAILOR_API void Initialize() override;
		SAILOR_API void EndPlay() override;

		SAILOR_API Physics::ECollisionShapeType GetShapeType() const { return m_shapeType; }
		SAILOR_API void SetShapeType(Physics::ECollisionShapeType value);
		SAILOR_API const glm::vec3& GetCenter() const { return m_center; }
		SAILOR_API void SetCenter(const glm::vec3& value);
		SAILOR_API const glm::quat& GetRotation() const { return m_rotation; }
		SAILOR_API void SetRotation(const glm::quat& value);
		SAILOR_API const glm::vec3& GetSize() const { return m_size; }
		SAILOR_API void SetSize(const glm::vec3& value);
		SAILOR_API float GetRadius() const { return m_radius; }
		SAILOR_API void SetRadius(float value);
		SAILOR_API float GetHeight() const { return m_height; }
		SAILOR_API void SetHeight(float value);

		SAILOR_API Physics::CollisionShapeDesc BuildDesc() const;

	private:
		void NotifyRigidBody();

		Physics::ECollisionShapeType m_shapeType =
			Physics::ECollisionShapeType::Box;
		glm::vec3 m_center{};
		glm::quat m_rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		glm::vec3 m_size = glm::vec3(1.0f);
		float m_radius = 0.5f;
		float m_height = 1.0f;
	};
}

using namespace Sailor::Attributes;

REFL_AUTO(
	type(Sailor::CollisionShapeComponent, bases<Sailor::Component>),
	func(GetShapeType, property("shapeType")),
	func(SetShapeType, property("shapeType")),
	func(GetCenter, property("center")),
	func(SetCenter, property("center")),
	func(GetRotation, property("rotation")),
	func(SetRotation, property("rotation")),
	func(GetSize, property("size")),
	func(SetSize, property("size")),
	func(GetRadius, property("radius")),
	func(SetRadius, property("radius")),
	func(GetHeight, property("height")),
	func(SetHeight, property("height"))
)
