#include "Physics/PhysicsWorld.h"
#include "Physics/JoltJobSystem.h"
#include "Core/LogMacros.h"
#include "Math/Math.h"
#include "Sailor.h"
#include "Tasks/Scheduler.h"
#include "Tasks/Tasks.h"
#include <Jolt/Jolt.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#if __has_include(<concurrent_queue.h>)
#include <concurrent_queue.h>
#elif __has_include(<tbb/concurrent_queue.h>)
#include <tbb/concurrent_queue.h>
namespace concurrency = tbb;
#endif

using namespace Sailor;

namespace
{
	constexpr JPH::ObjectLayer c_nonMovingLayerBase = 0;
	constexpr JPH::ObjectLayer c_movingLayerBase = 16;
	constexpr uint8_t c_numCollisionLayers = 16;
	constexpr float c_minShapeExtent = 0.001f;

	uint8_t GetCollisionLayer(JPH::ObjectLayer layer)
	{
		return static_cast<uint8_t>(layer & 0x0f);
	}

	bool IsFinite(const glm::quat& value)
	{
		return Math::AllFinite(glm::vec4(
			value.x,
			value.y,
			value.z,
			value.w));
	}

	glm::quat SanitizeRotation(const glm::quat& value)
	{
		const glm::vec4 raw(value.x, value.y, value.z, value.w);
		const glm::vec4 normalized = Math::SafeNormalize(
			raw,
			glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
		return glm::quat(
			normalized.w,
			normalized.x,
			normalized.y,
			normalized.z);
	}

	JPH::Vec3 ToJolt(const glm::vec3& value)
	{
		return JPH::Vec3(value.x, value.y, value.z);
	}

	JPH::RVec3 ToJoltPosition(const glm::vec3& value)
	{
		return JPH::RVec3(value.x, value.y, value.z);
	}

	JPH::Quat ToJolt(const glm::quat& value)
	{
		const glm::quat normalized = SanitizeRotation(value);
		return JPH::Quat(normalized.x, normalized.y, normalized.z, normalized.w);
	}

	template<typename TVector>
	glm::vec3 FromJoltVector(const TVector& value)
	{
		return glm::vec3(
			static_cast<float>(value.GetX()),
			static_cast<float>(value.GetY()),
			static_cast<float>(value.GetZ()));
	}

	glm::quat FromJoltQuat(JPH::QuatArg value)
	{
		return SanitizeRotation(glm::quat(
			value.GetW(),
			value.GetX(),
			value.GetY(),
			value.GetZ()));
	}

	JPH::ObjectLayer MakeObjectLayer(
		Physics::ERigidBodyMotionType motionType,
		uint8_t collisionLayer)
	{
		const JPH::ObjectLayer localLayer =
			static_cast<JPH::ObjectLayer>(collisionLayer & 0x0f);
		return motionType == Physics::ERigidBodyMotionType::Static
			? c_nonMovingLayerBase + localLayer
			: c_movingLayerBase + localLayer;
	}

	class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface
	{
	public:
		JPH::uint GetNumBroadPhaseLayers() const override { return 2; }

		JPH::BroadPhaseLayer GetBroadPhaseLayer(
			JPH::ObjectLayer layer) const override
		{
			return JPH::BroadPhaseLayer(
				layer >= c_movingLayerBase ? 1 : 0);
		}

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
		const char* GetBroadPhaseLayerName(
			JPH::BroadPhaseLayer layer) const override
		{
			return static_cast<JPH::BroadPhaseLayer::Type>(layer) == 0
				? "NonMoving"
				: "Moving";
		}
#endif
	};

	class ObjectVsBroadPhaseLayerFilter final :
		public JPH::ObjectVsBroadPhaseLayerFilter
	{
	public:
		bool ShouldCollide(
			JPH::ObjectLayer objectLayer,
			JPH::BroadPhaseLayer broadPhaseLayer) const override
		{
			const bool bMoving = objectLayer >= c_movingLayerBase;
			const bool bBroadPhaseMoving =
				static_cast<JPH::BroadPhaseLayer::Type>(broadPhaseLayer) == 1;
			return bMoving || bBroadPhaseMoving;
		}
	};

	class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter
	{
	public:
		ObjectLayerPairFilter()
		{
			for (auto& mask : m_collisionMasks)
			{
				mask.store(0xffffu, std::memory_order_relaxed);
			}
		}

		bool ShouldCollide(
			JPH::ObjectLayer first,
			JPH::ObjectLayer second) const override
		{
			if (first < c_movingLayerBase && second < c_movingLayerBase)
			{
				return false;
			}

			const uint8_t firstLayer = GetCollisionLayer(first);
			const uint8_t secondLayer = GetCollisionLayer(second);
			return (m_collisionMasks[firstLayer].load(
				std::memory_order_relaxed) & (1u << secondLayer)) != 0;
		}

		void SetCollisionEnabled(
			uint8_t firstLayer,
			uint8_t secondLayer,
			bool bEnabled)
		{
			firstLayer &= 0x0f;
			secondLayer &= 0x0f;
			const uint16_t firstBit = static_cast<uint16_t>(1u << secondLayer);
			const uint16_t secondBit = static_cast<uint16_t>(1u << firstLayer);
			if (bEnabled)
			{
				m_collisionMasks[firstLayer].fetch_or(
					firstBit,
					std::memory_order_relaxed);
				m_collisionMasks[secondLayer].fetch_or(
					secondBit,
					std::memory_order_relaxed);
			}
			else
			{
				m_collisionMasks[firstLayer].fetch_and(
					static_cast<uint16_t>(~firstBit),
					std::memory_order_relaxed);
				m_collisionMasks[secondLayer].fetch_and(
					static_cast<uint16_t>(~secondBit),
					std::memory_order_relaxed);
			}
		}

		bool IsCollisionEnabled(
			uint8_t firstLayer,
			uint8_t secondLayer) const
		{
			firstLayer &= 0x0f;
			secondLayer &= 0x0f;
			return (m_collisionMasks[firstLayer].load(
				std::memory_order_relaxed) & (1u << secondLayer)) != 0;
		}

	private:
		std::atomic<uint16_t> m_collisionMasks[c_numCollisionLayers]{};
	};

	class QueryObjectLayerFilter final : public JPH::ObjectLayerFilter
	{
	public:
		explicit QueryObjectLayerFilter(uint16_t collisionMask) :
			m_collisionMask(collisionMask)
		{}

		bool ShouldCollide(JPH::ObjectLayer layer) const override
		{
			return (m_collisionMask &
				(1u << GetCollisionLayer(layer))) != 0;
		}

	private:
		uint16_t m_collisionMask = 0xffffu;
	};

	JPH::ShapeRefC BuildPrimitiveShape(
		const Physics::CollisionShapeDesc& desc,
		const glm::vec3& bodyScale)
	{
		if (!Math::AllFinite(bodyScale) ||
			!Math::AllFinite(desc.m_center) ||
			!IsFinite(desc.m_rotation) ||
			!Math::AllFinite(desc.m_size) ||
			!std::isfinite(desc.m_radius) ||
			!std::isfinite(desc.m_height))
		{
			return {};
		}

		const glm::vec3 absoluteScale = glm::max(
			glm::abs(bodyScale),
			glm::vec3(c_minShapeExtent));

		switch (desc.m_type)
		{
		case Physics::ECollisionShapeType::Box:
		{
			const glm::vec3 halfExtent = glm::max(
				glm::abs(desc.m_size) * absoluteScale * 0.5f,
				glm::vec3(c_minShapeExtent));
			return new JPH::BoxShape(ToJolt(halfExtent));
		}
		case Physics::ECollisionShapeType::Sphere:
		{
			const float scale = std::max({
				absoluteScale.x,
				absoluteScale.y,
				absoluteScale.z });
			return new JPH::SphereShape(std::max(
				c_minShapeExtent,
				std::abs(desc.m_radius) * scale));
		}
		case Physics::ECollisionShapeType::Capsule:
		{
			const float radiusScale = std::max(
				absoluteScale.x,
				absoluteScale.z);
			const float radius = std::max(
				c_minShapeExtent,
				std::abs(desc.m_radius) * radiusScale);
			const float totalHeight = std::max(
				2.0f * radius,
				std::abs(desc.m_height) * absoluteScale.y);
			return new JPH::CapsuleShape(
				std::max(0.0f, totalHeight * 0.5f - radius),
				radius);
		}
		default:
			return {};
		}
	}

	JPH::ShapeRefC BuildShape(const Physics::RigidBodyDesc& desc)
	{
		if (desc.m_shapes.IsEmpty())
		{
			return {};
		}

		if (desc.m_shapes.Num() == 1)
		{
			const auto& shapeDesc = desc.m_shapes[0];
			JPH::ShapeRefC shape = BuildPrimitiveShape(shapeDesc, desc.m_scale);
			if (!shape)
			{
				return {};
			}

			const glm::vec3 center = shapeDesc.m_center * desc.m_scale;
			if (glm::dot(center, center) <= 0.0f &&
				glm::abs(glm::dot(shapeDesc.m_rotation, shapeDesc.m_rotation) - 1.0f) <= 0.0001f &&
				glm::abs(shapeDesc.m_rotation.w - 1.0f) <= 0.0001f)
			{
				return shape;
			}

			JPH::RotatedTranslatedShapeSettings transformed(
				ToJolt(center),
				ToJolt(shapeDesc.m_rotation),
				shape.GetPtr());
			auto result = transformed.Create();
			return result.HasError() ? JPH::ShapeRefC{} : result.Get();
		}

		JPH::StaticCompoundShapeSettings compound;
		for (const auto& shapeDesc : desc.m_shapes)
		{
			JPH::ShapeRefC shape = BuildPrimitiveShape(shapeDesc, desc.m_scale);
			if (!shape)
			{
				return {};
			}

			compound.AddShape(
				ToJolt(shapeDesc.m_center * desc.m_scale),
				ToJolt(shapeDesc.m_rotation),
				shape.GetPtr());
		}

		auto result = compound.Create();
		return result.HasError() ? JPH::ShapeRefC{} : result.Get();
	}
}

class Physics::PhysicsWorld::Impl final
{
public:
	class ContactListener final : public JPH::ContactListener
	{
	public:
		explicit ContactListener(Impl& owner) : m_owner(owner) {}

