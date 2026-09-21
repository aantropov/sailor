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
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
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
			!Math::AllFinite(desc.m_rotation) ||
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
			const float convexRadius = std::min({
				JPH::cDefaultConvexRadius,
				halfExtent.x, halfExtent.y, halfExtent.z });
			return new JPH::BoxShape(ToJolt(halfExtent), convexRadius);
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
		case Physics::ECollisionShapeType::TriangleMesh:
		{
			if (desc.m_vertices.IsEmpty() || desc.m_indices.Num() < 3u)
			{
				return {};
			}

			JPH::VertexList vertices;
			vertices.reserve(desc.m_vertices.Num());
			for (const glm::vec3& vertex : desc.m_vertices)
			{
				const glm::vec3 scaled = vertex * absoluteScale;
				vertices.emplace_back(scaled.x, scaled.y, scaled.z);
			}

			JPH::IndexedTriangleList triangles;
			triangles.reserve(desc.m_indices.Num() / 3u);
			for (size_t index = 0u; index + 2u < desc.m_indices.Num(); index += 3u)
			{
				const uint32_t i0 = desc.m_indices[index];
				const uint32_t i1 = desc.m_indices[index + 1u];
				const uint32_t i2 = desc.m_indices[index + 2u];
				if (i0 < desc.m_vertices.Num() && i1 < desc.m_vertices.Num() && i2 < desc.m_vertices.Num())
				{
					triangles.emplace_back(i0, i1, i2, 0u);
				}
			}
			if (triangles.empty())
			{
				return {};
			}

			JPH::MeshShapeSettings settings(std::move(vertices), std::move(triangles));
			settings.mBuildQuality = JPH::MeshShapeSettings::EBuildQuality::FavorRuntimePerformance;
			auto result = settings.Create();
			return result.HasError() ? JPH::ShapeRefC{} : result.Get();
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

	bool IsRigidBody(uint32_t bodyId) const
	{
		if (!m_bodyInstances.ContainsKey(bodyId))
		{
			return false;
		}

		JPH::BodyLockRead lock(
			m_physicsSystem.GetBodyLockInterface(),
			JPH::BodyID(bodyId));
		return lock.SucceededAndIsInBroadPhase() && lock.GetBody().IsRigidBody();
	}

	BroadPhaseLayerInterface m_broadPhaseLayerInterface{};
	ObjectVsBroadPhaseLayerFilter m_objectVsBroadPhaseLayerFilter{};
	ObjectLayerPairFilter m_objectLayerPairFilter{};
	JoltJobSystem m_jobSystem;
	JPH::TempAllocatorImpl m_tempAllocator;
	JPH::PhysicsSystem m_physicsSystem{};
	ContactListener m_contactListener;
	TMap<uint32_t, InstanceId> m_bodyInstances{};
	TMap<uint32_t, JPH::Ref<JPH::SoftBodySharedSettings>> m_softBodySettings{};
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
		!Math::AllFinite(desc.m_rotation) ||
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

bool Physics::PhysicsWorld::CreateSoftBody(
	const SoftBodyDesc& desc,
	uint32_t& outBodyId)
{
	outBodyId = JPH::BodyID::cInvalidBodyID;
	const size_t numVertices = desc.m_vertices.Num();
	const bool bSkinned = !desc.m_maxDistances.IsEmpty();
	if (!desc.m_instanceId || numVertices < 3 || numVertices > 65535 ||
		desc.m_indices.Num() < 3 || desc.m_indices.Num() % 3 != 0 ||
		desc.m_inverseMasses.Num() != numVertices ||
		(bSkinned && desc.m_maxDistances.Num() != numVertices) ||
		!Math::AllFinite(desc.m_position) || !Math::AllFinite(desc.m_rotation) ||
		desc.m_numIterations == 0 || desc.m_numIterations > 32 ||
		desc.m_collisionLayer >= c_numCollisionLayers)
	{
		return false;
	}

	for (const float value : { desc.m_edgeCompliance, desc.m_shearCompliance,
		desc.m_bendCompliance, desc.m_vertexRadius, desc.m_friction, desc.m_restitution,
		desc.m_linearDamping, desc.m_maxLinearVelocity })
	{
		if (!std::isfinite(value) || value < 0.0f)
		{
			return false;
		}
	}
	if (!std::isfinite(desc.m_gravityFactor) || desc.m_maxLinearVelocity == 0.0f ||
		desc.m_linearDamping > 1.0f || desc.m_restitution > 1.0f)
	{
		return false;
	}

	JPH::Ref<JPH::SoftBodySharedSettings> sharedSettings = new JPH::SoftBodySharedSettings();
	sharedSettings->mVertexRadius = desc.m_vertexRadius;
	for (size_t i = 0; i < numVertices; ++i)
	{
		const glm::vec3& position = desc.m_vertices[i];
		if (!Math::AllFinite(position) ||
			!std::isfinite(desc.m_inverseMasses[i]) || desc.m_inverseMasses[i] < 0.0f)
		{
			return false;
		}

		sharedSettings->mVertices.emplace_back(
			JPH::Float3(position.x, position.y, position.z),
			JPH::Float3(0.0f, 0.0f, 0.0f),
			desc.m_inverseMasses[i]);
		if (bSkinned)
		{
			const float maxDistance = desc.m_maxDistances[i];
			if (!std::isfinite(maxDistance) || maxDistance < 0.0f)
			{
				return false;
			}
			if (maxDistance == 0.0f)
			{
				sharedSettings->mVertices.back().mInvMass = 0.0f;
			}

			const auto vertexIndex = static_cast<uint32_t>(i);
			sharedSettings->mInvBindMatrices.emplace_back(
				vertexIndex,
				JPH::Mat44::sTranslation(-ToJolt(position)));
			JPH::SoftBodySharedSettings::Skinned constraint(vertexIndex, maxDistance, FLT_MAX, 0.0f);
			constraint.mWeights[0] = JPH::SoftBodySharedSettings::SkinWeight(vertexIndex, 1.0f);
			sharedSettings->mSkinnedConstraints.push_back(constraint);
		}
	}

	// Constraint generation requires a consistently wound manifold surface.
	TMap<uint64_t, int32_t> edgeDirections;
	TVector<bool> usedVertices;
	usedVertices.Resize(numVertices);
	for (size_t i = 0; i < desc.m_indices.Num(); i += 3)
	{
		const uint32_t a = desc.m_indices[i];
		const uint32_t b = desc.m_indices[i + 1];
		const uint32_t c = desc.m_indices[i + 2];
		if (a >= numVertices || b >= numVertices || c >= numVertices || a == b || b == c || a == c)
		{
			return false;
		}

		const glm::vec3 normal = glm::cross(
			desc.m_vertices[b] - desc.m_vertices[a],
			desc.m_vertices[c] - desc.m_vertices[a]);
		const float areaSquared = glm::dot(normal, normal);
		if (!std::isfinite(areaSquared) || areaSquared < 1e-12f)
		{
			return false;
		}

		const uint32_t triangle[] = { a, b, c, a };
		for (uint32_t edge = 0; edge < 3; ++edge)
		{
			const uint32_t from = triangle[edge];
			const uint32_t to = triangle[edge + 1];
			const uint64_t key = (static_cast<uint64_t>(std::min(from, to)) << 32) | std::max(from, to);
			const int32_t direction = from < to ? 1 : -1;
			int32_t* previousDirection = nullptr;
			if (edgeDirections.Find(key, previousDirection))
			{
				if (*previousDirection != -direction)
				{
					return false;
				}
				*previousDirection = 0;
			}
			else
			{
				edgeDirections.Insert(key, direction);
			}
			usedVertices[from] = true;
		}
		sharedSettings->AddFace({ a, b, c });
	}
	for (const bool bUsed : usedVertices)
	{
		if (!bUsed)
		{
			return false;
		}
	}

	const JPH::SoftBodySharedSettings::VertexAttributes attributes(
		desc.m_edgeCompliance, desc.m_shearCompliance, desc.m_bendCompliance);
	sharedSettings->CreateConstraints(&attributes, 1, JPH::SoftBodySharedSettings::EBendType::Dihedral);
	if (bSkinned)
	{
		sharedSettings->CalculateSkinnedConstraintNormals();
	}
	sharedSettings->Optimize();

	JPH::SoftBodyCreationSettings settings(
		sharedSettings,
		ToJoltPosition(desc.m_position),
		ToJolt(desc.m_rotation),
		MakeObjectLayer(ERigidBodyMotionType::Dynamic, desc.m_collisionLayer));
	settings.mMakeRotationIdentity = false;
	settings.mNumIterations = desc.m_numIterations;
	settings.mLinearDamping = desc.m_linearDamping;
	settings.mMaxLinearVelocity = desc.m_maxLinearVelocity;
	settings.mFriction = desc.m_friction;
	settings.mRestitution = desc.m_restitution;
	settings.mGravityFactor = desc.m_gravityFactor;
	settings.mAllowSleeping = desc.m_bAllowSleeping;

	const JPH::BodyID bodyId = m_pImpl->m_physicsSystem.GetBodyInterface().CreateAndAddSoftBody(
		settings,
		JPH::EActivation::Activate);
	if (bodyId.IsInvalid())
	{
		return false;
	}

	outBodyId = bodyId.GetIndexAndSequenceNumber();
	m_pImpl->m_bodyInstances[outBodyId] = desc.m_instanceId;
	m_pImpl->m_softBodySettings[outBodyId] = sharedSettings;
	if (bSkinned)
	{
		TVector<glm::vec3> targets;
		targets.Reserve(numVertices);
		const glm::quat rotation = SanitizeRotation(desc.m_rotation);
		for (const auto& vertex : desc.m_vertices)
		{
			targets.Add(desc.m_position + rotation * vertex);
		}
		if (!SetSoftBodyTargets(outBodyId, targets, 1.0f, true))
		{
			DestroyBody(outBodyId);
			outBodyId = JPH::BodyID::cInvalidBodyID;
			return false;
		}
	}
	return true;
}

bool Physics::PhysicsWorld::SetSoftBodyRestPose(
	uint32_t bodyId,
	const TVector<glm::vec3>& positions,
	bool bPreserveEdgeLengths)
{
	JPH::Ref<JPH::SoftBodySharedSettings>* sharedSettings = nullptr;
	if (!m_pImpl->m_softBodySettings.Find(bodyId, sharedSettings))
	{
		return false;
	}

	{
		JPH::BodyLockWrite lock(
			m_pImpl->m_physicsSystem.GetBodyLockInterface(),
			JPH::BodyID(bodyId));
		if (!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsSoftBody())
		{
			return false;
		}

		auto& settings = **sharedSettings;
		if (positions.Num() != settings.mVertices.size())
		{
			return false;
		}
		for (const auto& position : positions)
		{
			if (!Math::AllFinite(position))
			{
				return false;
			}
		}
		for (const auto& face : settings.mFaces)
		{
			const glm::vec3 normal = glm::cross(
				positions[face.mVertex[1]] - positions[face.mVertex[0]],
				positions[face.mVertex[2]] - positions[face.mVertex[0]]);
			const float areaSquared = glm::dot(normal, normal);
			if (!std::isfinite(areaSquared) || areaSquared < 1e-12f)
			{
				return false;
			}
		}
		for (const auto& edge : settings.mEdgeConstraints)
		{
			if (!std::isfinite(glm::length(positions[edge.mVertex[1]] - positions[edge.mVertex[0]])))
			{
				return false;
			}
		}

		// Jolt also uses these positions for skin binds; keep the original bind pose.
		auto bindVertices = settings.mVertices;
		for (size_t i = 0; i < positions.Num(); ++i)
		{
			settings.mVertices[i].mPosition = JPH::Float3(positions[i].x, positions[i].y, positions[i].z);
		}
		if (!bPreserveEdgeLengths)
		{
			settings.CalculateEdgeLengths();
		}
		settings.CalculateBendConstraintConstants();
		settings.mVertices.swap(bindVertices);
	}
	m_pImpl->m_physicsSystem.GetBodyInterface().ActivateBody(JPH::BodyID(bodyId));
	return true;
}

bool Physics::PhysicsWorld::SetSoftBodyTargets(
	uint32_t bodyId,
	const TVector<glm::vec3>& targets,
	float maxDistanceMultiplier,
	bool bReset)
{
	if (!m_pImpl->m_bodyInstances.ContainsKey(bodyId) ||
		!std::isfinite(maxDistanceMultiplier) || maxDistanceMultiplier < 0.0f)
	{
		return false;
	}
	for (const auto& target : targets)
	{
		if (!Math::AllFinite(target))
		{
			return false;
		}
	}

	{
		JPH::BodyLockWrite lock(
			m_pImpl->m_physicsSystem.GetBodyLockInterface(),
			JPH::BodyID(bodyId));
		if (!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsSoftBody())
		{
			return false;
		}

		auto& body = lock.GetBody();
		auto* motion = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
		if (targets.Num() != motion->GetVertices().size() || motion->GetSettings()->mSkinnedConstraints.empty())
		{
			return false;
		}

		const auto transform = body.GetCenterOfMassTransform();
		const auto inverseTransform = transform.InversedRotationTranslation();
		JPH::Array<JPH::Mat44> joints;
		joints.reserve(targets.Num());
		for (const auto& target : targets)
		{
			const JPH::Vec3 localPosition(inverseTransform * ToJoltPosition(target));
			if (!Math::AllFinite(FromJoltVector(localPosition)))
			{
				return false;
			}
			joints.push_back(JPH::Mat44::sTranslation(localPosition));
		}

		if ((motion->GetSkinnedMaxDistanceMultiplier() == 0.0f) != (maxDistanceMultiplier == 0.0f))
		{
			// Contacts run after skin constraints, so hard-skinned vertices must be kinematic.
			auto& vertices = motion->GetVertices();
			for (size_t i = 0; i < vertices.size(); ++i)
			{
				vertices[i].mInvMass = maxDistanceMultiplier == 0.0f
					? 0.0f
					: motion->GetSettings()->mVertices[i].mInvMass;
			}
			motion->CalculateMassAndInertia();
		}
		motion->SetSkinnedMaxDistanceMultiplier(maxDistanceMultiplier);
		motion->SkinVertices(
			transform,
			joints.data(),
			static_cast<JPH::uint>(joints.size()),
			bReset,
			m_pImpl->m_tempAllocator);
	}
	m_pImpl->m_physicsSystem.GetBodyInterface().ActivateBody(JPH::BodyID(bodyId));
	return true;
}

bool Physics::PhysicsWorld::ApplySoftBodyWind(
	uint32_t bodyId,
	const glm::vec3& velocity,
	float airDensity,
	float drag,
	float deltaTime)
{
	if (!m_pImpl->m_bodyInstances.ContainsKey(bodyId) || !Math::AllFinite(velocity) ||
		!std::isfinite(airDensity) || airDensity < 0.0f || !std::isfinite(drag) || drag < 0.0f ||
		!std::isfinite(deltaTime) || deltaTime <= 0.0f)
	{
		return false;
	}

	{
		JPH::BodyLockWrite lock(
			m_pImpl->m_physicsSystem.GetBodyLockInterface(),
			JPH::BodyID(bodyId));
		if (!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsSoftBody())
		{
			return false;
		}

		auto& body = lock.GetBody();
		auto* motion = static_cast<JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
		auto& vertices = motion->GetVertices();
		const JPH::Vec3 wind = body.GetRotation().Conjugated() * ToJolt(velocity);
		JPH::Array<JPH::Vec3> impulses(vertices.size(), JPH::Vec3::sZero());
		for (const auto& face : motion->GetFaces())
		{
			const auto& a = vertices[face.mVertex[0]];
			const auto& b = vertices[face.mVertex[1]];
			const auto& c = vertices[face.mVertex[2]];
			const JPH::Vec3 cross = (b.mPosition - a.mPosition).Cross(c.mPosition - a.mPosition);
			const float twiceArea = cross.Length();
			if (twiceArea < 1e-8f)
			{
				continue;
			}

			const JPH::Vec3 normal = cross / twiceArea;
			const float speed = (wind - (a.mVelocity + b.mVelocity + c.mVelocity) / 3.0f).Dot(normal);
			const JPH::Vec3 impulse = normal * (speed * std::abs(speed) * twiceArea * airDensity * drag * deltaTime / 12.0f);
			for (const auto index : face.mVertex)
			{
				impulses[index] += impulse;
			}
		}
		for (size_t i = 0; i < vertices.size(); ++i)
		{
			const auto& vertex = vertices[i];
			JPH::Vec3 change = impulses[i] * vertex.mInvMass;
			const float length = change.Length();
			const float relativeSpeed = (wind - vertex.mVelocity).Length();
			if (!std::isfinite(length) || !std::isfinite(relativeSpeed))
			{
				return false;
			}

			// Drag must not overshoot the air velocity at a large time step.
			if (length > relativeSpeed)
			{
				change *= relativeSpeed / length;
			}
			impulses[i] = change;
		}
		for (size_t i = 0; i < vertices.size(); ++i)
		{
			vertices[i].mVelocity += impulses[i];
		}
	}
	m_pImpl->m_physicsSystem.GetBodyInterface().ActivateBody(JPH::BodyID(bodyId));
	return true;
}

bool Physics::PhysicsWorld::GetSoftBodyVertices(
	uint32_t bodyId,
	TVector<SoftBodyVertex>& outVertices) const
{
	if (!m_pImpl->m_bodyInstances.ContainsKey(bodyId))
	{
		return false;
	}

	JPH::BodyLockRead lock(
		m_pImpl->m_physicsSystem.GetBodyLockInterface(),
		JPH::BodyID(bodyId));
	if (!lock.SucceededAndIsInBroadPhase() || !lock.GetBody().IsSoftBody())
	{
		return false;
	}

	const auto& body = lock.GetBody();
	const auto* motion = static_cast<const JPH::SoftBodyMotionProperties*>(body.GetMotionProperties());
	const auto& vertices = motion->GetVertices();
	outVertices.Resize(vertices.size());
	const auto transform = body.GetCenterOfMassTransform();
	for (size_t i = 0; i < vertices.size(); ++i)
	{
		outVertices[i].m_position = FromJoltVector(transform * vertices[i].mPosition);
		outVertices[i].m_velocity = FromJoltVector(body.GetRotation() * vertices[i].mVelocity);
		outVertices[i].m_normal = glm::vec3(0.0f);
	}
	for (const auto& face : motion->GetFaces())
	{
		// Local differences retain small faces even far from the world origin.
		const glm::vec3 normal = FromJoltVector(
			(vertices[face.mVertex[1]].mPosition - vertices[face.mVertex[0]].mPosition).Cross(
			vertices[face.mVertex[2]].mPosition - vertices[face.mVertex[0]].mPosition));
		for (const auto index : face.mVertex)
		{
			outVertices[index].m_normal += normal;
		}
	}
	for (auto& vertex : outVertices)
	{
		const glm::vec3 normal = vertex.m_normal;
		const float scale = std::max({ std::abs(normal.x), std::abs(normal.y), std::abs(normal.z) });
		// Scale area-weighted normals before normalization to retain small faces.
		const glm::vec3 localNormal = scale > 0.0f && Math::AllFinite(normal)
			? glm::normalize(normal / scale)
			: glm::vec3(0.0f, 0.0f, 1.0f);
		vertex.m_normal = FromJoltVector(body.GetRotation() * ToJolt(localNormal));
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
	m_pImpl->m_softBodySettings.Remove(bodyId);
}

bool Physics::PhysicsWorld::SetBodyTransform(
	uint32_t bodyId,
	const glm::vec3& position,
	const glm::quat& rotation,
	bool bKinematic,
	float deltaTime)
{
	if (!m_pImpl->IsRigidBody(bodyId))
	{
		return false;
	}
	if (!Math::AllFinite(position) || !Math::AllFinite(rotation))
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
	if (!m_pImpl->m_bodyInstances.ContainsKey(bodyId))
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
	if (!m_pImpl->IsRigidBody(bodyId))
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

bool Physics::PhysicsWorld::AddForceAtPosition(
	uint32_t bodyId,
	const glm::vec3& force,
	const glm::vec3& position)
{
	if (!m_pImpl->IsRigidBody(bodyId) ||
		!Math::AllFinite(force) || !Math::AllFinite(position))
	{
		return false;
	}

	const JPH::BodyID id(bodyId);
	auto& bodyInterface = m_pImpl->m_physicsSystem.GetBodyInterface();
	if (!bodyInterface.IsAdded(id))
	{
		return false;
	}

	bodyInterface.AddForce(
		id,
		ToJolt(force),
		ToJoltPosition(position),
		JPH::EActivation::Activate);
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
