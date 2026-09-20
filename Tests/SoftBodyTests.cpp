#include "Physics/JoltRuntime.h"
#include "Physics/PhysicsWorld.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace Sailor;

namespace
{
	constexpr float c_fixedDeltaTime = 1.0f / 60.0f;

	void Require(bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	Physics::SoftBodyDesc MakeSheet(bool bSkinned = true)
	{
		Physics::SoftBodyDesc desc;
		desc.m_instanceId = InstanceId::GenerateNewInstanceId();
		desc.m_position = { 3.0f, 4.0f, -2.0f };
		desc.m_gravityFactor = 0.0f;
		for (uint32_t row = 0; row < 9; ++row)
		{
			for (uint32_t column = 0; column < 9; ++column)
			{
				desc.m_vertices.Add({
					static_cast<float>(column) * 0.25f,
					-static_cast<float>(row) * 0.25f,
					0.0f });
				desc.m_inverseMasses.Add(row == 0 ? 0.0f : 16.0f);
				if (bSkinned)
				{
					desc.m_maxDistances.Add(row == 0 ? 0.0f : 0.6f);
				}
				if (row < 8 && column < 8)
				{
					const uint32_t index = row * 9 + column;
					desc.m_indices.AddRange({
						index, index + 9, index + 1,
						index + 1, index + 9, index + 10 });
				}
			}
		}
		return desc;
	}

	TVector<glm::vec3> MakeTargets(const Physics::SoftBodyDesc& desc)
	{
		TVector<glm::vec3> targets;
		for (const auto& vertex : desc.m_vertices)
		{
			targets.Add(desc.m_position + desc.m_rotation * vertex);
		}
		return targets;
	}

	void NormalScaleAndTransform()
	{
		Physics::PhysicsWorld world;
		const glm::quat rotation = glm::angleAxis(0.8f, glm::normalize(glm::vec3(1.0f, 2.0f, 3.0f)));
		for (const float scale : { 0.005f, 0.01f, 1.0f })
		{
			for (const float offset : { 0.0f, 100000.0f })
			{
				Physics::SoftBodyDesc desc;
				desc.m_instanceId = InstanceId::GenerateNewInstanceId();
				desc.m_position = { offset, 0.0f, -offset };
				desc.m_rotation = rotation;
				desc.m_vertices.AddRange({
					{ 0.0f, 0.0f, 0.0f }, { scale, 0.0f, 0.0f }, { 0.0f, scale, scale } });
				desc.m_inverseMasses.AddRange({ 1.0f, 1.0f, 1.0f });
				desc.m_indices.AddRange({ 0, 1, 2 });
				uint32_t bodyId = ~0u;
				Require(world.CreateSoftBody(desc, bodyId), "Cannot create the small-triangle normal fixture");

				TVector<Physics::SoftBodyVertex> vertices;
				Require(world.GetSoftBodyVertices(bodyId, vertices) && vertices.Num() == 3,
					"Normal fixture lost a vertex");
				const glm::vec3 expected = rotation * glm::normalize(glm::vec3(0.0f, -1.0f, 1.0f));
				for (const auto& vertex : vertices)
				{
					Require(glm::dot(vertex.m_normal, expected) > 0.9999f &&
						std::abs(glm::length(vertex.m_normal) - 1.0f) < 0.001f,
						"Small or translated soft-body faces lose their rotated geometric normal");
				}
				world.DestroyBody(bodyId);
			}
		}

		auto desc = MakeSheet();
		desc.m_rotation = rotation;
		uint32_t bodyId = ~0u;
		Require(world.CreateSoftBody(desc, bodyId), "Cannot create the collapsed-normal fixture");
		auto targets = MakeTargets(desc);
		for (auto& target : targets)
		{
			target = desc.m_position;
		}
		Require(world.SetSoftBodyTargets(bodyId, targets, 0.0f, true), "Cannot collapse the normal fixture");
		TVector<Physics::SoftBodyVertex> vertices;
		Require(world.GetSoftBodyVertices(bodyId, vertices), "Collapsed surface has no normal readback");
		for (const auto& vertex : vertices)
		{
			Require(glm::dot(vertex.m_normal, rotation * glm::vec3(0.0f, 0.0f, 1.0f)) > 0.9999f,
				"Degenerate soft-body normals ignore the body's orientation");
		}
	}

	void WindAndAttachments()
	{
		Physics::PhysicsWorld world;
		auto desc = MakeSheet();
		desc.m_rotation = glm::angleAxis(0.6f, glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::vec3 wind = desc.m_rotation * glm::vec3(0.0f, 0.0f, 8.0f);
		auto targets = MakeTargets(desc);
		uint32_t bodyId = ~0u;
		Require(world.CreateSoftBody(desc, bodyId), "Cannot create skinned cloth");
		TVector<Physics::SoftBodyVertex> vertices;
		for (uint32_t step = 0; step < 180; ++step)
		{
			Require(world.SetSoftBodyTargets(bodyId, targets), "Cannot update cloth targets");
			Require(world.ApplySoftBodyWind(bodyId, wind, 1.225f, 1.2f, c_fixedDeltaTime) &&
				world.Step(c_fixedDeltaTime), "Cloth wind step failed");
		}
		Require(world.GetSoftBodyVertices(bodyId, vertices) && vertices.Num() == desc.m_vertices.Num(),
			"Readback reordered or lost particles");
		for (size_t i = 0; i < 9; ++i)
		{
			Require(glm::length(vertices[i].m_position - targets[i]) < 0.001f, "Wind detached a pinned vertex");
		}
		Require(glm::dot(vertices[40].m_position - targets[40], glm::normalize(wind)) > 0.02f,
			"Crosswind does not move the fabric interior");
		for (const auto& vertex : vertices)
		{
			Require(std::isfinite(glm::length(vertex.m_velocity)) &&
				std::abs(glm::length(vertex.m_normal) - 1.0f) < 0.001f,
				"Cloth readback contains invalid normals or velocities");
		}

		for (uint32_t step = 0; step < 60; ++step)
		{
			for (auto& target : targets)
			{
				target += glm::vec3(0.01f, 0.002f, -0.003f);
			}
			Require(world.SetSoftBodyTargets(bodyId, targets) && world.Step(c_fixedDeltaTime),
				"Moving skin attachment failed");
		}
		Require(world.GetSoftBodyVertices(bodyId, vertices), "Cannot read the moved attachments");
		for (size_t i = 0; i < 9; ++i)
		{
			Require(glm::length(vertices[i].m_position - targets[i]) < 0.002f,
				"Pinned points lag a moving attachment");
		}

		for (auto& target : targets)
		{
			target += glm::vec3(80.0f, 10.0f, -40.0f);
		}
		Require(world.SetSoftBodyTargets(bodyId, targets, 0.0f, true), "Cloth teleport failed");
		Require(world.GetSoftBodyVertices(bodyId, vertices), "Cannot read the teleported cloth");
		for (size_t i = 0; i < vertices.Num(); ++i)
		{
			Require(glm::length(vertices[i].m_position - targets[i]) < 0.001f &&
				glm::length(vertices[i].m_velocity) < 0.001f,
				"Reset left a stretched cloth or explosive velocity");
		}
		Require(world.Step(c_fixedDeltaTime), "Step after cloth teleport failed");
		Require(world.GetSoftBodyVertices(bodyId, vertices), "Cannot read hard-skinned cloth");
		for (size_t i = 0; i < vertices.Num(); ++i)
		{
			Require(glm::length(vertices[i].m_position - targets[i]) < 0.002f,
				"Hard skin did not hold its target");
		}

		Require(world.SetSoftBodyTargets(bodyId, targets), "Cannot release hard-skinned cloth");
		Require(world.ApplySoftBodyWind(bodyId, wind, 1.225f, 1.2f, c_fixedDeltaTime),
			"Wind failed after releasing hard skin");
		Require(world.GetSoftBodyVertices(bodyId, vertices), "Cannot read released cloth");
		Require(glm::dot(vertices[40].m_velocity, wind) > 0.0f,
			"Releasing hard skin did not restore the dynamic vertex masses");
		for (size_t i = 0; i < 9; ++i)
		{
			Require(glm::length(vertices[i].m_velocity) < 0.001f,
				"Releasing hard skin also released a pinned vertex");
		}

		world.DestroyBody(bodyId);
		Require(!world.GetSoftBodyVertices(bodyId, vertices) && !world.SetSoftBodyTargets(bodyId, targets),
			"Destroyed cloth still resolves");
	}

	float DropSheet(bool bCollisionEnabled)
	{
		Physics::PhysicsWorld world;
		Physics::RigidBodyDesc floor;
		floor.m_instanceId = InstanceId::GenerateNewInstanceId();
		floor.m_motionType = Physics::ERigidBodyMotionType::Static;
		floor.m_position = { 0.0f, -0.5f, 0.0f };
		Physics::CollisionShapeDesc box;
		box.m_size = { 20.0f, 1.0f, 20.0f };
		floor.m_shapes.Add(box);
		uint32_t floorId = ~0u;
		uint32_t clothId = ~0u;
		Require(world.CreateBody(floor, floorId), "Cannot create cloth collider");

		auto desc = MakeSheet(false);
		desc.m_position = { -1.0f, 3.0f, -1.0f };
		desc.m_rotation = glm::angleAxis(-1.5707963f, glm::vec3(1.0f, 0.0f, 0.0f));
		desc.m_gravityFactor = 1.0f;
		desc.m_collisionLayer = 1;
		for (auto& inverseMass : desc.m_inverseMasses)
		{
			inverseMass = 16.0f;
		}
		world.SetLayerCollisionEnabled(0, 1, bCollisionEnabled);
		Require(world.CreateSoftBody(desc, clothId), "Cannot create falling sheet");
		for (uint32_t step = 0; step < 180; ++step)
		{
			Require(world.Step(c_fixedDeltaTime), "Falling cloth step failed");
		}
		TVector<Physics::SoftBodyVertex> vertices;
		Require(world.GetSoftBodyVertices(clothId, vertices), "Cannot read falling sheet");
		const float height = vertices[40].m_position.y;
		world.Clear();
		Require(!world.GetSoftBodyVertices(clothId, vertices), "World clear leaked a soft body");
		return height;
	}

	void RigidContactsAndLayers()
	{
		const float height = DropSheet(true);
		Require(height > -0.02f && height < 0.08f,
			"Cloth passes through or hovers above a rigid collider");
		Require(DropSheet(false) < -10.0f, "Cloth ignores the world collision mask");
	}

	void RestPose()
	{
		Physics::PhysicsWorld world;
		const auto desc = MakeSheet();
		uint32_t bodyId = ~0u;
		uint32_t otherId = ~0u;
		Require(world.CreateSoftBody(desc, bodyId) && world.CreateSoftBody(desc, otherId),
			"Cannot create rest-pose cloth");
		Require(world.ApplySoftBodyWind(bodyId, { 0.0f, 0.0f, 3.0f }, 1.225f, 0.8f, c_fixedDeltaTime),
			"Cannot move refit fixture");
		TVector<Physics::SoftBodyVertex> before;
		TVector<Physics::SoftBodyVertex> after;
		Require(world.GetSoftBodyVertices(bodyId, before), "Cannot read cloth before refit");

		auto restPositions = desc.m_vertices;
		for (auto& vertex : restPositions)
		{
			vertex.y *= 0.5f;
		}
		Require(world.SetSoftBodyRestPose(bodyId, restPositions), "Cannot refit cloth constraints");
		Require(world.GetSoftBodyVertices(bodyId, after), "Cannot read cloth after refit");
		for (size_t i = 0; i < before.Num(); ++i)
		{
			Require(before[i].m_position == after[i].m_position && before[i].m_velocity == after[i].m_velocity,
				"Rest refit teleports or resets moving particles");
		}

		auto targets = MakeTargets(desc);
		for (size_t i = 0; i < restPositions.Num(); ++i)
		{
			targets[i] = desc.m_position + restPositions[i];
		}
		Require(world.SetSoftBodyTargets(bodyId, targets, 1.0f, true), "Cannot gather refitted cloth");
		for (uint32_t step = 0; step < 120; ++step)
		{
			Require(world.Step(c_fixedDeltaTime), "Refitted cloth step failed");
		}
		Require(world.GetSoftBodyVertices(bodyId, after), "Cannot read gathered cloth");
		for (size_t i = 0; i < restPositions.Num(); ++i)
		{
			Require(glm::length(after[i].m_position - targets[i]) < 0.002f,
				"Old spring lengths buckle a refitted sheet");
		}
		Require(world.GetSoftBodyVertices(otherId, after), "Cannot read independent cloth");
		for (size_t i = 0; i < restPositions.Num(); ++i)
		{
			Require(glm::length(after[i].m_position - desc.m_position - desc.m_vertices[i]) < 0.002f,
				"Refitting one cloth changes another body's shared constraints");
		}

		Require(world.SetSoftBodyRestPose(bodyId, desc.m_vertices) &&
			world.SetSoftBodyTargets(bodyId, MakeTargets(desc), 1.0f, true),
			"Cannot restore the deployed rest shape and original skin binds");
		Require(world.Step(c_fixedDeltaTime) && world.GetSoftBodyVertices(bodyId, after),
			"Restored cloth cannot simulate");
		for (size_t i = 0; i < restPositions.Num(); ++i)
		{
			Require(glm::length(after[i].m_position - desc.m_position - desc.m_vertices[i]) < 0.002f,
				"Rest refit corrupted the skin bind");
		}
		world.DestroyBody(bodyId);
		Require(!world.SetSoftBodyRestPose(bodyId, restPositions), "Destroyed cloth accepts a rest refit");
		world.Clear();
		Require(!world.SetSoftBodyRestPose(otherId, restPositions), "Cleared world retains rest constraints");
	}

	void InextensibleRestPose()
	{
		Physics::PhysicsWorld world;
		auto desc = MakeSheet(false);
		desc.m_gravityFactor = 1.0f;
		desc.m_edgeCompliance = 0.0f;
		desc.m_shearCompliance = 0.0f;
		desc.m_numIterations = 12;
		uint32_t bodyId = ~0u;
		Require(world.CreateSoftBody(desc, bodyId), "Cannot create inextensible cloth");
		auto restPositions = desc.m_vertices;
		for (auto& vertex : restPositions)
		{
			vertex.y *= 0.5f;
		}
		Require(world.SetSoftBodyRestPose(bodyId, restPositions, true),
			"Cannot guide bends without resizing material");
		for (uint32_t step = 0; step < 120; ++step)
		{
			Require(world.Step(c_fixedDeltaTime), "Inextensible cloth step failed");
		}
		TVector<Physics::SoftBodyVertex> vertices;
		Require(world.GetSoftBodyVertices(bodyId, vertices), "Cannot read inextensible cloth");
		for (size_t column = 0; column < 9; ++column)
		{
			float length = 0.0f;
			for (size_t row = 1; row < 9; ++row)
			{
				length += glm::length(vertices[row * 9 + column].m_position -
					vertices[(row - 1) * 9 + column].m_position);
			}
			Require(std::abs(length - 2.0f) < 0.01f, "Bend-only rest pose resizes the cloth's material");
		}
	}

	void InvalidInputs()
	{
		Physics::PhysicsWorld world;
		const auto requireRejected = [&](const Physics::SoftBodyDesc& desc)
		{
			uint32_t bodyId = 0;
			Require(!world.CreateSoftBody(desc, bodyId) && bodyId == ~0u, "Invalid cloth accepted");
		};

		auto desc = MakeSheet();
		desc.m_indices[0] = 1000;
		requireRejected(desc);
		desc = MakeSheet();
		desc.m_indices[0] = desc.m_indices[1];
		requireRejected(desc);
		desc = MakeSheet();
		desc.m_indices.AddRange({ 0, 9, 1 });
		requireRejected(desc);
		desc = MakeSheet();
		desc.m_vertices[4].x = std::numeric_limits<float>::quiet_NaN();
		requireRejected(desc);
		desc = MakeSheet();
		for (auto& vertex : desc.m_vertices)
		{
			vertex *= 1e12f;
		}
		requireRejected(desc);
		desc = MakeSheet();
		desc.m_inverseMasses[8] = -1.0f;
		requireRejected(desc);
		desc = MakeSheet();
		desc.m_maxDistances.RemoveLast();
		requireRejected(desc);
		desc = MakeSheet();
		desc.m_numIterations = 0;
		requireRejected(desc);

		desc = MakeSheet();
		uint32_t bodyId = ~0u;
		Require(world.CreateSoftBody(desc, bodyId), "Valid cloth rejected after invalid input");
		auto targets = MakeTargets(desc);
		targets[7].z = std::numeric_limits<float>::infinity();
		Require(!world.SetSoftBodyTargets(bodyId, targets) &&
			!world.ApplySoftBodyWind(bodyId, {}, 1.0f, 1.0f, 0.0f), "Invalid cloth update accepted");
		auto restPositions = desc.m_vertices;
		restPositions.RemoveLast();
		Require(!world.SetSoftBodyRestPose(bodyId, restPositions), "Rest refit accepts a changed vertex count");
		restPositions = desc.m_vertices;
		restPositions[10] = restPositions[9];
		Require(!world.SetSoftBodyRestPose(bodyId, restPositions), "Rest refit accepts a degenerate surface");
		restPositions = desc.m_vertices;
		restPositions[10].x = std::numeric_limits<float>::infinity();
		Require(!world.SetSoftBodyRestPose(bodyId, restPositions), "Rest refit accepts a nonfinite surface");
		Require(world.Step(c_fixedDeltaTime), "Rejected update corrupted the soft body");
	}

	void WindOverflow()
	{
		Physics::PhysicsWorld world;
		const auto desc = MakeSheet();
		uint32_t bodyId = ~0u;
		Require(world.CreateSoftBody(desc, bodyId), "Cannot create wind-overflow fixture");
		TVector<Physics::SoftBodyVertex> before;
		TVector<Physics::SoftBodyVertex> after;
		Require(world.GetSoftBodyVertices(bodyId, before), "Cannot read cloth before wind overflow");
		Require(!world.ApplySoftBodyWind(bodyId, { 0.0f, 0.0f, 1e20f }, 1.225f, 1.2f, c_fixedDeltaTime),
			"Overflowing wind impulse accepted");
		Require(world.GetSoftBodyVertices(bodyId, after), "Cannot read cloth after rejected wind");
		for (size_t i = 0; i < before.Num(); ++i)
		{
			Require(before[i].m_position == after[i].m_position && before[i].m_velocity == after[i].m_velocity,
				"Rejected wind changed particles");
		}
		Require(world.ApplySoftBodyWind(bodyId, { 0.0f, 0.0f, 8.0f }, 1.225f, 1.2f, c_fixedDeltaTime) &&
			world.Step(c_fixedDeltaTime), "Rejected wind corrupted the simulation");
	}

	void BodyTypesAndLifetime()
	{
		Physics::PhysicsWorld world;
		const auto desc = MakeSheet();
		const auto targets = MakeTargets(desc);
		TVector<Physics::SoftBodyVertex> vertices;
		Physics::PhysicsBodyPose pose;
		const glm::vec3 velocity(0.0f, 0.0f, 1.0f);
		const auto requireMissing = [&](uint32_t bodyId)
		{
			Require(!world.GetSoftBodyVertices(bodyId, vertices) &&
				!world.SetSoftBodyTargets(bodyId, targets) &&
				!world.SetSoftBodyRestPose(bodyId, desc.m_vertices) &&
				!world.ApplySoftBodyWind(bodyId, velocity, 1.225f, 1.2f, c_fixedDeltaTime) &&
				!world.GetBodyPose(bodyId, pose) &&
				!world.SetBodyTransform(bodyId, desc.m_position, desc.m_rotation, false, c_fixedDeltaTime) &&
				!world.SetBodyVelocity(bodyId, velocity, {}) &&
				!world.AddForceAtPosition(bodyId, velocity, desc.m_position),
				"Unregistered body ID was accepted");
			world.DestroyBody(bodyId);
		};
		requireMissing(~0u);
		requireMissing(0x007fffffu);

		Physics::RigidBodyDesc rigidDesc;
		rigidDesc.m_instanceId = InstanceId::GenerateNewInstanceId();
		rigidDesc.m_shapes.Add(Physics::CollisionShapeDesc{});
		uint32_t rigidId = ~0u;
		uint32_t clothId = ~0u;
		Require(world.CreateBody(rigidDesc, rigidId) && world.CreateSoftBody(desc, clothId),
			"Cannot create body-type fixtures");
		Require(!world.GetSoftBodyVertices(rigidId, vertices) &&
			!world.SetSoftBodyTargets(rigidId, targets) &&
			!world.SetSoftBodyRestPose(rigidId, desc.m_vertices) &&
			!world.ApplySoftBodyWind(rigidId, velocity, 1.225f, 1.2f, c_fixedDeltaTime),
			"Soft-body methods accepted a rigid body");
		Require(!world.SetBodyTransform(clothId, desc.m_position, desc.m_rotation, false, c_fixedDeltaTime) &&
			!world.SetBodyVelocity(clothId, velocity, {}) &&
			!world.AddForceAtPosition(clothId, velocity, desc.m_position),
			"Rigid-body methods accepted a soft body");
		Require(world.SetBodyVelocity(rigidId, velocity, {}) && world.GetBodyPose(rigidId, pose) &&
			pose.m_linearVelocity == velocity, "Soft-body guards broke rigid-body velocity updates");

		world.DestroyBody(clothId);
		requireMissing(clothId);
		uint32_t replacementId = ~0u;
		Require(world.CreateSoftBody(desc, replacementId) && replacementId != clothId,
			"Recreated cloth retained the destroyed handle");
		requireMissing(clothId);
		Require(world.SetSoftBodyTargets(replacementId, targets) && world.Step(c_fixedDeltaTime),
			"A stale handle damaged the replacement cloth");
		world.Clear();
		requireMissing(replacementId);
		requireMissing(rigidId);
	}
}

int main()
{
	Physics::JoltRuntime runtime;
	try
	{
		NormalScaleAndTransform();
		WindAndAttachments();
		RigidContactsAndLayers();
		RestPose();
		InextensibleRestPose();
		InvalidInputs();
		WindOverflow();
		BodyTypesAndLifetime();
		std::cout << "Soft-body simulation, attachments, rest poses, collisions and lifetime tests passed.\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
