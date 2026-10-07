#include "Sailor.h"
#include "ModelImporterTestAccess.h"
#include "TextureCommandFixtures.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/ModelImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include "Components/LandscapeComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "ECS/CameraECS.h"
#include "ECS/LandscapeECS.h"
#include "ECS/PhysicsECS.h"
#include "ECS/StaticMeshRendererECS.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "Engine/World.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/GraphicsDriver.h"
#include "RHI/Mesh.h"
#include "RHI/VertexDescription.h"
#include "Settings/GraphicsSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>

using namespace Sailor;

namespace Sailor::Tests
{
	void RunLandscapeCommandTests(const std::filesystem::path& workspace);
	void RequireMainMeshUploadRefusal(const std::function<void()>& record, VkResult error, uint32_t precedingUploads);
	void RequirePendingMeshUpload(const std::function<void()>& load, const std::function<void()>& checkPending);
	void RequireNoTransferSubmission(const std::function<void()>& record);
	TVector<uint8_t> ReadMaterialGpuBytes(RHI::RHIShaderBindingSetPtr bindings);
}

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	void Drain()
	{
		App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
		Require(GraphicsDriver::Vulkan::VulkanApi::GetInstance()->GetMainDevice()->WaitIdle() == VK_SUCCESS,
			"terrain fixture uploads must complete");
		RHI::Renderer::GetDriver()->TrackResources_ThreadSafe();
	}

	class LandscapeWorld final : public World
	{
	public:
		LandscapeWorld(const MaterialPtr& material) : World("Landscape upload", 0, CreateEcs())
		{
			GetECS<LandscapeECS>()->BeginPlay();
			m_owner = Instantiate("Terrain");
			m_owner->SetMobilityType(EMobilityType::Static);
			m_component = m_owner->AddComponent<LandscapeComponent>();
			auto& data = Data();
			data.SetSettings(2, 1, 8, 4, 0, 0.1f, 1337, 1);
			data.SetLodSettings({ 8, 16 }, 0);
			data.SetMaterial(material);
			// Isolate terrain transfers from the material's own initialization upload.
			data.m_runtimeMaterial = CreateMaterialInstance(material);
			data.m_cachedSourceMaterialContentRevision = material->GetContentRevision();
			data.m_cachedSourceMaterialRenderMetadataRevision = material->GetRenderMetadataRevision();
			Drain();
			Require(data.m_runtimeMaterial->IsReady(), "the terrain fixture material must be ready before refusal injection");
		}

		~LandscapeWorld() override { Clear(); }
		using World::DestroyPendingGameObjects;

		LandscapeData& Data() { return GetECS<LandscapeECS>()->GetComponentData(m_component->GetComponentIndex()); }
		GameObjectPtr Owner() { return m_owner; }
		void Regenerate() { m_component->SetRegenerate(true); }

		MaterialPtr CreateMaterialInstance(const MaterialPtr& source)
		{
			auto& driver = RHI::Renderer::GetDriver();
			m_commandList = driver->CreateCommandList(false, RHI::ECommandListQueue::Transfer);
			RHI::Renderer::GetDriverCommands()->BeginCommandList(m_commandList, true);
			auto material = Material::CreateInstance(this, source);
			RHI::Renderer::GetDriverCommands()->EndCommandList(m_commandList);
			Require(driver->SubmitCommandList(m_commandList, RHI::RHIFencePtr::Make()),
				"the fixture material transfer must be accepted");
			m_commandList.Clear();
			return material;
		}

		GameObjectPtr AddCamera()
		{
			auto owner = Instantiate("Grass camera");
			owner->GetTransformComponent().SetPosition(glm::vec3(0, 8, 12));
			owner->GetTransformComponent().SetRotation(glm::quatLookAtRH(glm::normalize(glm::vec3(0, -8, -12)), glm::vec3(0, 1, 0)));
			auto camera = owner->AddComponent<CameraComponent>();
			camera->SetAspect(1);
			camera->SetZFar(100);
			return owner;
		}

		void Step()
		{
			++m_currentFrame;
			GetECS<TransformECS>()->Tick(0);
			GetECS<TransformECS>()->PostTick();
			GetECS<CameraECS>()->Tick(0);
			GetECS<LandscapeECS>()->Tick(0);
		}

		void StepWithMaterialCommands(bool bDrain = true)
		{
			auto& driver = RHI::Renderer::GetDriver();
			m_commandList = driver->CreateCommandList(false, RHI::ECommandListQueue::Transfer);
			RHI::Renderer::GetDriverCommands()->BeginCommandList(m_commandList, true);
			Step();
			RHI::Renderer::GetDriverCommands()->EndCommandList(m_commandList);
			Require(driver->SubmitCommandList(m_commandList, RHI::RHIFencePtr::Make()),
				"landscape material commands must be accepted");
			m_commandList.Clear();
			if (bDrain) Drain();
		}

		RHI::RHISpatialSceneVersionPtr Scene()
		{
			auto view = RHI::RHISceneViewPtr::Make();
			GetECS<LandscapeECS>()->AppendSceneView(view);
			Require(view->m_sceneVersions.Num() == 1, "the terrain world must publish one spatial scene");
			return view->m_sceneVersions[0];
		}

		void CheckHit(float x, float expectedHeight)
		{
			Physics::PhysicsRaycastHit hit;
			Require(GetECS<PhysicsECS>()->Raycast({ x, 20, 0 }, { 0, -1, 0 }, 40, hit) &&
				hit.m_instanceId == m_owner->GetInstanceId() && std::abs(hit.m_position.y - expectedHeight) < 0.0001f,
				"the real terrain collider must retain the expected shape and owner");
		}

		void CheckLocalHit(const glm::vec3& localPoint)
		{
			const auto& matrix = m_owner->GetTransformComponent().GetCachedWorldMatrix();
			const glm::vec3 point(matrix * glm::vec4(localPoint, 1));
			const glm::vec3 normal = glm::normalize(glm::mat3(glm::transpose(glm::inverse(matrix))) * glm::vec3(0, 1, 0));
			Physics::PhysicsRaycastHit hit;
			const bool bHit = GetECS<PhysicsECS>()->Raycast(point + normal * 20.0f, -normal, 40, hit);
			if (!bHit || glm::length(hit.m_position - point) >= 0.001f)
				std::cerr << "Terrain ray: hit=" << bHit << ", expected=" << point.x << ',' << point.y << ',' << point.z
					<< ", actual=" << hit.m_position.x << ',' << hit.m_position.y << ',' << hit.m_position.z
					<< ", scale=" << Data().m_physicsScale.x << ',' << Data().m_physicsScale.y << ',' << Data().m_physicsScale.z << '\n';
			Require(bHit &&
				hit.m_instanceId == m_owner->GetInstanceId() && glm::length(hit.m_position - point) < 0.001f,
				"the actual Jolt collider must follow translation, rotation and scale of retained local geometry");
		}

	private:
		static TVector<ECS::TBaseSystemPtr> CreateEcs()
		{
			TVector<ECS::TBaseSystemPtr> systems;
			systems.Add(TUniquePtr<TransformECS>::Make());
			systems.Add(TUniquePtr<CameraECS>::Make());
			systems.Add(TUniquePtr<PhysicsECS>::Make());
			systems.Add(TUniquePtr<StaticMeshRendererECS>::Make());
			systems.Add(TUniquePtr<LandscapeECS>::Make());
			return systems;
		}

		GameObjectPtr m_owner;
		TObjectPtr<LandscapeComponent> m_component;
	};

	class VegetationModel final : public Model
	{
	public:
		VegetationModel(FileId id, RHI::RHIMeshPtr mesh) : Model(std::move(id), { std::move(mesh) })
		{
			m_boundsAabb = m_meshes[0]->m_bounds;
			m_renderInstances.Add({ 0, -1, glm::mat4(1) });
			Flush();
		}
	};

	ModelPtr CreateVegetationModel(LandscapeWorld& world)
	{
		auto& driver = RHI::Renderer::GetDriver();
		auto mesh = driver->CreateMesh();
		mesh->m_vertexDescription = driver->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4>();
		mesh->m_bounds = Math::AABB(glm::vec3(0, 0.5f, 0), glm::vec3(0.25f, 0.5f, 0.01f));
		return TObjectPtr<VegetationModel>::Make(world.GetAllocator(),
			FileId("5B80635B-586C-4AF7-B54F-E8E142F690A0"), mesh);
	}

	void UploadVegetationModel(const ModelPtr& model)
	{
		std::array<RHI::VertexP3N3T3B3UV2C4, 3> vertices{};
		vertices[0].m_position = { -0.25f, 0, 0 };
		vertices[1].m_position = { 0.25f, 0, 0 };
		vertices[2].m_position = { 0, 1, 0 };
		for (auto& vertex : vertices)
		{
			vertex.m_normal = { 0, 0, 1 };
			vertex.m_tangent = { 1, 0, 0 };
			vertex.m_bitangent = { 0, 1, 0 };
			vertex.m_color = glm::vec4(1);
		}
		const std::array<uint32_t, 3> indices{ 0, 1, 2 };
		RHI::Renderer::GetDriver()->UpdateMesh(model->GetMeshes()[0], vertices.data(), sizeof(vertices), indices.data(), sizeof(indices));
	}

	template<typename T>
	T ReadMaterialUniform(const MaterialPtr& material, StringHash name)
	{
		auto bindings = material->GetShaderBindings();
		const auto bytes = Tests::ReadMaterialGpuBytes(bindings);
		RHI::ShaderLayoutBindingMember member;
		Require(bindings->GetOrAddShaderBinding("material"_h)->FindVariableInUniformBuffer(name, member) &&
			member.m_absoluteOffset + sizeof(T) <= bytes.Num(), "the terrain uniform must fit its reflected GPU storage");
		T value;
		std::memcpy(&value, bytes.GetData() + member.m_absoluteOffset, sizeof(T));
		return value;
	}

	void CheckPublishedReplacement(const RHI::RHISpatialSceneVersionPtr& before,
		const RHI::RHISpatialSceneVersionPtr& after, const RHI::RHISceneProxyResourcePtr& previous,
		const RHI::RHISceneProxyResourcePtr& current)
	{
		for (const auto& handles : { before->m_sceneVersion->m_staticHandles,
			before->m_sceneVersion->m_stationaryHandles, before->m_sceneVersion->m_dynamicHandles })
		{
			if (!handles) continue;
			for (const auto handle : *handles)
			{
				const RHI::RHISceneInstanceRecord* oldRecord = nullptr;
				Require(before->m_sceneVersion->Resolve(handle, oldRecord), "retained scene handles must resolve");
				if (oldRecord->m_topology != previous) continue;
				const RHI::RHISceneInstanceRecord* newRecord = nullptr;
				Require(after->m_sceneVersion->Resolve(handle, newRecord) && newRecord->m_topology == current &&
					oldRecord->m_topologyRevision == newRecord->m_topologyRevision && previous != current,
					"both scene versions must resolve the same handle to different metadata with the same geometry revision");
				return;
			}
		}
		Require(false, "the retained scene must contain the previous landscape resource");
	}

	void CheckGpuGeometry(const LandscapeData& data, std::optional<glm::vec4> expectedColor = {})
	{
		for (const auto& chunk : data.m_chunks)
		{
			const auto mesh = chunk.m_resource->m_proxy.m_meshes[0];
			Require(mesh->IsReady() && mesh->GetNumLods() == 3, "terrain recovery must publish all three ready LODs");
			auto readback = Tasks::CreateTaskWithResult<bool>("Read terrain mesh"_h, [&data, &chunk, mesh, expectedColor]()
			{
				auto& driver = RHI::Renderer::GetDriver();
				const auto memory = RHI::EMemoryPropertyBit::HostVisible | RHI::EMemoryPropertyBit::HostCoherent;
				const uint32_t row = data.m_chunkResolution + 1;
				const size_t vertexBytes = row * row * sizeof(RHI::VertexP3N3T3B3UV2C4);
				auto vertices = driver->CreateBuffer(vertexBytes, RHI::EBufferUsageBit::BufferTransferDst_Bit, memory);
				if (!driver->CopyBuffer_Immediate(mesh->m_vertexBuffer, vertices, vertexBytes)) return false;
				const auto* values = static_cast<const RHI::VertexP3N3T3B3UV2C4*>(vertices->GetPointer());
				for (uint32_t z = 0; z < row; ++z)
					for (uint32_t x = 0; x < row; ++x)
					{
						const float localX = (chunk.m_chunkX - data.m_chunksX * 0.5f + float(x) / data.m_chunkResolution) * data.m_chunkSize;
						const float localZ = (chunk.m_chunkZ - data.m_chunksZ * 0.5f + float(z) / data.m_chunkResolution) * data.m_chunkSize;
						const auto& vertex = values[z * row + x];
						if (vertex.m_position != glm::vec3(localX, chunk.m_heightSamples[z * row + x], localZ) ||
							vertex.m_texcoord != glm::vec2(localX, localZ) * data.m_textureTiling) return false;
						if (expectedColor && glm::any(glm::greaterThan(glm::abs(vertex.m_color - *expectedColor), glm::vec4(0.00001f)))) return false;
					}
				for (uint32_t lod = 0; lod < mesh->GetNumLods(); ++lod)
				{
					TVector<uint32_t> expected;
					AppendLandscapeLodIndices(data.m_chunkResolution,
						BuildLandscapeLodCoordinates(data.m_chunkResolution, 1u << lod), false, expected);
					const auto selected = lod == 0 ? mesh : mesh->GetLod(lod);
					if (selected->GetIndexCount() != expected.Num()) return false;
					const size_t bytes = expected.Num() * sizeof(uint32_t);
					auto indices = driver->CreateBuffer(bytes, RHI::EBufferUsageBit::BufferTransferDst_Bit, memory);
					if (!driver->CopyBuffer_Immediate(selected->m_indexBuffer, indices, bytes,
						selected->m_firstIndex * sizeof(uint32_t)) || !std::equal(expected.begin(), expected.end(),
							static_cast<const uint32_t*>(indices->GetPointer()))) return false;
				}
				return true;
			}, EThreadType::RHI);
			readback->Run();
			readback->Wait();
			Require(readback->GetResult(), "terrain GPU vertices, UVs and every LOD index range must match the published geometry");
		}
	}

	void TestRemovalPublication(const MaterialPtr& material, uint32_t count, bool bDeferred)
	{
		LandscapeWorld world(material);
		TVector<GameObjectPtr> owners;
		for (uint32_t i = 0; i < count; ++i)
		{
			auto owner = world.Instantiate("Removed terrain");
			owner->SetMobilityType(EMobilityType::Static);
			owner->GetTransformComponent().SetPosition(glm::vec3(32 + i * 16, 0, 0));
			auto component = owner->AddComponent<LandscapeComponent>();
			auto& data = world.GetECS<LandscapeECS>()->GetComponentData(component->GetComponentIndex());
			data.SetSettings(1, 1, 8, 4, 0, 0.1f, 1337, 1);
			data.SetLodSettings({ 8, 16 }, 0);
			data.SetMaterial(material);
			owners.Add(owner);
		}
		world.StepWithMaterialCommands();
		auto retained = world.Scene();
		Require(retained->m_sceneVersion->m_staticHandles->Num() == count + 2,
			"every real terrain chunk must be published before removal");
		for (uint32_t i = 0; i < count; ++i)
		{
			Physics::PhysicsRaycastHit hit;
			Require(world.GetECS<PhysicsECS>()->Raycast({ 32 + i * 16, 20, 0 }, { 0, -1, 0 }, 40, hit) &&
				hit.m_instanceId == owners[i]->GetInstanceId(),
				"each removed owner must start with a real Jolt terrain body");
			if (bDeferred) world.Destroy(owners[i]);
			else world.DestroyImmediate(owners[i]);
		}
		if (bDeferred) world.DestroyPendingGameObjects();
		for (uint32_t i = 0; i < count; ++i)
		{
			Physics::PhysicsRaycastHit hit;
			Require(!world.GetECS<PhysicsECS>()->Raycast({ 32 + i * 16, 20, 0 }, { 0, -1, 0 }, 40, hit),
				"Jolt bodies must be removed immediately, before rendering publishes the batch");
		}
		auto current = world.Scene();
		Require(current->m_revision == retained->m_revision + 1 &&
			current->m_sceneVersion->m_staticHandles->Num() == 2 &&
			retained->m_sceneVersion->m_staticHandles->Num() == count + 2,
			"one terrain publication must remove the batch without altering an older view");
		world.CheckHit(-4, 0);
		for (const auto handle : *retained->m_sceneVersion->m_staticHandles)
		{
			const RHI::RHISceneInstanceRecord* record = nullptr;
			Require(retained->m_sceneVersion->Resolve(handle, record), "retained terrain records must still resolve");
			if (record->m_worldMatrix[3].x == 0) continue; // The original two chunks are still live.
			const auto resource = record->m_topology.DynamicCast<const RHI::RHISceneProxyResource>();
			const auto mesh = resource->m_proxy.m_meshes[0];
			auto read = Tasks::CreateTaskWithResult<bool>("Read removed terrain"_h, [mesh]()
			{
				const auto memory = RHI::EMemoryPropertyBit::HostVisible | RHI::EMemoryPropertyBit::HostCoherent;
				auto& driver = RHI::Renderer::GetDriver();
				auto vertex = driver->CreateBuffer(sizeof(RHI::VertexP3N3T3B3UV2C4),
					RHI::EBufferUsageBit::BufferTransferDst_Bit, memory);
				if (!driver->CopyBuffer_Immediate(mesh->m_vertexBuffer, vertex, sizeof(RHI::VertexP3N3T3B3UV2C4))) return false;
				const auto& first = *static_cast<const RHI::VertexP3N3T3B3UV2C4*>(vertex->GetPointer());
				return first.m_position == glm::vec3(-4, 0, -4) && first.m_texcoord == glm::vec2(-4);
			}, EThreadType::RHI);
			read->Run();
			read->Wait();
			Require(read->GetResult(), "retained terrain vertices must remain readable on the GPU after owner destruction");
		}
		world.Step();
		Require(world.Scene() == current, "unchanged terrain must not republish the removal on the next Tick");
		std::cout << "Landscape removal: " << count << " owners, deferred=" << bDeferred <<
			"; one publication, immediate Jolt cleanup and retained GPU vertices passed\n";
	}

	void TestFullReplacement(const MaterialPtr& material, VkResult error)
	{
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		Require(data.m_chunks.Num() == 2 && data.m_physicsBodies.Num() == 2 && !data.IsDirty(),
			"the warm terrain fixture must publish two chunks and colliders");
		CheckGpuGeometry(data);
		world.CheckHit(-4, 0);
		world.CheckHit(4, 0);
		const auto oldScene = world.Scene();
		const auto oldRevision = data.m_buildRevision;
		const auto oldBodies = data.m_physicsBodies;
		const std::array oldResources = { data.m_chunks[0].m_resource, data.m_chunks[1].m_resource };
		data.SetSettings(3, 1, 8, 4, 0, 0.1f, 1337, 1);
		Tests::RequireMainMeshUploadRefusal([&]() { world.Step(); }, error, 1);
		Require(data.m_chunks.Num() == 2 && data.m_chunks[0].m_resource == oldResources[0] &&
			data.m_chunks[1].m_resource == oldResources[1] && data.m_buildRevision == oldRevision &&
			data.m_physicsBodies == oldBodies && world.Scene() == oldScene && data.IsDirty(),
			"a rejected terrain replacement must retain every old chunk, collider, scene and pending rebuild");
		world.CheckHit(-4, 0);
		world.CheckHit(4, 0);
		world.Step();
		Drain();
		Require(data.m_chunks.Num() == 3 && data.m_physicsBodies.Num() == 3 && !data.IsDirty() &&
			world.Scene() != oldScene && oldScene->m_sceneVersion->m_staticHandles->Num() == 2,
			"unchanged-input retry must publish the complete new terrain without modifying a retained scene");
		world.CheckHit(10, 0);
		CheckGpuGeometry(data);
		const auto recovered = world.Scene();
		world.Step();
		Require(world.Scene() == recovered, "a recovered terrain must not rebuild again on an unchanged tick");
	}

	void TestColdUpload(const MaterialPtr& material, VkResult error)
	{
		LandscapeWorld world(material);
		Tests::RequireMainMeshUploadRefusal([&]() { world.Step(); }, error, 0);
		auto& data = world.Data();
		Require(data.m_chunks.IsEmpty() && data.m_physicsBodies.IsEmpty() && data.m_buildRevision == 0 && data.IsDirty(),
			"a cold rejected upload must publish no terrain or collision and keep its retry pending");
		world.Step();
		Drain();
		Require(data.m_chunks.Num() == 2 && data.m_physicsBodies.Num() == 2 && !data.IsDirty(),
			"the next unchanged tick must recover cold terrain geometry and collision");
		CheckGpuGeometry(data);
		world.CheckHit(-4, 0);
	}

	void TestPartialReplacement(const MaterialPtr& material, VkResult error)
	{
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		const auto oldScene = world.Scene();
		const auto oldRevision = data.m_buildRevision;
		const auto oldBodies = data.m_physicsBodies;
		const std::array oldResources = { data.m_chunks[0].m_resource, data.m_chunks[1].m_resource };
		const std::array neighborBodies = { data.m_chunks[1].m_terrainBodyId, data.m_chunks[1].m_vegetationBodyId };
		data.SetAuthoredStamps({ { -4, 0, 0.5f, 1, ELandscapeSculptOperation::Raise } }, {});
		Require(!data.m_bRebuildAllChunks && data.m_dirtyChunks.Num() == 1,
			"the sculpt fixture must affect exactly one chunk");
		Tests::RequireMainMeshUploadRefusal([&]() { world.Step(); }, error, 0);
		Require(data.m_chunks[0].m_resource == oldResources[0] && data.m_chunks[1].m_resource == oldResources[1] &&
			data.m_buildRevision == oldRevision && data.m_physicsBodies == oldBodies && world.Scene() == oldScene && data.IsDirty(),
			"a rejected sculpt upload must preserve both chunks, their colliders and the published scene");
		world.CheckHit(-4, 0);
		world.Step();
		Drain();
		Require(!data.IsDirty() && data.m_buildRevision == oldRevision + 1 && data.m_physicsBodies.Num() == 2 &&
			data.m_chunks[0].m_resource != oldResources[0] && data.m_chunks[1].m_resource == oldResources[1] &&
			std::array{ data.m_chunks[1].m_terrainBodyId, data.m_chunks[1].m_vegetationBodyId } == neighborBodies && world.Scene() != oldScene,
			"sculpt retry must replace only the edited chunk and keep its neighbor's geometry and collision");
		world.CheckHit(-4, 1);
		world.CheckHit(4, 0);
		CheckGpuGeometry(data);
		const auto recovered = world.Scene();
		world.Step();
		Require(world.Scene() == recovered, "an unchanged recovered sculpt must not rebuild");
	}

	void TestPendingUpload(const MaterialPtr& material)
	{
		LandscapeWorld world(material);
		auto& data = world.Data();
		data.SetSettings(1, 1, 8, 4, 0, 0.1f, 1337, 1);
		Tests::RequirePendingMeshUpload([&]() { world.Step(); }, [&]()
		{
			Require(data.m_chunks.Num() == 1 && !data.IsDirty(), "an accepted upload must finish the CPU terrain rebuild");
			const auto mesh = data.m_chunks[0].m_resource->m_proxy.m_meshes[0];
			Require(!mesh->IsReady() && !mesh->HasInitializationFailed(),
				"accepted terrain must remain pending until its real upload fence completes");
		});
		Drain();
		CheckGpuGeometry(data);
		world.CheckHit(0, 0);
	}

	void CheckMatrix(const glm::mat4& actual, const glm::mat4& expected)
	{
		for (size_t column = 0; column < 4; ++column)
			Require(glm::length(actual[column] - expected[column]) < 0.0001f,
				"rendered world matrices must match the published owner and local instance transforms");
	}

	void CheckTransformPublication(const RHI::RHISpatialSceneVersionPtr& before,
		const RHI::RHISpatialSceneVersionPtr& after, const glm::mat4& expected)
	{
		Require(before != after && after->m_shadowCastersRevision > before->m_shadowCastersRevision,
			"a transform update must publish scene and shadow changes");
		for (const auto& handles : { before->m_sceneVersion->m_staticHandles,
			before->m_sceneVersion->m_stationaryHandles, before->m_sceneVersion->m_dynamicHandles })
		{
			if (!handles) continue;
			for (const auto handle : *handles)
			{
				const RHI::RHISceneInstanceRecord* oldRecord = nullptr;
				const RHI::RHISceneInstanceRecord* newRecord = nullptr;
				Require(before->m_sceneVersion->Resolve(handle, oldRecord) && after->m_sceneVersion->Resolve(handle, newRecord) &&
					oldRecord->m_topology == newRecord->m_topology && oldRecord->m_topologyRevision == newRecord->m_topologyRevision &&
					oldRecord->m_worldMatrix != newRecord->m_worldMatrix,
					"retained scene versions must share unchanged topology under stable handles and own different transforms");
				CheckMatrix(newRecord->m_worldMatrix, expected);
				const auto* resource = static_cast<const RHI::RHISceneProxyResource*>(newRecord->m_topology.GetRawPtr());
				Math::AABB bounds;
				const auto& topology = resource->m_proxy;
				for (size_t mesh = 0; mesh < topology.m_meshes.Num(); ++mesh)
				{
					auto meshBounds = topology.m_meshes[mesh]->m_bounds;
					meshBounds.Apply(topology.m_meshModelMatrices[mesh]);
					bounds.Extend(meshBounds);
				}
				for (const auto& group : topology.m_instancedGroups)
				{
					Math::AABB groupBounds;
					for (size_t mesh = 0; mesh < group.m_meshes.Num(); ++mesh)
					{
						auto meshBounds = group.m_meshes[mesh]->m_bounds;
						meshBounds.Apply(group.m_meshTransforms[mesh]);
						groupBounds.Extend(meshBounds);
					}
					for (const auto& matrix : group.m_instanceTransforms)
					{
						auto instanceBounds = groupBounds;
						instanceBounds.Apply(matrix);
						bounds.Extend(instanceBounds);
					}
				}
				bounds.Apply(expected);
				Require(glm::length(bounds.m_min - newRecord->m_worldBounds.m_min) < 0.0001f &&
					glm::length(bounds.m_max - newRecord->m_worldBounds.m_max) < 0.0001f,
					"published culling bounds must follow the current owner transform");
				for (const auto* record : { oldRecord, newRecord })
				{
					RHI::RHIVisibleSceneProxy main{ handle, *record, *resource };
					RHI::RHIVisibleShadowCaster shadow{ handle, *record, *resource };
					for (size_t mesh = 0; mesh < resource->m_proxy.m_meshes.Num(); ++mesh)
					{
						CheckMatrix(main.ResolveMeshWorldMatrix(mesh), record->m_worldMatrix);
						CheckMatrix(shadow.ResolveMeshWorldMatrix(resource->m_proxy.m_shadowCaster->m_meshes[mesh]), record->m_worldMatrix);
					}
					for (const auto& group : resource->m_proxy.m_instancedGroups)
						for (size_t instance = 0; instance < group.m_instanceTransforms.Num(); ++instance)
							for (size_t mesh = 0; mesh < group.m_meshes.Num(); ++mesh)
							{
								const auto matrix = record->m_worldMatrix * group.m_instanceTransforms[instance] * group.m_meshTransforms[mesh];
								CheckMatrix(main.ResolveInstancedMeshWorldMatrix(group, instance, mesh), matrix);
								CheckMatrix(shadow.ResolveInstancedMeshWorldMatrix(group, instance, mesh), matrix);
							}
				}
			}
		}
	}

	size_t CountVisible(const RHI::RHISpatialSceneVersionPtr& scene, const glm::vec3& center)
	{
		Math::Frustum frustum;
		frustum.ExtractFrustumPlanes(glm::inverse(glm::lookAtRH(center + glm::vec3(0, 12, 16), center, glm::vec3(0, 1, 0))),
			1, 60, 0.1f, 100);
		auto view = RHI::RHISceneViewPtr::Make();
		view->AddSceneVersion(scene);
		return view->TraceScene(frustum).Num();
	}

	void SetVegetationProfiles(LandscapeData& data, const TVector<LandscapeVegetationProfile>& profiles)
	{
		TVector<LandscapeVegetationSettings> settings;
		for (const auto& profile : profiles) settings.Add(profile.m_settings);
		data.SetVegetationProfiles(settings);
	}

	void TestVegetationProfileEdits(const MaterialPtr& material)
	{
		LandscapeWorld world(material);
		world.AddCamera();
		auto model = CreateVegetationModel(world);
		UploadVegetationModel(model);
		Drain();
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 2;
		data.m_vegetationProfiles = { profile, profile };
		world.Step();
		Drain();
		const auto revision = data.m_buildRevision;
		const auto terrain = data.m_chunks[0].m_resource;
		const auto bodies = data.m_physicsBodies;
		const std::array terrainBodies = { data.m_chunks[0].m_terrainBodyId, data.m_chunks[1].m_terrainBodyId };
		const auto original = data.m_chunks[0].m_vegetationProxies[0];
		const auto neighbor = data.m_chunks[0].m_vegetationProxies[1].m_resource;
		const auto oldScene = world.Scene();
		auto selected = [&]() -> const LandscapeVegetationRenderProxy&
		{
			const auto& proxies = data.m_chunks[0].m_vegetationProxies;
			const auto index = proxies.FindIf([](const auto& proxy) { return proxy.m_profileIndex == 0; });
			Require(index != size_t(-1), "the selected vegetation profile must remain resident");
			return proxies[index];
		};
		auto checkTerrain = [&]()
		{
			Require(data.m_buildRevision == revision && data.m_chunks[0].m_resource == terrain &&
				std::array{ data.m_chunks[0].m_terrainBodyId, data.m_chunks[1].m_terrainBodyId } == terrainBodies,
				"vegetation edits must retain both terrain bodies, topology and uploaded mesh storage");
		};
		auto profiles = data.m_vegetationProfiles;
		profiles[0].m_settings.m_instancesPerChunk = 4;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(data.m_buildRevision == revision && data.m_chunks[0].m_resource == terrain && data.m_physicsBodies == bodies,
			"vegetation placement edits must retain terrain geometry and collision");
		Require(data.m_chunks[0].m_vegetationProxies[0].m_instanceCount == 4 &&
			data.m_chunks[0].m_vegetationProxies[1].m_resource == neighbor,
			"a placement edit must replace only its selected vegetation profile");
		Require(selected().m_revision != original.m_revision && world.Scene() != oldScene &&
			original.m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms.Num() == 2,
			"placement updates must advance vegetation revisions and preserve previous scene resources");
		profiles[0].m_settings.m_minScale = profiles[0].m_settings.m_maxScale = 2;
		profiles[0].m_settings.m_groundOffset = 3;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		for (const auto& matrix : selected().m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms)
			Require(std::abs(glm::length(glm::vec3(matrix[0])) - 2) < 0.0001f && matrix[3].y == 3,
				"procedural scale and ground offset edits must regenerate only the selected placements");
		const auto placements = selected();
		profiles[0].m_settings.m_cullDistance = 80;
		profiles[0].m_settings.m_shadowDistance = 12;
		profiles[0].m_settings.m_screenCoverageThresholds = { 0.75f, 0.25f };
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(selected().m_revision == placements.m_revision && selected().m_resource != placements.m_resource &&
			selected().m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms ==
			placements.m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms &&
			selected().m_resource->m_proxy.m_lodPolicy.m_maxCameraDistance == 80 &&
			selected().m_resource->m_proxy.m_lodPolicy.m_screenCoverageThresholds[0] == 0.75f &&
			selected().m_resource->m_proxy.m_instancedGroups[0].m_maxShadowDistance == 12,
			"render policy edits must retain placements and change only their render metadata");
		const auto renderOnly = selected();
		for (float height : { 4.0f, 6.0f })
		{
			const auto previousBody = data.m_chunks[0].m_vegetationBodyId;
			profiles[0].m_settings.m_colliderRadius = 0.2f;
			profiles[0].m_settings.m_colliderHeight = height;
			profiles[0].m_settings.m_colliderOffsetY = height * 0.5f;
			SetVegetationProfiles(data, profiles);
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			Require(selected().m_resource == renderOnly.m_resource && data.m_chunks[0].m_vegetationBodyId != previousBody &&
				data.m_physicsBodies.Num() == 4, "collider-only edits must replace vegetation collision, not rendering or terrain");
			const auto& matrix = selected().m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms[0];
			const glm::vec3 top(matrix * glm::vec4(0, height, 0, 1));
			Physics::PhysicsRaycastHit hit;
			Require(world.GetECS<PhysicsECS>()->Raycast(top + glm::vec3(0, 10, 0), { 0, -1, 0 }, 20, hit) &&
				glm::length(hit.m_position - top) < 0.001f, "vegetation collision edits must update the actual Jolt capsule");
			checkTerrain();
		}
		profiles[0].m_settings.m_residency = ELandscapeVegetationResidency::Grass;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(IsLandscapeGrassProxy(selected()) && selected().m_mobility == EMobilityType::Dynamic &&
			data.m_activeGrassInstances == 8 && data.m_physicsBodies.Num() == 2 && data.m_chunks[0].m_bakeVegetation.Num() == 2,
			"switching to grass must move placements into residency and remove their bake geometry and colliders");
		const auto grass = selected();
		profiles[0].m_settings.m_groundOffset = 4;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(selected().m_resource != grass.m_resource && selected().m_instanceCount == grass.m_instanceCount &&
			selected().m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms[0][3].y == 4,
			"grass source edits must replace stale residents even when their capacity and view are unchanged");
		profiles[0].m_settings.m_residency = ELandscapeVegetationResidency::Persistent;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(!IsLandscapeGrassProxy(selected()) && selected().m_mobility == EMobilityType::Static &&
			data.m_activeGrassInstances == 0 && data.m_physicsBodies.Num() == 4 && data.m_chunks[0].m_bakeVegetation.Num() == 6,
			"returning to persistent residency must restore bake placements and collisions exactly once");
		checkTerrain();
		const auto retained = selected();
		profiles.Resize(1);
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(selected().m_resource == retained.m_resource && data.m_chunks[0].m_vegetationProxies.Num() == 1 &&
			data.m_chunks[0].m_bakeVegetation.Num() == 4, "removing a profile must retain its unaffected neighbor");
		profiles[0].m_settings.m_priority = 10;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(selected().m_resource == retained.m_resource, "priority-only edits must not regenerate persistent placements");
		checkTerrain();
		profiles[0].m_settings.m_meshIndex = 99;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(data.m_chunks[0].m_vegetationProxies.IsEmpty() && data.m_chunks[0].m_bakeVegetation.Num() == 4,
			"an absent mesh selection must hide rendering while retaining its authored placements");
		profiles[0].m_settings.m_meshIndex = -1;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(selected().m_instanceCount == 4 && selected().m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms ==
			retained.m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms,
			"restoring a mesh selection must recover rendering from retained placements without regenerating terrain");
		checkTerrain();
		CheckGpuGeometry(data);
	}

	void TestGrassPriorityAndBudget(const MaterialPtr& material)
	{
		struct BudgetRestore
		{
			uint32_t& m_value = const_cast<Settings::GraphicsQualityProfile&>(App::GetActiveGraphicsSettings()).m_vegetationInstanceBudget;
			uint32_t m_previous = m_value;
			~BudgetRestore() { m_value = m_previous; }
		} budget;
		budget.m_value = 4;
		LandscapeWorld world(material);
		world.AddCamera();
		auto model = CreateVegetationModel(world);
		UploadVegetationModel(model);
		Drain();
		auto& data = world.Data();
		data.SetSettings(1, 1, 8, 4, 0, 0.1f, 1337, 1);
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 4;
		profile.m_settings.m_residency = ELandscapeVegetationResidency::Grass;
		data.m_vegetationProfiles = { profile, profile };
		world.Step();
		Drain();
		const auto terrain = data.m_chunks[0].m_resource;
		const auto bodies = data.m_physicsBodies;
		const auto revision = data.m_buildRevision;
		const auto sourceRevision = data.m_vegetationRevision;
		Require(data.m_activeGrassInstances == 4 && data.m_chunks[0].m_vegetationProxies[0].m_profileIndex == 0,
			"a limited grass budget must initially select the first equally ranked profile");
		auto profiles = data.m_vegetationProfiles;
		profiles[1].m_settings.m_priority = 10;
		SetVegetationProfiles(data, profiles);
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(data.m_activeGrassInstances == 4 && data.m_chunks[0].m_vegetationProxies.Num() == 1 &&
			data.m_chunks[0].m_vegetationProxies[0].m_profileIndex == 1,
			"a priority edit must let the existing residency scheduler replace the selected grass profile");
		const auto resident = data.m_chunks[0].m_vegetationProxies[0].m_resource;
		budget.m_value = 6;
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(data.m_activeGrassInstances == 6 && data.m_chunks[0].m_vegetationProxies.Num() == 2 &&
			data.m_chunks[0].m_vegetationProxies.ContainsIf([&](const auto& proxy) { return proxy.m_resource == resident; }),
			"budget growth must fill additional residents while retaining an unchanged full profile");
		budget.m_value = 0;
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(data.m_activeGrassInstances == 0 && data.m_chunks[0].m_vegetationProxies.IsEmpty() &&
			data.m_chunks[0].m_resource == terrain && data.m_physicsBodies == bodies && data.m_buildRevision == revision &&
			data.m_vegetationRevision == sourceRevision,
			"residency changes and eviction must not modify terrain or vegetation source revisions");
	}

	void TestVegetationSourceReload(const MaterialPtr& material, const std::filesystem::path& workspace)
	{
		LandscapeWorld world(material);
		auto model = CreateVegetationModel(world);
		UploadVegetationModel(model);
		Drain();
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 3;
		profile.m_settings.m_colliderRadius = 0.25f;
		data.m_vegetationProfiles.Add(profile);
		world.Step();
		Drain();
		const auto terrainRevision = data.m_buildRevision;
		const auto terrain = data.m_chunks[0].m_resource;
		const auto terrainBody = data.m_chunks[0].m_terrainBodyId;
		const auto originalScene = world.Scene();
		LandscapeVegetationAssetData source;
		source.m_chunksX = 2;
		source.m_chunksZ = 1;
		source.m_chunkSize = 8;
		source.m_profileCount = 1;
		for (uint32_t x = 0; x < 2; ++x)
		{
			LandscapeVegetationChunkData chunk;
			chunk.m_chunkX = x;
			for (uint32_t index = 0; index < 2; ++index)
			{
				LandscapeVegetationInstance instance;
				instance.m_stableId = x * 2 + index + 1;
				instance.m_transform[3] = glm::vec4(-6.0f + x * 8 + index * 2, 2, 0, 1);
				instance.m_lodBias = -1;
				instance.m_cullDistanceScale = 0.5f;
				instance.m_shadowDistanceScale = 1.5f;
				if (index == 1) instance.m_flags = 0;
				chunk.m_instances.Add(instance);
			}
			source.m_chunks.Add(std::move(chunk));
		}
		const auto path = workspace / "Content" / "LandscapeSource.vegetation";
		std::string diagnostic;
		Require(source.Save(path, diagnostic), "the binary vegetation source fixture must save");
		const auto id = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		Require(static_cast<bool>(id), "the binary vegetation source must register as a project asset");
		data.SetVegetationAsset(id);
		auto checkSource = [&]()
		{
			Require(data.m_buildRevision == terrainRevision && data.m_chunks[0].m_resource == terrain &&
				data.m_chunks[0].m_terrainBodyId == terrainBody, "source reload must preserve real terrain resources and collision");
			for (size_t index = 0; index < data.m_chunks.Num(); ++index)
			{
				const auto& chunk = data.m_chunks[index];
				const auto& group = chunk.m_vegetationProxies[0].m_resource->m_proxy.m_instancedGroups[0];
				const auto& expected = source.m_chunks[index].m_instances[0];
				Require(group.m_instanceTransforms.Num() == 1 && group.m_instanceTransforms[0] == expected.m_transform &&
					group.m_instanceLodBiases[0] == expected.m_lodBias && group.m_instanceCullDistanceScales[0] == 0.5f &&
					group.m_instanceShadowDistanceScales[0] == 1.5f && chunk.m_bakeVegetation.Num() == 1 &&
					chunk.m_bakeVegetation[0].m_stableId == expected.m_stableId,
					"reloaded sources must preserve enabled instances, stable IDs and per-instance render overrides");
			}
		};
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		checkSource();
		const auto previous = data.m_chunks[0].m_vegetationProxies[0];
		for (auto& chunk : source.m_chunks) chunk.m_instances[0].m_transform[3].y = 5;
		Require(source.Save(path, diagnostic), "replacement vegetation source must save");
		data.RequestVegetationAssetReload();
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		checkSource();
		Require(data.m_chunks[0].m_vegetationProxies[0].m_revision != previous.m_revision &&
			previous.m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms[0][3].y == 2 && world.Scene() != originalScene,
			"source replacement must publish a new vegetation revision without mutating retained scenes");
		TVector<LandscapeBakeGeometrySnapshot> snapshots;
		Require(world.GetECS<LandscapeECS>()->CollectBakeGeometrySnapshots(snapshots, diagnostic) && snapshots.Num() == 4,
			"bake snapshots must contain unchanged terrain plus only enabled persistent vegetation");
		Require(snapshots[0].m_sourceRevision == data.m_chunks[0].m_buildRevision &&
			snapshots[1].m_sourceRevision == data.m_chunks[0].m_vegetationRevision && snapshots[1].m_worldMatrix[3].y == 5,
			"terrain and vegetation bake revisions must describe their independent source updates");
		std::ofstream(path, std::ios::trunc) << "invalid vegetation fixture";
		data.RequestVegetationAssetReload();
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		checkSource();
		data.RequestSaveVegetation();
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		LandscapeVegetationAssetData saved;
		Require(saved.Load(path, diagnostic) && saved.m_chunks[0].m_instances[0].m_transform[3].y == 5 &&
			!saved.m_chunks[0].m_instances[1].IsEnabled(), "saving a loaded source must retain its last valid authored data");
		data.SetVegetationAsset({});
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(!data.m_bVegetationAssetLoaded && data.m_chunks[0].m_vegetationProxies[0].m_instanceCount == 3 &&
			data.m_buildRevision == terrainRevision, "removing the source must restore procedural vegetation without rebuilding terrain");
		std::ofstream(path, std::ios::trunc) << "invalid vegetation fixture";
		data.SetVegetationAsset(id);
		data.RequestSaveVegetation();
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(data.IsDirty() && saved.Load(path, diagnostic) && saved.GetInstanceCount() == 6,
			"saving procedural vegetation must schedule publication of its newly authored source");
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(!data.IsDirty() && data.m_buildRevision == terrainRevision &&
			data.m_chunks[0].m_bakeVegetation[0].m_transform == saved.m_chunks[0].m_instances[0].m_transform,
			"generated-save publication must use saved placements and leave terrain unchanged");
		world.Regenerate();
		world.Step();
		Drain();
		Require(data.m_buildRevision == terrainRevision + 2 && data.m_chunks[0].m_resource != terrain,
			"the explicit component Regenerate action must still rebuild terrain");
		CheckGpuGeometry(data);
	}

	void TestVegetationEditDuringSculptRetry(const MaterialPtr& material)
	{
		LandscapeWorld world(material);
		auto model = CreateVegetationModel(world);
		UploadVegetationModel(model);
		Drain();
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 2;
		data.m_vegetationProfiles.Add(profile);
		world.Step();
		Drain();
		const auto before = world.Scene();
		const auto neighbor = data.m_chunks[1].m_resource;
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		auto profiles = data.m_vegetationProfiles;
		profiles[0].m_settings.m_instancesPerChunk = 4;
		SetVegetationProfiles(data, profiles);
		data.SetAuthoredStamps({ { -4, 0, 0.5f, 1, ELandscapeSculptOperation::Raise } }, {});
		Tests::RequireMainMeshUploadRefusal([&]() { world.Step(); }, VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
		Require(world.Scene() == before && data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
			data.m_chunks[0].m_vegetationProxies[0].m_instanceCount == 2 && data.m_chunks[1].m_vegetationProxies[0].m_instanceCount == 2,
			"a failed terrain edit must retain old vegetation while its source update is still pending");
		world.Step();
		Drain();
		Require(data.m_buildRevision == revision + 1 && data.m_chunks[1].m_resource == neighbor &&
			data.m_chunks[1].m_terrainBodyId == bodies[1] && data.m_chunks[0].m_vegetationProxies[0].m_instanceCount == 4 &&
			data.m_chunks[1].m_vegetationProxies[0].m_instanceCount == 4,
			"a successful retry must apply vegetation edits to both chunks while rebuilding only the sculpted terrain");
		world.CheckHit(-4, 1);
		world.CheckHit(4, 0);
		CheckGpuGeometry(data);
	}

	void TestTerrainTransform(const MaterialPtr& material)
	{
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		const auto resource = data.m_chunks[0].m_resource;
		const auto triangles = data.m_chunks[0].m_bakeTriangles;
		const auto scene = world.Scene();
		world.Owner()->GetTransformComponent().SetPosition({ 60, 3, 0 });
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
			data.m_chunks[0].m_resource == resource && data.m_chunks[0].m_bakeTriangles == triangles && world.Scene() != scene,
			"moving terrain must retain local topology, collision bodies and bake geometry and publish the new transform");
		CheckTransformPublication(scene, world.Scene(), world.Owner()->GetTransformComponent().GetCachedWorldMatrix());
		Require(CountVisible(scene, glm::vec3(0)) == 2 && CountVisible(world.Scene(), glm::vec3(0)) == 0 &&
			CountVisible(world.Scene(), { 60, 3, 0 }) == 2,
			"retained and current spatial trees must cull terrain at their own published positions");
		world.CheckHit(56, 3);
		const auto rotation = glm::angleAxis(0.4f, glm::vec3(0, 1, 0)) * glm::angleAxis(0.2f, glm::vec3(1, 0, 0));
		for (const auto scale : { glm::vec3(1), glm::vec3(2, 1.5f, 0.75f), glm::vec3(-1.5f, 0.8f, 2) })
		{
			const auto previousScene = world.Scene();
			const auto previousBodies = data.m_physicsBodies;
			world.Owner()->GetTransformComponent().SetRotation(rotation);
			world.Owner()->GetTransformComponent().SetScale(glm::vec4(scale, 1));
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			Require(data.m_buildRevision == revision && data.m_chunks[0].m_resource == resource &&
				data.m_chunks[0].m_bakeTriangles == triangles && data.m_physicsBodies.Num() == bodies.Num() &&
				(scale == glm::vec3(1) ? data.m_physicsBodies == previousBodies : data.m_physicsBodies != previousBodies),
				"rotation must retain bodies, scale must replace only collision, and both must retain local rendering geometry");
			CheckTransformPublication(previousScene, world.Scene(), world.Owner()->GetTransformComponent().GetCachedWorldMatrix());
			// Cast inside triangles, not at shared vertices where roundoff can miss every adjacent face.
			world.CheckLocalHit({ -3.75f, 0, 0.125f });
			world.CheckLocalHit({ 4.25f, 0, 0.125f });
			const auto unchanged = world.Scene();
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			Require(world.Scene() == unchanged, "an applied transform must not publish again on unchanged ticks");
		}
		TVector<LandscapeBakeGeometrySnapshot> snapshots;
		std::string diagnostic;
		Require(world.GetECS<LandscapeECS>()->CollectBakeGeometrySnapshots(snapshots, diagnostic) && snapshots.Num() == 2 &&
			snapshots[0].m_triangles == triangles, "GI snapshots must retain local triangles after transform-only edits");
		CheckMatrix(snapshots[0].m_worldMatrix, world.Owner()->GetTransformComponent().GetCachedWorldMatrix());
		const auto neighbor = data.m_chunks[1].m_resource;
		data.SetAuthoredStamps({ { -4, 0, 0.5f, 1, ELandscapeSculptOperation::Raise } }, {});
		world.Owner()->GetTransformComponent().SetPosition({ 80, 8, 0 });
		Tests::RequireMainMeshUploadRefusal([&]() { world.Step(); }, VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
		Require(data.m_buildRevision == revision && data.m_chunks[0].m_resource == resource && data.IsDirty(),
			"a refused sculpt must retain local geometry while applying the independent owner transform");
		world.CheckLocalHit({ -3.75f, 0, 0.125f });
		world.Step();
		Drain();
		Require(data.m_buildRevision == revision + 1 && data.m_chunks[1].m_resource == neighbor,
			"sculpt recovery after movement must rebuild only the edited local chunk");
		world.CheckLocalHit({ -3.75f, 0.8125f, 0.125f });
		world.CheckLocalHit({ 4.25f, 0, 0.125f });
		CheckGpuGeometry(data);
		std::cout << "Landscape transform: shared topology, main/shadow matrices, culling, Jolt movement/scaling and sculpt recovery passed\n";
	}

	void TestTerrainMaterialMetadata(MaterialPtr material, const MaterialPtr& replacement)
	{
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		const auto scene = world.Scene();
		const auto bodies = data.m_physicsBodies;
		const auto revision = data.m_buildRevision;
		const auto old = data.m_chunks[0].m_resource;
		const auto mesh = old->m_proxy.m_meshes[0];
		const auto triangles = data.m_chunks[0].m_bakeTriangles;
		const auto heights = data.m_chunks[0].m_heightSamples;
		material->SetRenderState(RHI::RenderState(true, true, 0, false, RHI::ECullMode::Back,
			RHI::EBlendMode::None, RHI::EFillMode::Fill, "Masked"_h.GetHash()));
		material->SetUniform("material.alphaCutoff"_h, 0.375f);
		world.StepWithMaterialCommands();
		Require(data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
			data.m_chunks[0].m_resource->m_proxy.m_meshes[0] == mesh &&
			data.m_chunks[0].m_bakeTriangles == triangles && data.m_chunks[0].m_heightSamples == heights,
			"terrain material metadata must not rebuild terrain meshes, CPU geometry or collision");
		const auto& proxy = data.m_chunks[0].m_resource->m_proxy;
		Require(data.m_chunks[0].m_resource != old && proxy.m_renderQueueTags[0] == "Masked"_h.GetHash() &&
			proxy.m_alphaCutoffs[0] == 0.375f && proxy.m_shadowCaster &&
			proxy.m_shadowCaster->m_meshes[0].m_alphaCutoff == 0.375f &&
			old->m_proxy.m_renderQueueTags[0] == "Opaque"_h.GetHash() &&
			world.Scene() != scene && scene->m_sceneVersion->m_staticHandles->Num() == 2,
			"new depth/shadow metadata must publish without changing retained proxies or scene versions");
		CheckPublishedReplacement(scene, world.Scene(), old, data.m_chunks[0].m_resource);
		world.CheckHit(-4, 0);
		CheckGpuGeometry(data);
		const auto readyScene = world.Scene();
		world.Step();
		Require(world.Scene() == readyScene, "an unchanged material revision must not republish terrain");
		const auto runtimeMaterial = data.m_runtimeMaterial;
		const glm::vec4 color(0.125f, 0.5f, 0.75f, 1);
		material->SetUniform("material.baseColorFactor"_h, color);
		world.StepWithMaterialCommands();
		Require(data.m_runtimeMaterial == runtimeMaterial && world.Scene() == readyScene &&
			data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
			ReadMaterialUniform<glm::vec4>(data.m_runtimeMaterial, "baseColorFactor"_h) == color,
			"a color-only edit must update actual GPU uniforms without replacing the private material or scene");
		data.SetMaterial(replacement);
		world.StepWithMaterialCommands();
		Require(data.m_runtimeMaterial != runtimeMaterial && data.m_chunks[0].m_resource->m_proxy.m_meshes[0] == mesh &&
			data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
			data.m_chunks[0].m_bakeTriangles == triangles && world.Scene() != readyScene &&
			ReadMaterialUniform<glm::vec4>(data.m_runtimeMaterial, "baseColorFactor"_h) == glm::vec4(1),
			"replacing the terrain source material must retain geometry/collision and publish the new GPU values");
		std::cout << "Landscape material metadata: shared terrain/collision, masked depth/shadow metadata and retained scenes passed\n";
	}

	void TestSharedMaterialMetadata(const std::filesystem::path& workspace, bool bCustomDepth)
	{
		const std::string name = bCustomDepth ? "SharedCustomDepth" : "SharedMaskedDepth";
		Tests::TextureFixture baseColor(workspace, name + "Base");
		Tests::TextureFixture normal(workspace, name + "Normal");
		MaterialAsset::Data source;
		source.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
		source.m_shaderDefines = { "ALPHA_CUTOUT" };
		source.m_renderQueue = "Masked";
		source.m_renderState = RHI::RenderState(true, true, 0, bCustomDepth);
		source.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(0.25f, 0.5f, 0.75f, 0.625f);
		source.m_uniformsFloat["material.alphaCutoff"] = 0.375f;
		source.m_samplers["baseColorSampler"] = baseColor.m_id;
		source.m_samplers["normalSampler"] = normal.m_id;
		const auto path = (workspace / "Content" / (name + ".mat")).string();
		auto* importer = App::GetSubmodule<MaterialImporter>();
		const FileId id = importer->CreateMaterialAsset(path, source);
		MaterialPtr material;
		Require(importer->LoadMaterial_Immediate(id, material), "shared metadata material must load");
		Drain();
		const auto originalShader = material->GetShader();
		auto* textures = App::GetSubmodule<TextureImporter>();
		const uint32_t baseIndex = static_cast<uint32_t>(textures->GetTextureIndex(baseColor.m_id));
		const uint32_t normalIndex = static_cast<uint32_t>(textures->GetTextureIndex(normal.m_id));
		Require(baseIndex != 0 && normalIndex != 0 && baseIndex != normalIndex,
			"metadata parity must use two distinct real texture resources");

		LandscapeWorld world(material);
		world.AddCamera();
		auto model = CreateVegetationModel(world);
		UploadVegetationModel(model);
		Drain();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 2;
		world.Data().m_vegetationProfiles.Add(std::move(profile));
		auto* meshes = world.GetECS<StaticMeshRendererECS>();
		meshes->BeginPlay();
		auto owner = world.Instantiate("Metadata mesh");
		owner->SetMobilityType(EMobilityType::Static);
		auto component = owner->AddComponent<MeshRendererComponent>();
		component->GetData().SetModel(model);
		component->GetMaterials().Add(material);
		component->GetData().MarkDirty();
		auto step = [&]()
		{
			world.StepWithMaterialCommands();
			meshes->Tick(0);
			Drain();
		};
		auto capture = [&]()
		{
			const auto scene = meshes->GetRHIScene()->GetCurrentVersion();
			const RHI::RHISceneInstanceRecord* record = nullptr;
			Require(scene && scene->m_staticHandles->Num() == 1 &&
				scene->Resolve((*scene->m_staticHandles)[0], record), "the ordinary mesh must publish a scene record");
			Require(world.Data().m_chunks[0].m_vegetationProxies.Num() == 1,
				"the same material must also publish a vegetation group");
			auto topology = record->m_topology;
			return std::array<TRefPtr<const RHI::RHISceneProxyResource>, 3>{ topology.DynamicCast<const RHI::RHISceneProxyResource>(),
				world.Data().m_chunks[0].m_resource, world.Data().m_chunks[0].m_vegetationProxies[0].m_resource };
		};
		auto verifyMetadata = [&](const auto& resources, glm::vec4 color, float cutoff, uint32_t sampler,
			size_t queue, bool bCustom, const ShaderSetPtr& expectedShader)
		{
			for (size_t index = 0; index < 2; ++index)
			{
				const auto& proxy = resources[index]->m_proxy;
				Require(proxy.m_baseColorFactors[0] == color && proxy.m_alphaCutoffs[0] == cutoff &&
					proxy.m_baseColorSamplers[0] == sampler && proxy.m_renderQueueTags[0] == queue,
					"ordinary mesh and terrain must preserve the same material depth inputs");
				if (queue == "Transparent"_h.GetHash())
				{
					Require(!proxy.m_shadowCaster, "transparent mesh and terrain must not publish shadow meshes");
					continue;
				}
				Require(proxy.m_shadowCaster && proxy.m_shadowCaster->m_meshes.Num() == 1,
					"opaque and masked producers must publish their shadow mesh");
				const auto& shadow = proxy.m_shadowCaster->m_meshes[0];
				const bool bMasked = queue == "Masked"_h.GetHash();
				Require(shadow.m_renderQueueTag == queue &&
					shadow.m_baseColorFactor == (bMasked ? color : glm::vec4(1)) &&
					shadow.m_alphaCutoff == (bMasked ? cutoff : 0.5f) &&
					shadow.m_baseColorSampler == (bMasked ? sampler : 0u),
					"mesh and terrain shadow inputs must follow the same masked/opaque semantics");
				Require(static_cast<bool>(shadow.m_customDepthMaterial) == bCustom &&
					static_cast<bool>(shadow.m_customDepthShader) == bCustom &&
					(!bCustom || (shadow.m_customDepthMaterial == proxy.m_overrideMaterials[0] &&
						shadow.m_customDepthShader == expectedShader)),
					"custom depth must retain the producer's RHIPtr and shader");
#if defined(__APPLE__)
				TVector<uint32_t> expected{ 0u };
				if (bCustom) { expected.Add(baseIndex); expected.Add(normalIndex); }
				else if (bMasked) expected.Add(sampler);
				RHI::NormalizeTextureSamplers(expected);
				Require(shadow.m_materialTextureSamplers == expected,
					"Apple shadows must retain every custom-shader texture, or just the masked sampler");
#endif
			}
			const auto& group = resources[2]->m_proxy.m_instancedGroups[0];
			Require(group.m_baseColorFactors[0] == color && group.m_alphaCutoffs[0] == cutoff &&
				group.m_baseColorSamplers[0] == sampler && group.m_renderQueueTags[0] == queue &&
				group.m_sourceMaterialShaders[0] == expectedShader &&
				group.m_materials[0]->GetRenderState().IsRequiredCustomDepthShader() == bCustom,
				"instanced vegetation must retain the same depth inputs and custom shader as regular meshes");
#if defined(__APPLE__)
			TVector<uint32_t> expected{ 0u, baseIndex, normalIndex };
			RHI::NormalizeTextureSamplers(expected);
			Require(resources[0]->m_proxy.m_materialTextureSamplers[0] == expected &&
				resources[1]->m_proxy.m_materialTextureSamplers[0] == expected &&
				group.m_materialTextureSamplers[0] == expected,
				"all three producers must retain the complete Apple texture dependency set");
#endif
		};

		step();
		const auto original = capture();
		const glm::vec4 originalColor(0.25f, 0.5f, 0.75f, 0.625f);
		verifyMetadata(original, originalColor, 0.375f, baseIndex, "Masked"_h.GetHash(), bCustomDepth, originalShader);
		const auto materialRevision = material->GetRenderMetadataRevision();
		material->SetUniform("material.roughnessFactor"_h, 0.625f);
		material->SetUniform("material.baseColorFactor"_h, glm::vec4(0.125f, 0.25f, 0.5f, originalColor.a));
		material->UpdateRHIResourceAndUniforms();
		Drain();
		step();
		Require(capture() == original && material->GetRenderMetadataRevision() == materialRevision,
			"RGB and roughness edits must not rebuild depth/shadow snapshots or instance topology");

		const glm::vec4 aliasColor(0.75f, 0.25f, 0.5f, 0.375f);
		material->ClearUniforms();
		material->SetUniform("material.albedo"_h, aliasColor);
		material->SetUniform("material.alphaCutoff"_h, 0.125f);
		material->ClearSamplers();
		material->SetSampler("albedoSampler"_h, textures->GetLoadedTexture(normal.m_id));
		material->SetSampler("normalSampler"_h, textures->GetLoadedTexture(baseColor.m_id));
		material->UpdateRHIResourceAndUniforms();
		Drain();
		step();
		const auto aliased = capture();
		verifyMetadata(aliased, aliasColor, 0.125f, normalIndex, "Masked"_h.GetHash(), bCustomDepth, originalShader);
		verifyMetadata(original, originalColor, 0.375f, baseIndex, "Masked"_h.GetHash(), bCustomDepth, originalShader);
		step();
		Require(capture() == aliased, "unchanged material metadata must reuse all three producer resources");

		const glm::vec4 reloadedColor(0.625f, 0.375f, 0.125f, 0.75f);
		source.m_uniformsVec4["material.baseColorFactor"] = reloadedColor;
		source.m_uniformsFloat["material.alphaCutoff"] = 0.25f;
		source.m_shaderDefines.Add("CLEAR_COAT");
		source.m_renderState = RHI::RenderState(true, true, 0, !bCustomDepth);
		Require(importer->CreateMaterialAsset(path, source) == id, "reload must preserve material asset identity");
		importer->OnUpdateAssetInfo(App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr(id), true);
		Drain();
		step();
		const auto reloaded = capture();
		const auto reloadedShader = material->GetShader();
		Require(reloadedShader != originalShader, "hot reload must exercise a different real shader permutation");
		verifyMetadata(reloaded, reloadedColor, 0.25f, baseIndex, "Masked"_h.GetHash(), !bCustomDepth, reloadedShader);
		verifyMetadata(aliased, aliasColor, 0.125f, normalIndex, "Masked"_h.GetHash(), bCustomDepth, originalShader);
		for (const auto queue : { "Opaque"_h, "Transparent"_h })
		{
			material->SetRenderState(RHI::RenderState(true, true, 0, !bCustomDepth, RHI::ECullMode::Back,
				RHI::EBlendMode::None, RHI::EFillMode::Fill, queue.GetHash()));
			material->UpdateRHIResourceAndUniforms();
			Drain();
			step();
			verifyMetadata(capture(), reloadedColor, 0.25f, baseIndex, queue.GetHash(), !bCustomDepth, reloadedShader);
		}
		verifyMetadata(reloaded, reloadedColor, 0.25f, baseIndex, "Masked"_h.GetHash(), !bCustomDepth, reloadedShader);
		std::cout << "Shared material metadata: mesh/terrain/vegetation, aliases, hot reload, retained snapshots and Apple dependencies passed; custom="
			<< bCustomDepth << '\n';
	}

	void TestVegetationMaterialMetadata(MaterialPtr material, ELandscapeVegetationResidency residency)
	{
		LandscapeWorld world(material);
		world.AddCamera();
		auto model = CreateVegetationModel(world);
		UploadVegetationModel(model);
		Drain();
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 2;
		profile.m_settings.m_residency = residency;
		data.m_vegetationProfiles.Add(std::move(profile));
		world.Step();
		Drain();
		Require(data.m_chunks[0].m_vegetationProxies.Num() == 1 && data.m_chunks[1].m_vegetationProxies.Num() == 1 &&
			(residency != ELandscapeVegetationResidency::Grass || data.m_activeGrassInstances == 4),
			"the metadata fixture must have resident vegetation on both visible chunks");
		const auto scene = world.Scene();
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		const std::array meshes{ data.m_chunks[0].m_resource->m_proxy.m_meshes[0], data.m_chunks[1].m_resource->m_proxy.m_meshes[0] };
		const std::array previous{ data.m_chunks[0].m_vegetationProxies[0], data.m_chunks[1].m_vegetationProxies[0] };
		const float cutoff = residency == ELandscapeVegetationResidency::Grass ? 0.675f : 0.425f;
		material->SetUniform("material.alphaCutoff"_h, cutoff);
		world.StepWithMaterialCommands();
		Require(data.m_buildRevision == revision && data.m_physicsBodies == bodies && world.Scene() != scene,
			"vegetation metadata must publish without terrain or collision rebuilding");
		for (size_t index = 0; index < data.m_chunks.Num(); ++index)
		{
			const auto& chunk = data.m_chunks[index];
			Require(chunk.m_resource->m_proxy.m_meshes[0] == meshes[index] && chunk.m_vegetationProxies.Num() == 1,
				"metadata refresh must retain the terrain mesh and exactly one vegetation group");
			const auto& current = chunk.m_vegetationProxies[0];
			const auto& group = current.m_resource->m_proxy.m_instancedGroups[0];
			const auto& oldGroup = previous[index].m_resource->m_proxy.m_instancedGroups[0];
			Require(current.m_resource != previous[index].m_resource && current.m_viewRevision == previous[index].m_viewRevision &&
				current.m_instanceCount == previous[index].m_instanceCount && group.m_alphaCutoffs[0] == cutoff &&
				oldGroup.m_alphaCutoffs[0] != cutoff && group.m_instanceTransforms == oldGroup.m_instanceTransforms &&
				group.m_instanceLodBiases == oldGroup.m_instanceLodBiases &&
				group.m_instanceCullDistanceScales == oldGroup.m_instanceCullDistanceScales &&
				group.m_instanceShadowDistanceScales == oldGroup.m_instanceShadowDistanceScales,
				"persistent and grass metadata refresh must preserve placements, overrides, residency and retained resources");
			CheckPublishedReplacement(scene, world.Scene(), previous[index].m_resource, current.m_resource);
		}
		const auto readyScene = world.Scene();
		world.Step();
		Require(world.Scene() == readyScene, "an unchanged vegetation material must not rebuild its render proxies");
		CheckGpuGeometry(data);
		std::cout << "Landscape vegetation material: residency=" << static_cast<int>(residency)
			<< "; metadata, placements and stable terrain/collision passed\n";
	}

	void TestVegetationTransform(const MaterialPtr& material, ELandscapeVegetationResidency residency)
	{
		LandscapeWorld world(material);
		auto camera = world.AddCamera();
		auto model = CreateVegetationModel(world);
		UploadVegetationModel(model);
		Drain();
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 2;
		profile.m_settings.m_residency = residency;
		profile.m_settings.m_colliderRadius = 0.2f;
		data.m_vegetationProfiles.Add(profile);
		world.Step();
		Drain();
		Require(data.m_chunks[0].m_vegetationProxies.Num() == 1 && data.m_chunks[1].m_vegetationProxies.Num() == 1 &&
			data.m_physicsBodies.Num() == (residency == ELandscapeVegetationResidency::Persistent ? 4 : 2),
			"the transform fixture must start with both resident groups and persistent vegetation colliders");
		const auto revision = data.m_buildRevision;
		const std::array terrain{ data.m_chunks[0].m_resource, data.m_chunks[1].m_resource };
		const std::array vegetation{ data.m_chunks[0].m_vegetationProxies[0], data.m_chunks[1].m_vegetationProxies[0] };
		const auto rotation = glm::angleAxis(0.4f, glm::vec3(0, 1, 0));
		for (const auto scale : { glm::vec3(1), glm::vec3(2, 1.5f, 0.75f), glm::vec3(-2, 1.5f, 0.75f) })
		{
			const auto scene = world.Scene();
			const auto bodies = data.m_physicsBodies;
			world.Owner()->GetTransformComponent().SetPosition({ 60, 3, 0 });
			world.Owner()->GetTransformComponent().SetRotation(rotation);
			world.Owner()->GetTransformComponent().SetScale(glm::vec4(scale, 1));
			const auto matrix = Math::Transform(glm::vec4(60, 3, 0, 1), rotation, glm::vec4(scale, 1)).Matrix();
			const glm::vec3 cameraPosition(matrix * glm::vec4(0, 8, 12, 1));
			camera->GetTransformComponent().SetPosition(cameraPosition);
			camera->GetTransformComponent().SetRotation(glm::quatLookAtRH(
				glm::normalize(glm::vec3(matrix[3]) - cameraPosition), glm::vec3(0, 1, 0)));
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			Require(data.m_buildRevision == revision && data.m_physicsBodies.Num() == bodies.Num() &&
				(scale == glm::vec3(1) ? data.m_physicsBodies == bodies : data.m_physicsBodies != bodies) &&
				(residency != ELandscapeVegetationResidency::Grass || data.m_activeGrassInstances == 4),
				"moving vegetation must retain terrain, collider count and grass residency at the current world position");
			CheckTransformPublication(scene, world.Scene(), matrix);
			Require(CountVisible(world.Scene(), glm::vec3(matrix[3])) == 4,
				"current spatial culling must include both moved chunks and their resident vegetation");
			for (size_t index = 0; index < data.m_chunks.Num(); ++index)
			{
				const auto& chunk = data.m_chunks[index];
				Require(chunk.m_resource == terrain[index] && chunk.m_vegetationProxies.Num() == 1,
					"moving vegetation must not rebuild terrain or duplicate resident groups");
				const auto& current = chunk.m_vegetationProxies[0];
				Require(current.m_resource == vegetation[index].m_resource && current.m_revision == vegetation[index].m_revision &&
					current.m_viewRevision == vegetation[index].m_viewRevision &&
					current.m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms.GetData() ==
					vegetation[index].m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms.GetData(),
					"transform-only updates must share resident topology and instance storage, not clone or regenerate them");
				if (residency != ELandscapeVegetationResidency::Persistent) continue;
				const auto& placement = chunk.m_bakeVegetation[0].m_transform;
				const float instanceScale = glm::length(glm::vec3(placement[1]));
				const glm::vec3 center(matrix * placement * glm::vec4(0, profile.m_settings.m_colliderOffsetY, 0, 1));
				const float halfHeight = std::max(profile.m_settings.m_colliderHeight * instanceScale * std::abs(scale.y) * 0.5f,
					profile.m_settings.m_colliderRadius * instanceScale * std::max(std::abs(scale.x), std::abs(scale.z)));
				const glm::vec3 top = center + glm::vec3(0, halfHeight, 0);
				Physics::PhysicsRaycastHit hit;
				Require(world.GetECS<PhysicsECS>()->Raycast(top + glm::vec3(0, 10, 0), { 0, -1, 0 }, 20, hit) &&
					hit.m_instanceId == world.Owner()->GetInstanceId() && glm::length(hit.m_position - top) < 0.001f,
					"persistent vegetation capsules must follow the same translated, rotated and scaled owner as their render instances");
			}
		}
		std::cout << "Landscape vegetation transform: residency=" << static_cast<int>(residency)
			<< "; shared instance storage, current culling and Jolt capsules passed\n";
	}

	void TestPendingVegetationMaterial(const MaterialPtr& material, const MaterialPtr& replacement,
		ELandscapeVegetationResidency residency, bool bEvict)
	{
		LandscapeWorld world(material);
		auto camera = world.AddCamera();
		auto model = CreateVegetationModel(world);
		UploadVegetationModel(model);
		Drain();
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 2;
		profile.m_settings.m_residency = residency;
		data.m_vegetationProfiles.Add(std::move(profile));
		world.Step();
		Drain();
		Require(data.m_chunks[0].m_vegetationProxies.Num() == 1 && data.m_chunks[1].m_vegetationProxies.Num() == 1,
			"the pending-material fixture must start with resident vegetation");
		const auto scene = world.Scene();
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		const std::array terrain{ data.m_chunks[0].m_resource, data.m_chunks[1].m_resource };
		const std::array previous{ data.m_chunks[0].m_vegetationProxies[0], data.m_chunks[1].m_vegetationProxies[0] };
		auto pending = world.CreateMaterialInstance(replacement);
		Drain();
		Tests::RequirePendingMeshUpload([&]()
		{
			pending->SetUniform("material.alphaCutoff"_h, 0.875f);
			pending->UpdateRHIResourceAndUniforms();
		}, [&]()
		{
			Require(!pending->IsReady(), "the replacement material must wait on its real transfer fence");
			data.m_vegetationProfiles[0].m_settings.m_materialFileId = pending->GetFileId();
			data.m_vegetationProfiles[0].m_material = pending;
			for (size_t frame = 0; frame < 3; ++frame)
			{
				world.Step();
				Require(!pending->IsReady() && data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
					world.Scene() == scene, "pending material frames must retain terrain, collision and scene publication");
				for (size_t index = 0; index < data.m_chunks.Num(); ++index)
					Require(data.m_chunks[index].m_resource == terrain[index] &&
						data.m_chunks[index].m_vegetationProxies.Num() == 1 &&
						data.m_chunks[index].m_vegetationProxies[0].m_resource == previous[index].m_resource,
						"pending metadata must retain each current vegetation resource without duplicate proxies");
			}
			world.Owner()->GetTransformComponent().SetPosition({ 60, 3, 0 });
			camera->GetTransformComponent().SetPosition({ 60, 11, 12 });
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			CheckTransformPublication(scene, world.Scene(), world.Owner()->GetTransformComponent().GetCachedWorldMatrix());
			const auto movedScene = world.Scene();
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			Require(!pending->IsReady() && world.Scene() == movedScene && data.m_physicsBodies == bodies,
				"pending material retries must keep the moved scene stable and retain its collision bodies");
			if (bEvict)
			{
				camera->GetTransformComponent().SetPosition({ 10000, 8, 12 });
				world.Step();
				Require(data.m_activeGrassInstances == 0 && data.m_pendingVegetation.IsEmpty() &&
					data.m_chunks[0].m_vegetationProxies.IsEmpty() && data.m_chunks[1].m_vegetationProxies.IsEmpty(),
					"the grass scheduler must evict residents while their replacement material is pending");
			}
		});
		Drain();
		Require(pending->IsReady() && ReadMaterialUniform<float>(pending, "alphaCutoff"_h) == 0.875f,
			"the completed replacement must contain the uploaded GPU uniform");
		const auto waitingScene = world.Scene();
		world.Step();
		if (bEvict)
		{
			Require(world.Scene() == waitingScene && data.m_activeGrassInstances == 0 &&
				data.m_chunks[0].m_vegetationProxies.IsEmpty() && data.m_chunks[1].m_vegetationProxies.IsEmpty(),
				"a completed material must not resurrect evicted grass");
			camera->GetTransformComponent().SetPosition({ 60, 11, 12 });
			world.Step();
		}
		Require(world.Scene() != waitingScene && data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
			data.m_pendingVegetation.IsEmpty(), "material completion must publish vegetation without rebuilding terrain or collision");
		for (size_t index = 0; index < data.m_chunks.Num(); ++index)
		{
			const auto& chunk = data.m_chunks[index];
			Require(chunk.m_resource == terrain[index] && chunk.m_vegetationProxies.Num() == 1,
				"the completed metadata must retain terrain and exactly one vegetation group");
			const auto& current = chunk.m_vegetationProxies[0];
			const auto& group = current.m_resource->m_proxy.m_instancedGroups[0];
			const auto& oldGroup = previous[index].m_resource->m_proxy.m_instancedGroups[0];
			Require(current.m_resource != previous[index].m_resource && group.m_alphaCutoffs[0] == 0.875f &&
				oldGroup.m_alphaCutoffs[0] != 0.875f && group.m_instanceTransforms == oldGroup.m_instanceTransforms &&
				group.m_instanceLodBiases == oldGroup.m_instanceLodBiases &&
				group.m_instanceCullDistanceScales == oldGroup.m_instanceCullDistanceScales &&
				group.m_instanceShadowDistanceScales == oldGroup.m_instanceShadowDistanceScales,
				"replacement or re-streamed vegetation must use new metadata and preserve placements and overrides");
			if (!bEvict) CheckPublishedReplacement(scene, world.Scene(), previous[index].m_resource, current.m_resource);
		}
		const auto readyScene = world.Scene();
		world.Step();
		Require(world.Scene() == readyScene, "material completion must publish once");
		CheckGpuGeometry(data);
		world.CheckLocalHit({ -3.75f, 0, 0.125f });
		std::cout << "Landscape pending material: residency=" << static_cast<int>(residency)
			<< "; eviction=" << bEvict << "; retained residents, GPU data and one-shot publication passed\n";
	}

	void TestPendingCpuImages(const MaterialPtr& material, const std::filesystem::path& workspace)
	{
		Tests::TextureFixture fixture(workspace, "LandscapeCpuHeight");
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		const auto terrain = data.m_chunks[0].m_resource;
		Tests::DecodeProbe decode(fixture.m_id, true);
		data.SetSettings(2, 1, 8, 4, 2, 0.1f, 1337, 1);
		data.SetImportMaps(fixture.m_id, { fixture.m_id });
		for (size_t frame = 0; frame < 3; ++frame)
		{
			world.Step();
			Require(data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
				data.m_chunks[0].m_resource == terrain,
				"pending CPU images must preserve published terrain and collision without a synchronous rebuild");
		}
		decode.WaitDecoded(1);
		decode.CheckWorker(1);
		decode.Release();
		Drain();
		world.Step();
		Drain();
		Require(data.m_buildRevision == revision + 2 && data.m_chunks[0].m_heightSamples[0] == 2,
			"one shared height/mask decode must publish both chunks with the actual imported height");
		decode.CheckWorker(1);
		world.CheckHit(-4, 2);
		const auto unchanged = data.m_chunks[1].m_resource;
		const std::array unchangedBodies = { data.m_chunks[1].m_terrainBodyId, data.m_chunks[1].m_vegetationBodyId };
		data.SetAuthoredStamps({ { -4, 0, 1, 1, ELandscapeSculptOperation::Raise } }, {});
		world.Step();
		Drain();
		Require(data.m_buildRevision == revision + 3 && data.m_chunks[1].m_resource == unchanged &&
			std::array{ data.m_chunks[1].m_terrainBodyId, data.m_chunks[1].m_vegetationBodyId } == unchangedBodies,
			"brush edits must reuse decoded pixels and rebuild only the affected chunk");
		decode.CheckWorker(1);
		CheckGpuGeometry(data, glm::vec4(0.5f, 0, 0, 0.5f));
		fixture.Write(0, 255);
		data.SetAuthoredStamps({ { -4, 0, 1, 2, ELandscapeSculptOperation::Raise } }, {});
		world.Step();
		Drain();
		world.Step();
		Drain();
		Require(data.m_buildRevision == revision + 5 && data.m_chunks[1].m_heightSamples[0] == -2,
			"a changed heightmap revision during a brush edit must rebuild untouched neighbors too");
		decode.CheckWorker(2);
		world.CheckHit(4, -2);
		CheckGpuGeometry(data, glm::vec4(0, 0, 0.5f, 0.5f));
		fixture.Write(128, 0);
		auto metadata = fixture.m_info->Serialize();
		metadata["format"] = RHI::ETextureFormat::R32G32B32A32_SFLOAT;
		fixture.m_info->Deserialize(metadata);
		data.RequestFullRebuild();
		world.Step();
		Drain();
		world.Step();
		Drain();
		const float linearRed = std::pow(128.0f / 255.0f, 2.2f);
		Require(std::abs(data.m_chunks[1].m_heightSamples[0] - (linearRed * 2 - 1) * 2) < 0.0001f,
			"float CPU snapshots must be sampled as floats, not guessed from unrelated texture byte sizes");
		decode.CheckWorker(3);
		CheckGpuGeometry(data, glm::vec4(linearRed, 0, 0, 1) / (linearRed + 1));
		std::cout << "Landscape CPU images: three pending frames, shared decode, brush reuse and source revision passed\n";
	}

	void TestIndependentCpuMaps(const MaterialPtr& material, const std::filesystem::path& workspace)
	{
		Tests::TextureFixture height(workspace, "LandscapeIndependentHeight");
		Tests::TextureFixture mask(workspace, "LandscapeIndependentMask");
		mask.Write(128, 0);
		LandscapeWorld world(material);
		Tests::DecodeProbe decode(height.m_id);
		auto& data = world.Data();
		data.SetSettings(2, 1, 8, 4, 2, 0.1f, 1337, 1);
		data.SetImportMaps(height.m_id, { height.m_id, mask.m_id });
		world.Step();
		Drain();
		world.Step();
		Drain();
		CheckGpuGeometry(data, glm::vec4(255.0f, 128.0f, 0, 0) / 383.0f);
		decode.CheckWorker(1);
		decode.CheckOtherDecoded(1);
		const auto revision = data.m_buildRevision;
		mask.Write(0, 255);
		data.SetAuthoredStamps({ { -4, 0, 1, 1, ELandscapeSculptOperation::Raise } }, {});
		world.Step();
		Drain();
		world.Step();
		Drain();
		Require(data.m_buildRevision == revision + 2 && data.m_chunks[1].m_heightSamples[0] == 2,
			"an independently changed mask must refresh every chunk without changing the cached height source");
		decode.CheckWorker(1);
		decode.CheckOtherDecoded(2);
		CheckGpuGeometry(data, glm::vec4(1, 0, 0, 0));
		world.CheckHit(4, 2);
		std::cout << "Landscape CPU masks: independent revisions, full color refresh and shared height source passed\n";
	}

	enum class PendingCpuImageEdit { RemoveMaps, ClearWorld, SourceRevision };

	void TestPendingCpuImageEdit(const MaterialPtr& material, const std::filesystem::path& workspace, PendingCpuImageEdit edit)
	{
		const char* names[] = { "LandscapeRemovedCpuMaps", "LandscapeClearedCpuWorld", "LandscapeChangedCpuSource" };
		Tests::TextureFixture fixture(workspace, names[static_cast<size_t>(edit)]);
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		const auto revision = data.m_buildRevision;
		Tests::DecodeProbe decode(fixture.m_id, true);
		data.SetSettings(2, 1, 8, 4, 2, 0.1f, 1337, 1);
		data.SetImportMaps(fixture.m_id, {});
		world.Step();
		decode.WaitDecoded(1);
		if (edit == PendingCpuImageEdit::RemoveMaps)
		{
			data.SetImportMaps({}, {});
			world.Step();
			Require(!data.IsDirty() && data.m_buildRevision == revision + 2,
				"removing import maps must not wait for an obsolete decode or retain its pending handles");
		}
		if (edit == PendingCpuImageEdit::SourceRevision)
		{
			fixture.Write(0, 255);
		}
		else
		{
			world.Clear();
		}
		decode.Release();
		Drain();
		if (edit == PendingCpuImageEdit::SourceRevision)
		{
			world.Step();
			Drain();
			world.Step();
			Drain();
			Require(data.m_buildRevision == revision + 2 && data.m_chunks[1].m_heightSamples[0] == -2,
				"a source changed during decoding must publish only the latest image, not an intermediate fallback terrain");
			decode.CheckWorker(2);
			CheckGpuGeometry(data);
		}
		else
		{
			decode.CheckWorker(1);
		}
		std::cout << "Landscape CPU image edit: " << static_cast<int>(edit) << "; pending owner/request lifetime passed\n";
	}

	void RequireNonblockingTicks(LandscapeWorld& world, const std::function<void()>& release)
	{
		std::promise<void> finished;
		std::atomic<bool> bBlocked{ false };
		std::jthread watchdog([&, done = finished.get_future()]() mutable
		{
			if (done.wait_for(std::chrono::seconds(3)) != std::future_status::ready)
			{
				bBlocked = true;
				release();
			}
		});
		for (size_t frame = 0; frame < 3; ++frame) world.StepWithMaterialCommands(false);
		finished.set_value();
		watchdog.join();
		Require(!bBlocked, "Landscape Tick must not wait for a pending resource task");
	}

	FileId WriteLandscapeModel(const std::filesystem::path& workspace, std::string_view name,
		FileId material, FileId id = FileId::CreateNewFileId())
	{
		const auto path = (workspace / "Content" / name).concat(".gltf");
		const std::array<float, 24> vertices{
			-0.25f, 0, 0, 0.25f, 0, 0, 0, 1, 0,
			0, 0, 1, 0, 0, 1, 0, 0, 1,
			0, 0, 1, 0, 0.5f, 1 };
		const std::array<uint16_t, 3> indices{ 0, 1, 2 };
		std::ofstream buffer((workspace / "Content" / name).concat(".bin"), std::ios::binary);
		buffer.write(reinterpret_cast<const char*>(vertices.data()), sizeof(vertices));
		buffer.write(reinterpret_cast<const char*>(indices.data()), sizeof(indices));
		buffer.close();
		std::ofstream model(path);
		model << R"({"asset":{"version":"2.0"},"buffers":[{"uri":")" << name << R"(.bin","byteLength":102}],
	"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":36},
		{"buffer":0,"byteOffset":72,"byteLength":24},{"buffer":0,"byteOffset":96,"byteLength":6}],
	"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
		{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},
		{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},
		{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],
	"materials":[{},{}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":1}]}],
	"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
		model.close();
		ModelAssetInfo defaults;
		defaults.GetDefaultMaterials() = { FileId{}, material };
		auto metadata = defaults.Serialize();
		metadata["fileId"] = id;
		metadata["filename"] = path.filename().string();
		metadata["bShouldGenerateMaterials"] = false;
		metadata["bGenerateLods"] = false;
		metadata["bGenerateBLAS"] = false;
		metadata["bShouldKeepCpuBuffers"] = true;
		std::ofstream sidecar(path.string() + ".asset");
		sidecar << metadata;
		sidecar.close();
		Require(buffer && model && sidecar, "the async Landscape glTF fixture must be written");
		Require(App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string()) == id,
			"the async Landscape model must register");
		return id;
	}

	class ModelLoadGate final
	{
	public:
		ModelLoadGate()
		{
			Drain();
			ModelImporterTestAccess::BeforeCpuPreparation(*App::GetSubmodule<ModelImporter>(), [this]()
			{
				++m_calls;
				m_release.wait();
			});
		}
		~ModelLoadGate()
		{
			Release();
			Drain();
			ModelImporterTestAccess::BeforeCpuPreparation(*App::GetSubmodule<ModelImporter>(), {});
		}
		void Release() { if (!m_bIsReleased.exchange(true)) m_release.count_down(); }
		void CheckCalls(uint32_t count)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (m_calls < count && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
			Require(m_calls == count, "each selected model must start one CPU preparation request");
		}

	private:
		std::atomic<uint32_t> m_calls{ 0 };
		std::atomic<bool> m_bIsReleased{ false };
		std::latch m_release{ 1 };
	};

	enum class PendingResourceEdit { None, Replace, Remove, ClearWorld };

	void TestAsyncVegetationModel(const MaterialPtr& material, const std::filesystem::path& workspace, PendingResourceEdit edit)
	{
		const auto id = WriteLandscapeModel(workspace, "LandscapeAsyncModel" + std::to_string(static_cast<int>(edit)), material->GetFileId());
		LandscapeWorld world(material);
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = id;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_settings.m_instancesPerChunk = 2;
		data.m_vegetationProfiles.Add(profile);
		ModelLoadGate gate;
		RequireNonblockingTicks(world, [&]() { gate.Release(); });
		gate.CheckCalls(1);
		const auto load = data.m_vegetationProfiles[0].m_modelLoad;
		Require(load && !load->IsFinished() && !data.m_vegetationProfiles[0].m_model &&
			data.m_buildRevision == 2 && data.m_pendingVegetation.Num() == 2,
			"a pending model must not expose partial geometry or delay the first terrain publication");
		const auto bodies = data.m_physicsBodies;
		const auto terrain = data.m_chunks[0].m_resource;
		const auto placements = data.m_pendingVegetation[0].m_instances.m_transforms.GetData();
		RequireNonblockingTicks(world, [&]() { gate.Release(); });
		Require(data.m_buildRevision == 2 && data.m_physicsBodies == bodies && data.m_chunks[0].m_resource == terrain &&
			data.m_vegetationProfiles[0].m_modelLoad == load &&
			data.m_pendingVegetation[0].m_instances.m_transforms.GetData() == placements,
			"pending model checks must reuse the request, placements, terrain and collision");
		if (edit == PendingResourceEdit::ClearWorld) world.Clear();
		else if (edit == PendingResourceEdit::Remove)
		{
			data.m_vegetationProfiles.Clear();
			data.RequestFullRebuild();
			world.Step();
		}
		gate.Release();
		Drain();
		Require(load->GetResult() && load->GetResult()->IsReady(), "an importer request must finish independently of its Landscape owner");
		if (edit == PendingResourceEdit::ClearWorld) return;
		world.Step();
		if (edit == PendingResourceEdit::Remove)
		{
			Require(data.m_vegetationProfiles.IsEmpty() && data.m_pendingVegetation.IsEmpty() &&
				data.m_chunks[0].m_vegetationProxies.IsEmpty(), "a removed profile must not receive a late model completion");
			return;
		}
		Require(data.m_buildRevision == 2 && data.m_physicsBodies == bodies && data.m_chunks[0].m_resource == terrain &&
			data.m_pendingVegetation.IsEmpty() && data.m_chunks[0].m_vegetationProxies.Num() == 1 &&
			data.m_vegetationProfiles[0].m_model == load->GetResult(),
			"model readiness must publish vegetation once without rebuilding ready terrain");
		const auto scene = world.Scene();
		world.Step();
		Require(world.Scene() == scene, "a completed model must not republish unchanged vegetation");
		CheckGpuGeometry(data);
		world.CheckHit(-4, 0);
		std::cout << "Landscape async model: one Worker request, retained terrain and one-shot vegetation publication passed\n";
	}

	void TestAsyncVegetationMaterial(const MaterialPtr& terrainMaterial, const std::filesystem::path& workspace,
		bool bDefaultMaterials, PendingResourceEdit edit)
	{
		const std::string name = "LandscapeAsyncMaterial" + std::to_string(bDefaultMaterials) + std::to_string(static_cast<int>(edit));
		Tests::TextureFixture texture(workspace, name);
		MaterialAsset::Data source;
		source.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
		source.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(1);
		source.m_samplers["baseColorSampler"] = texture.m_id;
		const auto materialId = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(
			(workspace / "Content" / (name + ".mat")).string(), std::move(source));
		const auto modelId = WriteLandscapeModel(workspace, name, materialId);
		ModelPtr model;
		Require(App::GetSubmodule<ModelImporter>()->LoadModel_Immediate(modelId, model), "the material fixture model must load");
		Drain();
		LandscapeWorld world(terrainMaterial);
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = modelId;
		profile.m_model = model;
		profile.m_settings.m_materialFileId = bDefaultMaterials ? FileId{} : materialId;
		profile.m_settings.m_instancesPerChunk = 2;
		data.m_vegetationProfiles.Add(profile);
		Tests::DecodeProbe decode(texture.m_id, true);
		RequireNonblockingTicks(world, [&]() { decode.Release(); });
		decode.WaitDecoded(1);
		Require(data.m_buildRevision == 2 && data.m_pendingVegetation.Num() == 2 &&
			!data.m_vegetationProfiles[0].m_material && data.m_vegetationProfiles[0].m_modelMaterials.IsEmpty(),
			"pending material tasks must not expose partially initialized materials or rebuild terrain");
		const auto bodies = data.m_physicsBodies;
		const auto terrain = data.m_chunks[0].m_resource;
		if (edit == PendingResourceEdit::ClearWorld) world.Clear();
		else if (edit == PendingResourceEdit::Remove)
		{
			data.m_vegetationProfiles.Clear();
			data.RequestFullRebuild();
			world.Step();
		}
		decode.Release();
		Drain();
		if (edit == PendingResourceEdit::ClearWorld) return;
		world.Step();
		if (edit == PendingResourceEdit::Remove)
		{
			Require(data.m_pendingVegetation.IsEmpty() && data.m_chunks[0].m_vegetationProxies.IsEmpty(),
				"removed vegetation must not receive a late material completion");
			return;
		}
		Require(data.m_buildRevision == 2 && data.m_physicsBodies == bodies && data.m_chunks[0].m_resource == terrain &&
			data.m_pendingVegetation.IsEmpty() && data.m_chunks[0].m_vegetationProxies.Num() == 1,
			"material readiness must publish vegetation without rebuilding the ready terrain");
		const auto& ready = data.m_vegetationProfiles[0];
		if (bDefaultMaterials)
			Require(ready.m_modelMaterials.Num() == 2 && !ready.m_modelMaterials[0],
				"publishing default materials must preserve an empty neighboring material slot");
		const auto loaded = bDefaultMaterials ? ready.m_modelMaterials[1] : ready.m_material;
		Require(loaded && loaded->GetFileId() == materialId, "the selected material must finish loading");
		Require(ReadMaterialUniform<uint32_t>(loaded, "baseColorSampler"_h) ==
			App::GetSubmodule<TextureImporter>()->GetTextureIndex(texture.m_id),
			"the vegetation material must publish the actual loaded GPU sampler");
		decode.CheckWorker(1);
		const auto scene = world.Scene();
		world.Step();
		Require(world.Scene() == scene, "a completed material must publish once");
		if (bDefaultMaterials)
		{
			auto* info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(modelId);
			info->GetDefaultMaterials().Clear();
			data.RequestVegetationAssetReload();
			world.StepWithMaterialCommands();
			world.StepWithMaterialCommands();
			Require(data.m_vegetationProfiles[0].m_bAreModelMaterialsPublished &&
				data.m_vegetationProfiles[0].m_modelMaterials.IsEmpty(),
				"an empty default-material result must replace the previous published slots");
		}
		std::cout << "Landscape async material: default slots=" << bDefaultMaterials
			<< "; coherent publication, GPU sampler and retained terrain passed\n";
	}

	MaterialPtr TestTerrainLayerTextures(const std::filesystem::path& workspace)
	{
		const auto path = workspace / "Content" / "LandscapeLayer.tga";
		std::array<uint8_t, 21> pixels{};
		pixels[2] = 2;
		pixels[12] = pixels[14] = 1;
		pixels[16] = 24;
		pixels[20] = 255;
		std::ofstream output(path, std::ios::binary);
		output.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
		output.close();
		Require(static_cast<bool>(output), "the terrain layer fixture must be written");
		const auto textureId = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string());
		auto* textureImporter = App::GetSubmodule<TextureImporter>();
		MaterialAsset::Data source;
		source.m_shader = FileId("545C0F94-89A0-4D44-9894-852C7AD24BDB");
		source.m_uniformsVec4["material.albedo"] = glm::vec4(1);
		source.m_uniformsVec4["material.layerUvScale"] = glm::vec4(1);
		auto* importer = App::GetSubmodule<MaterialImporter>();
		const auto materialId = importer->CreateMaterialAsset((workspace / "Content" / "LandscapeLayers.mat").string(), std::move(source));
		MaterialPtr material;
		Require(importer->LoadMaterial_Immediate(materialId, material), "the real Landscape shader fixture must load");
		Drain();
		LandscapeWorld world(material);
		auto& data = world.Data();
		data.SetSettings(2, 1, 8, 4, 2, 0.1f, 1337, 1);
		data.SetImportMaps(textureId, {});
		world.Step();
		Drain();
		world.Step();
		Drain();
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		const auto mesh = data.m_chunks[0].m_resource->m_proxy.m_meshes[0];
		const auto triangles = data.m_chunks[0].m_bakeTriangles;
		const auto heights = data.m_chunks[0].m_heightSamples;
		Require(heights[0] > 0, "the real heightmap must contribute nonzero terrain height");
		const auto oldScene = world.Scene();
		Tests::DecodeProbe decode(textureId, true);
		data.SetLayerTextures({ textureId });
		RequireNonblockingTicks(world, [&]() { decode.Release(); });
		decode.WaitDecoded(1);
		const auto request = data.m_layerTextureLoads[0];
		const glm::vec4 pendingColor(0.25f, 0.5f, 0.75f, 1);
		material->SetUniform("material.albedo"_h, pendingColor);
		RequireNonblockingTicks(world, [&]() { decode.Release(); });
		Require(data.m_buildRevision == revision && data.m_physicsBodies == bodies && world.Scene() == oldScene,
			"pending layer textures must retain the published terrain and collision for three ticks");
		Require(data.m_layerTextureLoads[0] == request, "material edits must reuse the pending layer request");
		decode.Release();
		Drain();
		world.StepWithMaterialCommands();
		TexturePtr sampler;
		Require(data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
			data.m_chunks[0].m_resource->m_proxy.m_meshes[0] == mesh && data.m_chunks[0].m_bakeTriangles == triangles &&
			data.m_chunks[0].m_heightSamples == heights &&
			data.m_runtimeMaterial->GetSamplers().TryGet("layer0Sampler"_h, sampler) && sampler && sampler->GetFileId() == textureId &&
			ReadMaterialUniform<uint32_t>(data.m_runtimeMaterial, "layer0Sampler"_h) == textureImporter->GetTextureIndex(textureId),
			"adding a layer must update the real GPU sampler index without rebuilding imported terrain or collision");
		Require(material->GetSamplers().IsEmpty(), "private terrain layers must not change their source material");
		Require(ReadMaterialUniform<glm::vec4>(data.m_runtimeMaterial, "albedo"_h) == pendingColor,
			"layer completion must use the latest source uniforms, not a snapshot from before loading");
		decode.CheckWorker(1);
		data.SetLayerTextures({});
		world.StepWithMaterialCommands();
		Require(data.m_buildRevision == revision && data.m_physicsBodies == bodies &&
			data.m_chunks[0].m_resource->m_proxy.m_meshes[0] == mesh && data.m_chunks[0].m_bakeTriangles == triangles &&
			ReadMaterialUniform<uint32_t>(data.m_runtimeMaterial, "layer0Sampler"_h) == 0,
			"removing a layer must restore the GPU fallback without rebuilding terrain");
		world.CheckHit(-4, heights[0]);
		CheckGpuGeometry(data);
		std::cout << "Landscape layers: real heightmap, GPU sampler add/remove and shared geometry/collision passed\n";
		return material;
	}

	void TestPendingLayerEdit(const MaterialPtr& material, const std::filesystem::path& workspace, PendingResourceEdit edit)
	{
		const auto name = "LandscapeLayerEdit" + std::to_string(static_cast<int>(edit));
		Tests::TextureFixture first(workspace, name);
		Tests::TextureFixture replacement(workspace, name + "Replacement");
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		const auto bodies = data.m_physicsBodies;
		const auto mesh = data.m_chunks[0].m_resource->m_proxy.m_meshes[0];
		Tests::DecodeProbe decode(first.m_id, true);
		data.SetLayerTextures({ first.m_id });
		RequireNonblockingTicks(world, [&]() { decode.Release(); });
		decode.WaitDecoded(1);
		if (edit == PendingResourceEdit::ClearWorld) world.Clear();
		else
		{
			data.SetLayerTextures(edit == PendingResourceEdit::Remove ? TVector<FileId>{} : TVector<FileId>{ replacement.m_id });
			RequireNonblockingTicks(world, [&]() { decode.Release(); });
			if (edit == PendingResourceEdit::Replace) decode.WaitOtherDecoded();
		}
		decode.Release();
		Drain();
		if (edit == PendingResourceEdit::ClearWorld) return;
		world.StepWithMaterialCommands();
		TexturePtr sampler;
		const bool bHasSampler = data.m_runtimeMaterial->GetSamplers().TryGet("layer0Sampler"_h, sampler);
		Require(edit == PendingResourceEdit::Remove ? !bHasSampler : bHasSampler && sampler->GetFileId() == replacement.m_id,
			"a superseded layer completion must not restore a removed or replaced sampler");
		Require(data.m_buildRevision == 2 && data.m_physicsBodies == bodies && data.m_chunks[0].m_resource->m_proxy.m_meshes[0] == mesh,
			"pending layer replacement/removal must retain terrain meshes and collision");
		Require(ReadMaterialUniform<uint32_t>(data.m_runtimeMaterial, "layer0Sampler"_h) ==
			(edit == PendingResourceEdit::Remove ? 0 : App::GetSubmodule<TextureImporter>()->GetTextureIndex(replacement.m_id)),
			"the GPU sampler must match the latest layer selection");
		decode.CheckWorker(1);
	}

	void TestFailedModelRetry(const MaterialPtr& material, const std::filesystem::path& workspace)
	{
		LandscapeWorld world(material);
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = FileId::CreateNewFileId();
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_settings.m_instancesPerChunk = 2;
		data.m_vegetationProfiles.Add(profile);
		world.Step();
		Drain();
		const auto failed = data.m_vegetationProfiles[0].m_modelLoad;
		Require(failed && failed->IsFinished() && !failed->GetResult(), "a missing model must retain a completed failed request");
		for (size_t frame = 0; frame < 3; ++frame) world.Step();
		Require(data.m_vegetationProfiles[0].m_modelLoad == failed && data.m_buildRevision == 2,
			"a missing model must not restart loading or rebuild terrain each frame");
		WriteLandscapeModel(workspace, "LandscapeRepairedModel", material->GetFileId(), profile.m_settings.m_modelFileId);
		data.RequestVegetationAssetReload();
		world.Step();
		Drain();
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		world.Step();
		Require(data.m_vegetationProfiles[0].m_modelLoad != failed && data.m_vegetationProfiles[0].m_model &&
			data.m_pendingVegetation.IsEmpty() && data.m_chunks[0].m_vegetationProxies.Num() == 1 &&
			data.m_buildRevision == revision && data.m_physicsBodies == bodies,
			"explicit regeneration must retry a repaired model and publish without another terrain rebuild");
	}

	void TestPendingModelSelection(const MaterialPtr& material, const std::filesystem::path& workspace)
	{
		const auto first = WriteLandscapeModel(workspace, "LandscapeModelSelectionFirst", material->GetFileId());
		const auto replacement = WriteLandscapeModel(workspace, "LandscapeModelSelectionReplacement", material->GetFileId());
		LandscapeWorld world(material);
		auto& data = world.Data();
		auto select = [&](FileId model, uint32_t count)
		{
			LandscapeVegetationSettings settings;
			settings.m_modelFileId = model;
			settings.m_materialFileId = material->GetFileId();
			settings.m_instancesPerChunk = count;
			data.SetVegetationProfiles({ settings });
		};
		select(first, 2);
		ModelLoadGate gate;
		RequireNonblockingTicks(world, [&]() { gate.Release(); });
		gate.CheckCalls(1);
		const auto firstLoad = data.m_vegetationProfiles[0].m_modelLoad;
		select(first, 3);
		RequireNonblockingTicks(world, [&]() { gate.Release(); });
		Require(data.m_vegetationProfiles[0].m_modelLoad == firstLoad && !firstLoad->IsFinished(),
			"authored placement changes must keep the selected pending model request");
		select(replacement, 3);
		RequireNonblockingTicks(world, [&]() { gate.Release(); });
		gate.CheckCalls(2);
		const auto replacementLoad = data.m_vegetationProfiles[0].m_modelLoad;
		Require(replacementLoad != firstLoad && !replacementLoad->IsFinished() && !data.m_vegetationProfiles[0].m_model,
			"changing the model selection must replace its request without exposing partial geometry");
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		const auto terrain = data.m_chunks[0].m_resource;
		gate.Release();
		Drain();
		world.Step();
		Require(firstLoad->GetResult()->GetFileId() == first && replacementLoad->GetResult()->GetFileId() == replacement &&
			data.m_vegetationProfiles[0].m_model == replacementLoad->GetResult() &&
			data.m_chunks[0].m_vegetationProxies.Num() == 1 && data.m_pendingVegetation.IsEmpty() &&
			data.m_buildRevision == revision && data.m_physicsBodies == bodies && data.m_chunks[0].m_resource == terrain,
			"only the latest model selection may publish, without another readiness terrain rebuild");
		CheckGpuGeometry(data);
	}

	void TestProfileRecordReorder(const MaterialPtr& material, MaterialPtr replacement,
		const std::filesystem::path& workspace, bool bRemoveWhilePending)
	{
		const auto suffix = std::to_string(bRemoveWhilePending);
		const auto first = WriteLandscapeModel(workspace, "LandscapeRecordFirst" + suffix, material->GetFileId());
		const auto second = WriteLandscapeModel(workspace, "LandscapeRecordSecond" + suffix, material->GetFileId());
		LandscapeWorld world(material);
		world.AddCamera();
		auto& data = world.Data();
		LandscapeVegetationSettings firstSettings;
		firstSettings.m_modelFileId = first;
		firstSettings.m_instancesPerChunk = 2;
		LandscapeVegetationSettings secondSettings;
		secondSettings.m_modelFileId = second;
		secondSettings.m_materialFileId = replacement->GetFileId();
		secondSettings.m_instancesPerChunk = 3;
		secondSettings.m_groundOffset = 1;
		secondSettings.m_residency = ELandscapeVegetationResidency::Grass;
		TVector<LandscapeVegetationSettings> settings{ firstSettings, secondSettings };
		data.SetVegetationProfiles(settings);
		ModelLoadGate gate;
		RequireNonblockingTicks(world, [&]() { gate.Release(); });
		gate.CheckCalls(2);
		const auto firstLoad = data.m_vegetationProfiles[0].m_modelLoad;
		const auto secondLoad = data.m_vegetationProfiles[1].m_modelLoad;
		const auto revision = data.m_buildRevision;
		const auto terrain = data.m_chunks[0].m_resource;
		const auto bodies = data.m_physicsBodies;
		std::swap(settings[0], settings[1]);
		data.SetVegetationProfiles(settings);
		RequireNonblockingTicks(world, [&]() { gate.Release(); });
		Require(data.m_vegetationProfiles[0].m_modelLoad == secondLoad &&
			data.m_vegetationProfiles[1].m_modelLoad == firstLoad && !firstLoad->IsFinished() && !secondLoad->IsFinished(),
			"record reorder must reuse each pending model request by identity, not its old array index");
		if (bRemoveWhilePending)
		{
			settings.Resize(1);
			data.SetVegetationProfiles(settings);
			RequireNonblockingTicks(world, [&]() { gate.Release(); });
			Require(data.m_vegetationProfiles[0].m_modelLoad == secondLoad,
				"removing a pending neighbor must retain the selected record's resource request");
		}
		gate.CheckCalls(2);
		gate.Release();
		Drain();
		world.StepWithMaterialCommands();
		world.StepWithMaterialCommands();
		const auto firstModel = firstLoad->GetResult();
		const auto secondModel = secondLoad->GetResult();
		auto proxy = [&](uint32_t profileIndex) -> const LandscapeVegetationRenderProxy&
		{
			const auto& proxies = data.m_chunks[0].m_vegetationProxies;
			const size_t index = proxies.FindIf([&](const auto& value) { return value.m_profileIndex == profileIndex; });
			Require(index != size_t(-1), "the selected record must have a resident render proxy");
			return proxies[index];
		};
		Require(firstModel && secondModel && data.m_vegetationProfiles[0].m_model == secondModel &&
			data.m_chunks[0].m_vegetationProxies.Num() == settings.Num() && data.m_pendingVegetation.IsEmpty(),
			"late completions must publish only the currently selected records");
		Require(proxy(0).m_instanceCount == 3 && data.m_activeGrassInstances == 6 &&
			proxy(0).m_resource->m_proxy.m_instancedGroups[0].m_meshes[0] == secondModel->GetMeshes()[0],
			"reordered fields must select their own mesh and instance count");
		if (!bRemoveWhilePending)
		{
			const auto defaults = data.m_vegetationProfiles[1].m_modelMaterials;
			const auto defaultLoads = data.m_vegetationProfiles[1].m_modelMaterialsLoad;
			Require(defaults.Num() == 2 && !defaults[0] && defaults[1] == material,
				"the first model must publish its actual default-material slots");
			std::swap(settings[0], settings[1]);
			data.SetVegetationProfiles(settings);
			Require(data.m_vegetationProfiles[0].m_model == firstModel && data.m_vegetationProfiles[1].m_model == secondModel &&
				data.m_vegetationProfiles[0].m_modelMaterials == defaults &&
				data.m_vegetationProfiles[0].m_modelMaterialsLoad == defaultLoads &&
				data.m_vegetationProfiles[0].m_bAreModelMaterialsPublished,
				"ready record reorder must retain models, default materials and their completed requests");
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			const auto selected = proxy(0);
			const auto neighbor = proxy(1).m_resource;
			settings[0].m_materialFileId = replacement->GetFileId();
			data.SetVegetationProfiles(settings);
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			const auto& edited = proxy(0);
			const auto& group = edited.m_resource->m_proxy.m_instancedGroups[0];
			Require(data.m_vegetationProfiles[0].m_material == replacement && edited.m_revision == selected.m_revision &&
				group.m_instanceTransforms == selected.m_resource->m_proxy.m_instancedGroups[0].m_instanceTransforms &&
				group.m_materials[0] == replacement->GetOrAddRHI(firstModel->GetMeshes()[0]->m_vertexDescription) &&
				proxy(1).m_resource == neighbor && data.m_activeGrassInstances == 6,
				"editing a record's material must retain placements and neighboring grass residents while publishing the selected RHI material");
			settings.RemoveAt(0);
			data.SetVegetationProfiles(settings);
			Tests::RequireNoTransferSubmission([&]() { world.Step(); });
			Require(data.m_vegetationProfiles[0].m_model == secondModel && data.m_vegetationProfiles[0].m_modelLoad == secondLoad &&
				data.m_chunks[0].m_vegetationProxies.Num() == 1 && data.m_chunks[0].m_vegetationProxies[0].m_instanceCount == 3,
				"removing the first ready record must keep the shifted record's model, request and authored count");
		}
		gate.CheckCalls(2);
		Require(data.m_buildRevision == revision && data.m_chunks[0].m_resource == terrain && data.m_physicsBodies == bodies,
			"record reorder, removal and material edits must never rebuild the terrain or its bodies");
		const auto* profileStorage = data.m_vegetationProfiles.GetData();
		const auto scene = world.Scene();
		data.SetVegetationProfiles(settings);
		Require(!data.IsDirty() && data.m_vegetationProfiles.GetData() == profileStorage,
			"unchanged authoring records must keep the runtime profiles without dirtying the Landscape");
		Tests::RequireNoTransferSubmission([&]() { world.Step(); });
		Require(world.Scene() == scene, "an unrelated component update must not republish vegetation");
		CheckGpuGeometry(data);
		std::cout << "Landscape records: pending/ready reorder, removal and retained resources passed; pending removal="
			<< bRemoveWhilePending << '\n';
	}

	void TestTypedBrushStamps(const MaterialPtr& material)
	{
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		const auto neighbor = data.m_chunks[1].m_resource;
		const auto neighborBody = data.m_chunks[1].m_terrainBodyId;
		for (auto operation : { ELandscapeSculptOperation::Raise, ELandscapeSculptOperation::Lower })
		{
			data.SetAuthoredStamps({ { -4, 0, 1, 3, operation } }, {});
			world.Step();
			Drain();
			world.CheckHit(-4, operation == ELandscapeSculptOperation::Raise ? 3.0f : -3.0f);
			CheckGpuGeometry(data);
		}
		const TVector<LandscapeSculptStamp> sculpt{
			{ -4, 0, 1, 3, ELandscapeSculptOperation::Raise }, { -4, 0, 1, 1, ELandscapeSculptOperation::Flatten } };
		data.SetAuthoredStamps(sculpt, {});
		world.Step();
		Drain();
		world.CheckHit(-4, 0);
		for (uint32_t layer = 0; layer < 4; ++layer)
		{
			data.SetAuthoredStamps(sculpt, { { -4, 0, 1, 1, layer } });
			world.Step();
			Drain();
			const auto mesh = data.m_chunks[0].m_resource->m_proxy.m_meshes[0];
			auto readback = Tasks::CreateTaskWithResult<glm::vec4>("Read painted terrain layer"_h, [mesh, &data]()
			{
				auto& driver = RHI::Renderer::GetDriver();
				const uint32_t row = data.m_chunkResolution + 1;
				const size_t size = row * row * sizeof(RHI::VertexP3N3T3B3UV2C4);
				auto vertices = driver->CreateBuffer(size, RHI::EBufferUsageBit::BufferTransferDst_Bit,
					RHI::EMemoryPropertyBit::HostVisible | RHI::EMemoryPropertyBit::HostCoherent);
				Require(driver->CopyBuffer_Immediate(mesh->m_vertexBuffer, vertices, size), "painted vertices must be readable");
				return static_cast<const RHI::VertexP3N3T3B3UV2C4*>(vertices->GetPointer())[(row / 2) * row + row / 2].m_color;
			}, EThreadType::RHI);
			readback->Run();
			readback->Wait();
			glm::vec4 expected(0);
			expected[layer] = 1;
			Require(readback->GetResult() == expected, "typed paint layers must select the corresponding GPU splat weight");
		}
		Require(data.m_chunks[1].m_resource == neighbor && data.m_chunks[1].m_terrainBodyId == neighborBody,
			"typed brush records must dirty their affected chunk without rebuilding an untouched neighbor");
		CheckGpuGeometry(data);
		std::cout << "Landscape typed sculpt operations and four GPU paint layers passed\n";
	}

	void TestFailedLayerRetry(const MaterialPtr& material, const std::filesystem::path& workspace)
	{
		Tests::TextureFixture texture(workspace, "LandscapeFailedLayer");
		std::ofstream(texture.m_path, std::ios::trunc).close();
		LandscapeWorld world(material);
		world.Step();
		Drain();
		auto& data = world.Data();
		Tests::DecodeProbe decode(texture.m_id);
		data.SetLayerTextures({ texture.m_id });
		world.StepWithMaterialCommands();
		world.StepWithMaterialCommands();
		const auto failed = data.m_layerTextureLoads[0];
		Require(failed && failed->IsFinished() && !failed->GetResult(), "an invalid layer must retain its failed request");
		for (size_t frame = 0; frame < 3; ++frame) world.Step();
		Require(data.m_layerTextureLoads[0] == failed && data.m_buildRevision == 2 &&
			ReadMaterialUniform<uint32_t>(data.m_runtimeMaterial, "layer0Sampler"_h) == 0,
			"failed layers must use the fallback without repeated loading or terrain rebuilds");
		decode.CheckWorker(1);
		texture.Write(64, 128);
		data.RequestVegetationAssetReload();
		world.StepWithMaterialCommands();
		world.StepWithMaterialCommands();
		Require(data.m_layerTextureLoads[0] != failed && data.m_layerTextureLoads[0]->GetResult() &&
			ReadMaterialUniform<uint32_t>(data.m_runtimeMaterial, "layer0Sampler"_h) ==
			App::GetSubmodule<TextureImporter>()->GetTextureIndex(texture.m_id),
			"explicit regeneration must replace a failed layer request with the repaired GPU texture");
		decode.CheckWorker(2);
	}

	void TestFailedMaterialRetry(const MaterialPtr& terrainMaterial, const std::filesystem::path& workspace,
		bool bDefaultMaterials)
	{
		const auto name = "LandscapeRepairedMaterial" + std::to_string(bDefaultMaterials);
		const auto materialId = FileId::CreateNewFileId();
		const auto modelId = WriteLandscapeModel(workspace, name, materialId);
		ModelPtr model;
		Require(App::GetSubmodule<ModelImporter>()->LoadModel_Immediate(modelId, model), "the material retry model must load");
		Drain();
		LandscapeWorld world(terrainMaterial);
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = modelId;
		profile.m_model = model;
		profile.m_settings.m_materialFileId = bDefaultMaterials ? FileId{} : materialId;
		profile.m_settings.m_instancesPerChunk = 2;
		data.m_vegetationProfiles.Add(profile);
		world.StepWithMaterialCommands();
		world.StepWithMaterialCommands();
		const auto failedOverride = data.m_vegetationProfiles[0].m_materialLoad;
		const auto failedDefaults = data.m_vegetationProfiles[0].m_modelMaterialsLoad;
		if (bDefaultMaterials)
		{
			Require(failedDefaults && failedDefaults->IsFinished() &&
				data.m_vegetationProfiles[0].m_modelMaterials.Num() == 2 &&
				!data.m_vegetationProfiles[0].m_modelMaterials[1], "a missing default material must retain its empty slot");
		}
		else
		{
			Require(failedOverride && failedOverride->IsFinished() && !failedOverride->GetResult(),
				"a missing override material must retain a completed failed request");
		}
		for (size_t frame = 0; frame < 3; ++frame) world.Step();
		Require(data.m_vegetationProfiles[0].m_materialLoad == failedOverride &&
			data.m_vegetationProfiles[0].m_modelMaterialsLoad == failedDefaults && data.m_buildRevision == 2,
			"unavailable materials must not restart requests or rebuild terrain each frame");

		const auto path = workspace / "Content" / (name + ".mat");
		MaterialAsset::Data source;
		source.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
		std::ofstream materialFile(path);
		materialFile << MaterialAsset::Serialize(source);
		materialFile.close();
		MaterialAssetInfo defaults;
		auto metadata = defaults.Serialize();
		metadata["fileId"] = materialId;
		metadata["filename"] = path.filename().string();
		std::ofstream sidecar(path.string() + ".asset");
		sidecar << metadata;
		sidecar.close();
		Require(materialFile && sidecar && App::GetSubmodule<AssetRegistry>()->GetOrLoadFile(path.string()) == materialId,
			"the repaired material must register under the originally selected identifier");
		data.RequestVegetationAssetReload();
		world.StepWithMaterialCommands();
		const auto revision = data.m_buildRevision;
		const auto bodies = data.m_physicsBodies;
		world.StepWithMaterialCommands();
		const auto& ready = data.m_vegetationProfiles[0];
		if (bDefaultMaterials) Require(ready.m_modelMaterials.Num() == 2, "material retry must keep the model's authored slots");
		const auto loaded = bDefaultMaterials ? ready.m_modelMaterials[1] : ready.m_material;
		Require(loaded && loaded->IsReady() && loaded->GetFileId() == materialId &&
			data.m_pendingVegetation.IsEmpty() && data.m_chunks[0].m_vegetationProxies.Num() == 1 &&
			data.m_buildRevision == revision && data.m_physicsBodies == bodies,
			"explicit regeneration must retry a repaired material without another readiness terrain rebuild");
		Require(bDefaultMaterials ? ready.m_modelMaterialsLoad != failedDefaults : ready.m_materialLoad != failedOverride,
			"the explicit material retry must replace its failed request");
	}

	enum class PendingVegetationEdit { None, Sculpt, RemoveProfile, Material, Transform };

	void TestPendingVegetation(const MaterialPtr& material, PendingVegetationEdit edit)
	{
		LandscapeWorld world(material);
		auto model = CreateVegetationModel(world);
		auto& data = world.Data();
		LandscapeVegetationProfile profile;
		profile.m_settings.m_modelFileId = model->GetFileId();
		profile.m_model = model;
		profile.m_settings.m_materialFileId = material->GetFileId();
		profile.m_material = material;
		profile.m_settings.m_instancesPerChunk = 2;
		data.m_vegetationProfiles.Add(std::move(profile));
		auto& authored = data.m_vegetationAssetData;
		authored.m_chunksX = 2;
		authored.m_chunksZ = 1;
		authored.m_chunkSize = 8;
		authored.m_profileCount = 1;
		for (uint32_t x = 0; x < 2; ++x)
		{
			LandscapeVegetationChunkData chunk;
			chunk.m_chunkX = x;
			for (uint32_t index = 0; index < 2; ++index)
			{
				LandscapeVegetationInstance instance;
				instance.m_stableId = x * 2 + index + 1;
				instance.m_transform[3] = glm::vec4(-6.0f + x * 8.0f + index * 2.0f, 0, 0, 1);
				instance.m_lodBias = static_cast<int32_t>(index) - 1;
				instance.m_cullDistanceScale = 0.5f + index;
				instance.m_shadowDistanceScale = 0.25f + index;
				chunk.m_instances.Add(std::move(instance));
			}
			authored.m_chunks.Add(std::move(chunk));
		}
		std::string diagnostic;
		Require(authored.Validate(diagnostic), "the pending vegetation fixture must use valid authored placements");
		authored.RebuildRuntimeIndices();
		data.m_bVegetationAssetLoaded = true;
		std::array<RHI::RHISceneProxyResourcePtr, 2> terrain;
		TVector<uint32_t> bodies;
		uint64_t terrainRevision = 0;
		RHI::RHISpatialSceneVersionPtr waitingScene;
		Tests::RequirePendingMeshUpload([&]() { UploadVegetationModel(model); }, [&]()
		{
			world.Step();
			Require(data.m_chunks.Num() == 2 && data.m_physicsBodies.Num() == 2 && !model->IsReady(),
				"terrain and collision must build while the real vegetation upload is pending");
			terrain = { data.m_chunks[0].m_resource, data.m_chunks[1].m_resource };
			bodies = data.m_physicsBodies;
			terrainRevision = data.m_buildRevision;
			const auto scene = world.Scene();
			for (uint32_t frame = 0; frame < 3; ++frame)
			{
				world.Step();
				Require(!model->IsReady() && data.m_chunks[0].m_resource == terrain[0] &&
					data.m_chunks[1].m_resource == terrain[1] && data.m_buildRevision == terrainRevision &&
					data.m_physicsBodies == bodies && world.Scene() == scene,
					"three pending vegetation frames must preserve terrain meshes, colliders and scene identity");
				Require(data.m_chunks[0].m_vegetationProxies.IsEmpty() && data.m_chunks[1].m_vegetationProxies.IsEmpty(),
					"pending vegetation must not publish incomplete render proxies");
				world.CheckHit(-4, 0);
			}
			if (edit == PendingVegetationEdit::Transform)
			{
				const auto* placements = data.m_pendingVegetation[0].m_instances.m_transforms.GetData();
				world.Owner()->GetTransformComponent().SetPosition({ 60, 3, 0 });
				world.Owner()->GetTransformComponent().SetRotation(glm::angleAxis(0.4f, glm::vec3(0, 1, 0)));
				Tests::RequireNoTransferSubmission([&]() { world.Step(); });
				Require(data.m_buildRevision == terrainRevision && data.m_physicsBodies == bodies &&
					data.m_chunks[0].m_resource == terrain[0] && data.m_chunks[1].m_resource == terrain[1] &&
					data.m_pendingVegetation[0].m_instances.m_transforms.GetData() == placements,
					"moving a pending owner must retain terrain, bodies and the original pending placement storage");
				CheckTransformPublication(scene, world.Scene(), world.Owner()->GetTransformComponent().GetCachedWorldMatrix());
				world.CheckLocalHit({ -3.75f, 0, 0.125f });
			}
			else if (edit == PendingVegetationEdit::Sculpt)
			{
				const std::array neighborBodies = { data.m_chunks[1].m_terrainBodyId, data.m_chunks[1].m_vegetationBodyId };
				data.SetAuthoredStamps({ { -4, 0, 0.5f, 1, ELandscapeSculptOperation::Raise } }, {});
				authored.m_chunks[0].m_instances[1].m_transform[3].y = 1;
				authored.m_chunks[0].m_instances[1].m_lodBias = 2;
				Tests::RequireMainMeshUploadRefusal([&]() { world.Step(); }, VK_ERROR_OUT_OF_DEVICE_MEMORY, 0);
				Require(world.Scene() == scene && data.m_buildRevision == terrainRevision &&
					data.m_physicsBodies == bodies && data.m_chunks[0].m_resource == terrain[0] &&
					data.m_chunks[1].m_resource == terrain[1],
					"a refused sculpt must retain terrain, collision and pending vegetation together");
				world.Step();
				Require(data.m_buildRevision == terrainRevision + 1 && data.m_chunks[0].m_resource != terrain[0] &&
					data.m_chunks[1].m_resource == terrain[1] &&
					std::array{ data.m_chunks[1].m_terrainBodyId, data.m_chunks[1].m_vegetationBodyId } == neighborBodies,
					"sculpting pending vegetation must replace only the edited chunk and its placements");
				world.CheckHit(-4, 1);
			}
			else if (edit == PendingVegetationEdit::RemoveProfile)
			{
				data.m_vegetationProfiles.Clear();
				data.RequestFullRebuild();
				world.Step();
				Require(data.m_buildRevision == terrainRevision + 2 && data.m_chunks[0].m_resource != terrain[0] &&
					data.m_chunks[1].m_resource != terrain[1],
					"removing a pending profile must accept a complete replacement terrain");
			}
			else if (edit == PendingVegetationEdit::Material)
			{
				auto source = material;
				source->SetUniform("material.alphaCutoff"_h, 0.275f);
				world.StepWithMaterialCommands();
				Require(data.m_buildRevision == terrainRevision && data.m_physicsBodies == bodies &&
					data.m_chunks[0].m_resource->m_proxy.m_meshes[0] == terrain[0]->m_proxy.m_meshes[0] &&
					data.m_chunks[1].m_resource->m_proxy.m_meshes[0] == terrain[1]->m_proxy.m_meshes[0],
					"a material edit during vegetation loading must retain both terrain meshes and colliders");
			}
			terrain = { data.m_chunks[0].m_resource, data.m_chunks[1].m_resource };
			bodies = data.m_physicsBodies;
			terrainRevision = data.m_buildRevision;
			waitingScene = world.Scene();
		});
		Drain();
		Require(model->IsReady(), "the vegetation model must complete its real native upload");
		world.Step();
		Require(data.m_chunks[0].m_resource == terrain[0] && data.m_chunks[1].m_resource == terrain[1] &&
			data.m_buildRevision == terrainRevision && data.m_physicsBodies == bodies,
			"completed vegetation must publish without replacing terrain or colliders");
		for (const auto& chunk : data.m_chunks)
		{
			if (edit == PendingVegetationEdit::RemoveProfile)
			{
				Require(chunk.m_vegetationProxies.IsEmpty(),
					"finishing a removed profile's upload must not publish obsolete vegetation");
				continue;
			}
			Require(chunk.m_vegetationProxies.Num() == 1 && chunk.m_vegetationProxies[0].m_instanceCount == 2,
				"completed vegetation must publish the requested instances on every chunk");
			const auto& group = chunk.m_vegetationProxies[0].m_resource->m_proxy.m_instancedGroups[0];
			if (edit == PendingVegetationEdit::Material)
			{
				Require(group.m_alphaCutoffs[0] == 0.275f, "pending vegetation must publish the latest material metadata");
			}
			for (size_t index = 0; index < chunk.m_bakeVegetation.Num(); ++index)
			{
				const auto& instance = authored.m_chunks[chunk.m_chunkX].m_instances[index];
				Require(group.m_instanceTransforms[index] == instance.m_transform &&
					group.m_instanceTransforms[index] == chunk.m_bakeVegetation[index].m_transform &&
					group.m_instanceLodBiases[index] == instance.m_lodBias &&
					group.m_instanceCullDistanceScales[index] == instance.m_cullDistanceScale &&
					group.m_instanceShadowDistanceScales[index] == instance.m_shadowDistanceScale,
					"delayed vegetation must retain authored matrices, LOD biases and culling/shadow overrides");
			}
		}
		CheckGpuGeometry(data);
		const auto readyScene = world.Scene();
		if (edit == PendingVegetationEdit::Transform)
		{
			const auto matrix = world.Owner()->GetTransformComponent().GetCachedWorldMatrix();
			Require(CountVisible(readyScene, glm::vec3(0)) == 0 && CountVisible(readyScene, glm::vec3(matrix[3])) == 4,
				"completed vegetation must appear at the current owner position, not its position when loading started");
			for (auto handle : *readyScene->m_sceneVersion->m_staticHandles)
			{
				const RHI::RHISceneInstanceRecord* record = nullptr;
				Require(readyScene->m_sceneVersion->Resolve(handle, record), "completed vegetation handles must resolve");
				CheckMatrix(record->m_worldMatrix, matrix);
			}
		}
		if (edit == PendingVegetationEdit::RemoveProfile)
		{
			Require(readyScene == waitingScene && readyScene->m_sceneVersion->m_staticHandles->Num() == 2,
				"removed vegetation must not invalidate the published scene when its old upload completes");
		}
		else
		{
			Require(readyScene != waitingScene && waitingScene->m_sceneVersion->m_staticHandles->Num() == 2 &&
				readyScene->m_sceneVersion->m_staticHandles->Num() == 4 &&
				readyScene->m_shadowCastersRevision > waitingScene->m_shadowCastersRevision &&
				readyScene->m_sceneVersion->m_staticRevision > waitingScene->m_sceneVersion->m_staticRevision,
				"vegetation completion must publish new static geometry and shadows without changing retained scenes");
		}
		world.Step();
		Require(world.Scene() == readyScene, "completed vegetation must stop retrying on unchanged ticks");
		std::cout << "Landscape vegetation: three pending frames retain terrain/collision; edit=" << static_cast<int>(edit)
			<< "; accepted placements and overrides publish once\n";
	}
}

