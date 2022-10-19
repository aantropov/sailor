#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "Components/TestComponent.h"
#include "Components/MeshRendererComponent.h"
#include "Components/CameraComponent.h"
#include "Components/LightComponent.h"
#include "Engine/GameObject.h"
#include "Engine/EngineLoop.h"
#include "ECS/TransformECS.h"
#include "glm/glm/gtc/random.hpp"

using namespace Sailor;
using namespace Sailor::Tasks;

void TestComponent::BeginPlay()
{
	GetWorld()->GetDebugContext()->DrawOrigin(glm::vec4(600, 2, 0, 0), 20.0f, 1000.0f);

	for (int32_t i = -1000; i < 1000; i += 32)
	{
		for (int32_t j = -1000; j < 1000; j += 32)
		{
			int k = 10;
			//for (int32_t k = -1000; k < 1000; k += 32) 
			{
				m_boxes.Add(Math::AABB(glm::vec3(i, k, j), glm::vec3(1.0f, 1.0f, 1.0f)));
				const auto& aabb = m_boxes[m_boxes.Num() - 1];

				GetWorld()->GetDebugContext()->DrawAABB(aabb, glm::vec4(0.2f, 0.8f, 0.2f, 1.0f), 5.0f);

				m_octree.Insert(aabb.GetCenter(), aabb.GetExtents(), aabb);
			}
		}
	}

	/*for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
		{
			auto gameObject2 = GetWorld()->Instantiate();
			gameObject2->GetTransformComponent().SetPosition(vec3(j * 3000, 0, i * 2500));
			gameObject2->AddComponent<MeshRendererComponent>();
		}
		*/

	auto gameObject3 = GetWorld()->Instantiate();
	gameObject3->GetTransformComponent().SetPosition(vec3(0, 0, 0));
	gameObject3->AddComponent<MeshRendererComponent>();

	auto lightGameObject = GetWorld()->Instantiate();
	auto lightComponent = lightGameObject->AddComponent<LightComponent>();
	lightGameObject->GetTransformComponent().SetPosition(vec3(0.0f, 10.0f, 0.0f));
	lightComponent->SetBounds(vec3(40.0f, 40.0f, 40.0f));
	lightComponent->SetIntensity(vec3(10.0f, 100.0f, 100.0f));

	for (int32_t i = -1000; i < 1000; i += 100)
	{
		for (int32_t j = 0; j < 800; j += 100)
		{
			for (int32_t k = -1000; k < 1000; k += 100)
			{
				auto lightGameObject = GetWorld()->Instantiate();
				auto lightComponent = lightGameObject->AddComponent<LightComponent>();

				lightGameObject->GetTransformComponent().SetPosition(vec3(i, j, k));
				lightComponent->SetBounds(vec3(30.0f, 30.0f, 30.0f));
				lightComponent->SetIntensity(vec3(100.0f, 100.0f, 100.0f));

				m_lights.Emplace(std::move(lightGameObject));
				m_lightVelocities.Add(vec4(0));
			}
		}
	}

	//m_octree.DrawOctree(*GetWorld()->GetDebugContext(), 10);
}

void TestComponent::EndPlay()
{
}