		JPH::ValidateResult OnContactValidate(
			const JPH::Body&,
			const JPH::Body&,
			JPH::RVec3Arg,
			const JPH::CollideShapeResult&) override
		{
			return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
		}

		void OnContactAdded(
			const JPH::Body& first,
			const JPH::Body& second,
			const JPH::ContactManifold& manifold,
			JPH::ContactSettings&) override
		{
			Push(
				Physics::EPhysicsContactType::Added,
				first,
				second,
				manifold);
		}

		void OnContactPersisted(
			const JPH::Body& first,
			const JPH::Body& second,
			const JPH::ContactManifold& manifold,
			JPH::ContactSettings&) override
		{
			Push(
				Physics::EPhysicsContactType::Persisted,
				first,
				second,
				manifold);
		}

		void OnContactRemoved(
			const JPH::SubShapeIDPair& pair) override
		{
			Physics::PhysicsContactEvent event{};
			event.m_type = Physics::EPhysicsContactType::Removed;
			if (m_owner.ResolveInstanceId(pair.GetBody1ID(), event.m_first) &&
				m_owner.ResolveInstanceId(pair.GetBody2ID(), event.m_second))
			{
				Canonicalize(event);
				m_owner.m_contactEvents.push(std::move(event));
			}
		}

	private:
		static void Canonicalize(
			Physics::PhysicsContactEvent& event)
		{
			if (event.m_second.ToString() < event.m_first.ToString())
			{
				std::swap(event.m_first, event.m_second);
				event.m_normal = -event.m_normal;
			}
		}