void Sailor::Tests::RunLandscapeCommandTests(const std::filesystem::path& workspace)
{
	MaterialAsset::Data source;
	source.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
	source.m_uniformsVec4["material.baseColorFactor"] = glm::vec4(1);
	auto* importer = App::GetSubmodule<MaterialImporter>();
	const auto id = importer->CreateMaterialAsset((workspace / "Content" / "LandscapeUpload.mat").string(), std::move(source));
	MaterialPtr material;
	Require(id && importer->LoadMaterial_Immediate(id, material), "the native terrain fixture material must load");
	Drain();
	Require(material->IsReady(), "the source terrain material must be ready");
	for (uint32_t count : { 32u, 64u })
		for (bool deferred : { false, true })
			TestRemovalPublication(material, count, deferred);
	TestSharedMaterialMetadata(workspace, false);
	TestSharedMaterialMetadata(workspace, true);
	TestPendingCpuImages(material, workspace);
	TestIndependentCpuMaps(material, workspace);
	for (auto edit : { PendingCpuImageEdit::RemoveMaps, PendingCpuImageEdit::ClearWorld, PendingCpuImageEdit::SourceRevision })
		TestPendingCpuImageEdit(material, workspace, edit);
	for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
	{
		TestFullReplacement(material, error);
		TestColdUpload(material, error);
		TestPartialReplacement(material, error);
	}
	TestPendingUpload(material);
	TestTerrainTransform(material);
	TestTypedBrushStamps(material);
	TestVegetationProfileEdits(material);
	TestGrassPriorityAndBudget(material);
	TestVegetationSourceReload(material, workspace);
	TestVegetationEditDuringSculptRetry(material);
	for (auto edit : { PendingVegetationEdit::None, PendingVegetationEdit::Sculpt, PendingVegetationEdit::RemoveProfile,
		PendingVegetationEdit::Material, PendingVegetationEdit::Transform })
		TestPendingVegetation(material, edit);
	TestVegetationTransform(material, ELandscapeVegetationResidency::Persistent);
	TestVegetationTransform(material, ELandscapeVegetationResidency::Grass);
	TestVegetationMaterialMetadata(material, ELandscapeVegetationResidency::Persistent);
	TestVegetationMaterialMetadata(material, ELandscapeVegetationResidency::Grass);
	MaterialAsset::Data replacementSource;
	replacementSource.m_shader = FileId("1A4BA353-FDA4-4F65-941F-D9FFEE4630A0");
	const auto replacementId = importer->CreateMaterialAsset((workspace / "Content" / "LandscapeReplacement.mat").string(), std::move(replacementSource));
	MaterialPtr replacement;
	Require(importer->LoadMaterial_Immediate(replacementId, replacement), "the replacement terrain material must load");
	Drain();
	TestProfileRecordReorder(material, replacement, workspace, false);
	TestProfileRecordReorder(material, replacement, workspace, true);
	TestPendingVegetationMaterial(material, replacement, ELandscapeVegetationResidency::Persistent, false);
	TestPendingVegetationMaterial(material, replacement, ELandscapeVegetationResidency::Grass, false);
	TestPendingVegetationMaterial(material, replacement, ELandscapeVegetationResidency::Grass, true);
	TestTerrainMaterialMetadata(material, replacement);
	for (auto edit : { PendingResourceEdit::None, PendingResourceEdit::Remove, PendingResourceEdit::ClearWorld })
	{
		TestAsyncVegetationModel(material, workspace, edit);
		TestAsyncVegetationMaterial(material, workspace, false, edit);
		TestAsyncVegetationMaterial(material, workspace, true, edit);
	}
	TestFailedModelRetry(material, workspace);
	TestPendingModelSelection(material, workspace);
	TestFailedMaterialRetry(material, workspace, false);
	TestFailedMaterialRetry(material, workspace, true);
	const auto layeredMaterial = TestTerrainLayerTextures(workspace);
	for (auto edit : { PendingResourceEdit::Replace, PendingResourceEdit::Remove, PendingResourceEdit::ClearWorld })
		TestPendingLayerEdit(layeredMaterial, workspace, edit);
	TestFailedLayerRetry(layeredMaterial, workspace);
	std::cout << "Landscape native full/partial/cold upload refusal, retained terrain/collision, pending fence and unchanged-input LOD recovery passed\n";
}