void DrawTile(WorldPtr world, const mat4& invViewProjection, const float zFar, const vec3& cameraPosition, const ivec2& tileId, const vec3& lightPos, float radius)
{
	float minDepth = -0.2f;//uintBitsToFloat(maxDepthInt);
	float maxDepth = -0.00001f;//uintBitsToFloat(maxDepthInt);

	const vec2 ndcUpperLeft = vec2(-1.0, -1.0);
	vec2 ndcSizePerTile = 2.0f * vec2(16, 16) / vec2(App::GetViewportWindow()->GetWidth(), App::GetViewportWindow()->GetHeight());

	vec2 ndcCorners[4];
	ndcCorners[0] = ndcUpperLeft + vec2(tileId) * ndcSizePerTile; // upper left
	ndcCorners[1] = vec2(ndcCorners[0].x + ndcSizePerTile.x, ndcCorners[0].y); // upper right
	ndcCorners[2] = ndcCorners[0] + ndcSizePerTile;
	ndcCorners[3] = vec2(ndcCorners[0].x, ndcCorners[0].y + ndcSizePerTile.y); // lower left

	vec3 points[8];
	vec4 planes[8];

	vec4 temp;
	for (int i = 0; i < 4; i++)
	{
		temp = invViewProjection * vec4(ndcCorners[i], minDepth, 1.0);
		points[i] = vec3(temp) / temp.w;

		temp = invViewProjection * vec4(ndcCorners[i], maxDepth, 1.0);
		points[i + 4] = vec3(temp) / temp.w;
	}

	vec3 tempNormal;
	for (int i = 0; i < 4; i++)
	{
		//Cax+Cby+Ccz+Cd = 0, planes[i] = (Ca, Cb, Cc, Cd)
		//tempNormal: normal without normalization
		tempNormal = glm::cross(points[i] - vec3(cameraPosition), points[i + 1] - vec3(cameraPosition));
		tempNormal = normalize(tempNormal);
		planes[i] = vec4(tempNormal, -glm::dot(tempNormal, points[i]));
	}
	// near plane
	{
		tempNormal = glm::cross(points[1] - points[0], points[3] - points[0]);
		tempNormal = normalize(tempNormal);
		planes[4] = vec4(tempNormal, glm::dot(tempNormal, points[0]));
	}
	// far plane
	{
		tempNormal = glm::cross(points[7] - points[4], points[5] - points[4]);
		tempNormal = glm::normalize(tempNormal);
		planes[5] = vec4(tempNormal, glm::dot(tempNormal, points[4]));
	}

	bool bIntersects = true;

	// We check if the light exists in our frustum
	float distance = 0.0;
	for (uint j = 0; j < 4; j++)
	{
		distance = glm::dot(vec4(lightPos, 1), planes[j]) + radius;

		// If one of the tests fails, then there is no intersection
		if (distance <= 0.0)
		{
			bIntersects = false;
			break;
		}
	}

	// If greater than zero, then it is a visible light
	if (distance > 0.0)
	{
		bIntersects = true;
	}

	for (int32_t i = 0; i < 4; i++)
	{
		if (bIntersects)
		{
			world->GetDebugContext()->DrawLine(points[0], points[1], vec4(1, 0, 0, 1), 1000.0f);
			world->GetDebugContext()->DrawLine(points[1], points[2], vec4(1, 0, 0, 1), 1000.0f);
			world->GetDebugContext()->DrawLine(points[2], points[3], vec4(1, 0, 0, 1), 1000.0f);
			world->GetDebugContext()->DrawLine(points[3], points[0], vec4(1, 0, 0, 1), 1000.0f);

			//world->GetDebugContext()->DrawLine(cameraPosition, points[i + 4], bIntersects ? vec4(0, 1, 0, 1) : vec4(0.5f, 0.45f, 0.3f, 1), 1000.0f);
		}
	}
}