		void Push(
			Physics::EPhysicsContactType type,
			const JPH::Body& first,
			const JPH::Body& second,
			const JPH::ContactManifold& manifold)
		{
			Physics::PhysicsContactEvent event{};
			event.m_type = type;
			if (!m_owner.ResolveInstanceId(first.GetID(), event.m_first) ||
				!m_owner.ResolveInstanceId(second.GetID(), event.m_second))
			{
				return;
			}

			if (!manifold.mRelativeContactPointsOn1.empty())
			{
				event.m_position = FromJoltVector(
					manifold.GetWorldSpaceContactPointOn1(0));
			}
			event.m_normal = FromJoltVector(manifold.mWorldSpaceNormal);
			event.m_bSensor = first.IsSensor() || second.IsSensor();
			Canonicalize(event);
			m_owner.m_contactEvents.push(std::move(event));
		}

		Impl& m_owner;
	};

	explicit Impl(Tasks::Scheduler* scheduler) :
		m_jobSystem(scheduler),
		m_tempAllocator(32 * 1024 * 1024),
		m_contactListener(*this)
	{
		m_physicsSystem.Init(
			65536,
			0,
			65536,
			10240,
			m_broadPhaseLayerInterface,
			m_objectVsBroadPhaseLayerFilter,
			m_objectLayerPairFilter);
		m_physicsSystem.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
		m_physicsSystem.SetContactListener(&m_contactListener);
	}

	bool ResolveInstanceId(
		const JPH::BodyID& bodyId,
		InstanceId& outInstanceId) const
	{
		const InstanceId* instanceId = nullptr;
		if (!m_bodyInstances.Find(
				bodyId.GetIndexAndSequenceNumber(),
				instanceId))
		{
			return false;
		}

		outInstanceId = *instanceId;
		return true;
	}

	BroadPhaseLayerInterface m_broadPhaseLayerInterface{};
	ObjectVsBroadPhaseLayerFilter m_objectVsBroadPhaseLayerFilter{};
	ObjectLayerPairFilter m_objectLayerPairFilter{};
	JoltJobSystem m_jobSystem;
	JPH::TempAllocatorImpl m_tempAllocator;
	JPH::PhysicsSystem m_physicsSystem{};
	ContactListener m_contactListener;
	TMap<uint32_t, InstanceId> m_bodyInstances{};
	concurrency::concurrent_queue<PhysicsContactEvent> m_contactEvents{};
};

Physics::PhysicsWorld::PhysicsWorld() :
	m_pImpl(TUniquePtr<Impl>::Make(App::GetSubmodule<Tasks::Scheduler>()))
{}

Physics::PhysicsWorld::~PhysicsWorld()
{
	Clear();
}

bool Physics::PhysicsWorld::CreateBody(
	const RigidBodyDesc& desc,
	uint32_t& outBodyId)
{
	outBodyId = JPH::BodyID::cInvalidBodyID;
	if (!desc.m_instanceId ||
		!Math::AllFinite(desc.m_position) ||
		!IsFinite(desc.m_rotation) ||
		!Math::AllFinite(desc.m_scale) ||
		!Math::AllFinite(desc.m_linearVelocity) ||
		!Math::AllFinite(desc.m_angularVelocity) ||
		!std::isfinite(desc.m_mass) ||
		!std::isfinite(desc.m_friction) ||
		!std::isfinite(desc.m_restitution) ||
		!std::isfinite(desc.m_linearDamping) ||
		!std::isfinite(desc.m_angularDamping) ||
		!std::isfinite(desc.m_gravityFactor))
	{
		SAILOR_LOG_ERROR(
			"Cannot create physics body: invalid identity or transform data.");
		return false;
	}

	JPH::ShapeRefC shape = BuildShape(desc);
	if (!shape)
	{
		SAILOR_LOG_ERROR(
			"Cannot create physics body '%s': no valid collision shape.",
			desc.m_instanceId.ToString().c_str());
		return false;
	}

	JPH::EMotionType motionType = JPH::EMotionType::Dynamic;
	switch (desc.m_motionType)
	{
	case ERigidBodyMotionType::Static:
		motionType = JPH::EMotionType::Static;
		break;
	case ERigidBodyMotionType::Kinematic:
		motionType = JPH::EMotionType::Kinematic;
		break;
	case ERigidBodyMotionType::Dynamic:
		motionType = JPH::EMotionType::Dynamic;
		break;
	}

	JPH::BodyCreationSettings settings(
		shape,
		ToJoltPosition(desc.m_position),
		ToJolt(desc.m_rotation),
		motionType,
		MakeObjectLayer(desc.m_motionType, desc.m_collisionLayer));
	settings.mFriction = std::max(0.0f, desc.m_friction);
	settings.mRestitution = std::clamp(desc.m_restitution, 0.0f, 1.0f);
	settings.mLinearDamping = std::clamp(desc.m_linearDamping, 0.0f, 1.0f);
	settings.mAngularDamping = std::clamp(desc.m_angularDamping, 0.0f, 1.0f);
	settings.mGravityFactor = desc.m_gravityFactor;
	settings.mIsSensor = desc.m_bSensor;
	settings.mAllowSleeping = desc.m_bAllowSleeping;
	if (motionType == JPH::EMotionType::Dynamic)
	{
		settings.mOverrideMassProperties =
			JPH::EOverrideMassProperties::CalculateInertia;
		settings.mMassPropertiesOverride.mMass =
			std::max(c_minShapeExtent, desc.m_mass);
	}

	auto& bodyInterface = m_pImpl->m_physicsSystem.GetBodyInterface();
	const JPH::BodyID bodyId = bodyInterface.CreateAndAddBody(
		settings,
		motionType == JPH::EMotionType::Static
			? JPH::EActivation::DontActivate
			: JPH::EActivation::Activate);
	if (bodyId.IsInvalid())
	{
		return false;
	}

	outBodyId = bodyId.GetIndexAndSequenceNumber();
	m_pImpl->m_bodyInstances[outBodyId] = desc.m_instanceId;
	if (motionType != JPH::EMotionType::Static)
	{
		bodyInterface.SetLinearAndAngularVelocity(
			bodyId,
			ToJolt(desc.m_linearVelocity),
			ToJolt(desc.m_angularVelocity));
	}
	return true;
}

void Physics::PhysicsWorld::DestroyBody(uint32_t bodyId)
{
	if (bodyId == JPH::BodyID::cInvalidBodyID)
	{
		return;
	}
	if (!m_pImpl->m_bodyInstances.ContainsKey(bodyId))
	{
		return;
	}

	const JPH::BodyID id(bodyId);
	auto& bodyInterface = m_pImpl->m_physicsSystem.GetBodyInterface();
	if (bodyInterface.IsAdded(id))
	{
		bodyInterface.RemoveBody(id);
	}
	bodyInterface.DestroyBody(id);
	m_pImpl->m_bodyInstances.Remove(bodyId);
}

bool Physics::PhysicsWorld::SetBodyTransform(
	uint32_t bodyId,
	const glm::vec3& position,
	const glm::quat& rotation,
	bool bKinematic,
	float deltaTime)
{
	if (bodyId == JPH::BodyID::cInvalidBodyID)
	{
		return false;
	}
	if (!Math::AllFinite(position) || !IsFinite(rotation))
	{
		return false;
	}

	auto& bodyInterface = m_pImpl->m_physicsSystem.GetBodyInterface();
	const JPH::BodyID id(bodyId);
	if (!bodyInterface.IsAdded(id))
	{
		return false;
	}

	if (bKinematic && deltaTime > 0.0f)
	{
		bodyInterface.MoveKinematic(
			id,
			ToJoltPosition(position),
			ToJolt(rotation),
			deltaTime);
	}
	else
	{
		bodyInterface.SetPositionAndRotation(
			id,
			ToJoltPosition(position),
			ToJolt(rotation),
			JPH::EActivation::Activate);
	}
	return true;
}

bool Physics::PhysicsWorld::GetBodyPose(
	uint32_t bodyId,
	PhysicsBodyPose& outPose) const
{
	if (bodyId == JPH::BodyID::cInvalidBodyID)
	{
		return false;
	}

	const JPH::BodyID id(bodyId);
	const auto& bodyInterface = m_pImpl->m_physicsSystem.GetBodyInterface();
	if (!bodyInterface.IsAdded(id))
	{
		return false;
	}

	JPH::RVec3 position;
	JPH::Quat rotation;
	bodyInterface.GetPositionAndRotation(id, position, rotation);
	outPose.m_position = FromJoltVector(position);
	outPose.m_rotation = FromJoltQuat(rotation);
	outPose.m_linearVelocity = FromJoltVector(bodyInterface.GetLinearVelocity(id));
	outPose.m_angularVelocity = FromJoltVector(bodyInterface.GetAngularVelocity(id));
	outPose.m_bActive = bodyInterface.IsActive(id);
	return true;
}

bool Physics::PhysicsWorld::SetBodyVelocity(
	uint32_t bodyId,
	const glm::vec3& linearVelocity,
	const glm::vec3& angularVelocity)
{
	if (bodyId == JPH::BodyID::cInvalidBodyID)
	{
		return false;
	}
	if (!Math::AllFinite(linearVelocity) ||
		!Math::AllFinite(angularVelocity))
	{
		return false;
	}

	const JPH::BodyID id(bodyId);
	auto& bodyInterface = m_pImpl->m_physicsSystem.GetBodyInterface();
	if (!bodyInterface.IsAdded(id))
	{
		return false;
	}
	bodyInterface.SetLinearAndAngularVelocity(
		id,
		ToJolt(linearVelocity),
		ToJolt(angularVelocity));
	return true;
}

bool Physics::PhysicsWorld::Step(float deltaTime)
{
	if (!std::isfinite(deltaTime) || deltaTime <= 0.0f)
	{
		return false;
	}

	const JPH::EPhysicsUpdateError result = m_pImpl->m_physicsSystem.Update(
		deltaTime,
		1,
		&m_pImpl->m_tempAllocator,
		&m_pImpl->m_jobSystem);
	if (result != JPH::EPhysicsUpdateError::None)
	{
		SAILOR_LOG_ERROR(
			"Jolt physics update reported capacity error mask %u.",
			static_cast<uint32_t>(result));
		return false;
	}
	return true;
}

bool Physics::PhysicsWorld::Raycast(
	const glm::vec3& origin,
	const glm::vec3& direction,
	float distance,
	PhysicsRaycastHit& outHit,
	uint16_t collisionMask) const
{
	if (!Math::AllFinite(origin) || !Math::AllFinite(direction) ||
		!std::isfinite(distance) || collisionMask == 0)
	{
		return false;
	}

	const float directionLength = glm::length(direction);
	if (directionLength <= 0.000001f || distance <= 0.0f)
	{
		return false;
	}

	const JPH::RRayCast ray(
		ToJoltPosition(origin),
		ToJolt(direction / directionLength * distance));
	JPH::RayCastResult hit;
	const QueryObjectLayerFilter objectLayerFilter(collisionMask);
	if (!m_pImpl->m_physicsSystem.GetNarrowPhaseQuery().CastRay(
		ray,
		hit,
		{},
		objectLayerFilter))
	{
		return false;
	}

	if (!m_pImpl->ResolveInstanceId(hit.mBodyID, outHit.m_instanceId))
	{
		return false;
	}

	const JPH::RVec3 hitPosition = ray.GetPointOnRay(hit.mFraction);
	outHit.m_position = FromJoltVector(hitPosition);
	outHit.m_normal = glm::vec3(0.0f);
	outHit.m_fraction = hit.mFraction;

	JPH::BodyLockRead lock(
		m_pImpl->m_physicsSystem.GetBodyLockInterface(),
		hit.mBodyID);
	if (lock.SucceededAndIsInBroadPhase())
	{
		const JPH::Body& body = lock.GetBody();
		outHit.m_normal = FromJoltVector(
			body.GetWorldSpaceSurfaceNormal(
				hit.mSubShapeID2,
				hitPosition));
	}
	return true;
}

void Physics::PhysicsWorld::SetLayerCollisionEnabled(
	uint8_t firstLayer,
	uint8_t secondLayer,
	bool bEnabled)
{
	m_pImpl->m_objectLayerPairFilter.SetCollisionEnabled(
		firstLayer,
		secondLayer,
		bEnabled);
}

bool Physics::PhysicsWorld::IsLayerCollisionEnabled(
	uint8_t firstLayer,
	uint8_t secondLayer) const
{
	return m_pImpl->m_objectLayerPairFilter.IsCollisionEnabled(
		firstLayer,
		secondLayer);
}

void Physics::PhysicsWorld::DrainContactEvents(
	TVector<PhysicsContactEvent>& outEvents)
{
	PhysicsContactEvent event;
	while (m_pImpl->m_contactEvents.try_pop(event))
	{
		outEvents.Add(std::move(event));
	}

	outEvents.Sort([](
		const PhysicsContactEvent& lhs,
		const PhysicsContactEvent& rhs)
		{
			if (lhs.m_first.ToString() != rhs.m_first.ToString())
			{
				return lhs.m_first.ToString() < rhs.m_first.ToString();
			}
			if (lhs.m_second.ToString() != rhs.m_second.ToString())
			{
				return lhs.m_second.ToString() < rhs.m_second.ToString();
			}
			return lhs.m_type < rhs.m_type;
		});
}

void Physics::PhysicsWorld::Clear()
{
	TVector<uint32_t> bodyIds;
	bodyIds.Reserve(m_pImpl->m_bodyInstances.Num());
	for (const auto& body : m_pImpl->m_bodyInstances)
	{
		bodyIds.Add(body.m_first);
	}
	for (uint32_t bodyId : bodyIds)
	{
		DestroyBody(bodyId);
	}
	m_pImpl->m_bodyInstances.Clear();
	PhysicsContactEvent event;
	while (m_pImpl->m_contactEvents.try_pop(event)) {}
}