void TestComponent::Tick(float deltaTime)
{
	auto& transform = GetOwner()->GetTransformComponent();
	const vec3 cameraViewDirection = transform.GetRotation() * Math::vec4_Forward;

	const float sensitivity = 500;

	glm::vec3 delta = glm::vec3(0.0f, 0.0f, 0.0f);
	if (GetWorld()->GetInput().IsKeyDown('A'))
		delta += -cross(cameraViewDirection, Math::vec3_Up);

	if (GetWorld()->GetInput().IsKeyDown('D'))
		delta += cross(cameraViewDirection, Math::vec3_Up);

	if (GetWorld()->GetInput().IsKeyDown('W'))
		delta += cameraViewDirection;

	if (GetWorld()->GetInput().IsKeyDown('S'))
		delta += -cameraViewDirection;

	if (GetWorld()->GetInput().IsKeyDown('X'))
		delta += vec3(1, 0, 0);

	if (GetWorld()->GetInput().IsKeyDown('Y'))
		delta += vec3(0, 1, 0);

	if (GetWorld()->GetInput().IsKeyDown('Z'))
		delta += vec3(0, 0, 1);

	if (glm::length(delta) > 0)
	{
		const vec4 newPosition = transform.GetPosition() + vec4(glm::normalize(delta) * sensitivity * deltaTime, 1.0f);
		transform.SetPosition(newPosition);
	}

	if (GetWorld()->GetInput().IsKeyDown(VK_LBUTTON))
	{
		const float speed = 1.0f;
		const vec2 shift = vec2(GetWorld()->GetInput().GetCursorPos() - m_lastCursorPos) * speed;

		m_yaw += -shift.x;
		m_pitch = glm::clamp(m_pitch - shift.y, -85.0f, 85.0f);

		glm::quat hRotation = angleAxis(glm::radians(m_yaw), Math::vec3_Up);
		glm::quat vRotation = angleAxis(glm::radians(m_pitch), hRotation * Math::vec3_Right);

		transform.SetRotation(vRotation * hRotation);
	}

	if (GetWorld()->GetInput().IsKeyPressed('F'))
	{
		if (auto camera = GetOwner()->GetComponent<CameraComponent>())
		{
			m_cachedFrustum = transform.GetTransform().Matrix();

			Math::Frustum frustum;
			frustum.ExtractFrustumPlanes(transform.GetTransform(), camera->GetAspect(), camera->GetFov(), camera->GetZNear(), camera->GetZFar());
			m_octree.Trace(frustum, m_culledBoxes);
		}
	}

	for (const auto& aabb : m_culledBoxes)
	{
		GetWorld()->GetDebugContext()->DrawAABB(aabb, glm::vec4(0.2f, 0.8f, 0.2f, 1.0f));
	}

	if (auto camera = GetOwner()->GetComponent<CameraComponent>())
	{
		if (m_cachedFrustum != glm::mat4(1))
		{
			GetWorld()->GetDebugContext()->DrawFrustum(m_cachedFrustum, camera->GetFov(), 500.0f, camera->GetZNear(), camera->GetAspect(), glm::vec4(1.0, 1.0, 0.0, 1.0f));
		}
	}

	m_lastCursorPos = GetWorld()->GetInput().GetCursorPos();

	if (GetWorld()->GetInput().IsKeyPressed('T'))
	{
		auto camera = GetOwner()->GetComponent<CameraComponent>();

		auto gameObjects = GetWorld()->GetGameObjects();

		for (int32_t i = 0; i < App::GetViewportWindow()->GetWidth() / 16; i++)
		{
			for (int32_t j = 0; j < App::GetViewportWindow()->GetHeight() / 16; j++)
			{
				const glm::mat4 invViewProjection = camera->GetData().GetInvViewProjection();
				const glm::mat4 invProjection = camera->GetData().GetInvProjection();
				const glm::mat4 view = camera->GetData().GetViewMatrix();
				const glm::mat4 projection = camera->GetData().GetProjectionMatrix();

				for (int32_t k = 0; k < gameObjects.Num(); k++)
				{
					if (auto light = gameObjects[k]->GetComponent<LightComponent>())
					{
						DrawTile(GetWorld(),
							invViewProjection,
							camera->GetZFar(),
							transform.GetWorldPosition(),
							glm::ivec2(i, j),
							gameObjects[k]->GetTransformComponent().GetWorldPosition(),
							light->GetBounds().x);
					}
				}
			}
		}
	}

	for (uint32_t i = 0; i < m_lights.Num(); i++)
	{
		if (glm::length(m_lightVelocities[i]) < 1.0f)
		{
			const glm::vec4 velocity = glm::vec4(glm::sphericalRand(75.0f + rand() % 50), 0.0f);
			m_lightVelocities[i] = velocity;
		}

		const auto& position = m_lights[i]->GetTransformComponent().GetWorldPosition();

		m_lightVelocities[i] = Math::Lerp(m_lightVelocities[i], vec4(0, 0, 0, 0), deltaTime * 0.5f);
		m_lights[i]->GetTransformComponent().SetPosition(position + deltaTime * m_lightVelocities[i]);
	}

	if (GetWorld()->GetInput().IsKeyPressed('R'))
	{
		for (auto& go : GetWorld()->GetGameObjects())
		{
			if (auto mr = go->GetComponent<MeshRendererComponent>())
			{
				for (auto& mat : mr->GetMaterials())
				{
					if (mat && mat->IsReady() && mat->GetShaderBindings()->HasParameter("material.color"))
					{
						mat = Material::CreateInstance(GetWorld(), mat);

						const glm::vec4 color = glm::vec4(glm::ballRand(1.0f), 1);

						App::GetSubmodule<Sailor::RHI::Renderer>()->GetDriverCommands()->SetMaterialParameter(GetWorld()->GetCommandList(),
							mat->GetShaderBindings(), "material.color", color);
					}
				}
			}
		}
	}
}
