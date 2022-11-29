#include "Types.h"
#include "Mesh.h"
#include "DebugContext.h"
#include "CommandList.h"
#include "VertexDescription.h"

#ifdef SAILOR_BUILD_WITH_VULKAN
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"
#endif //SAILOR_BUILD_WITH_VULKAN

using namespace Sailor;
using namespace Sailor::RHI;

void DebugContext::DrawSphere(const glm::vec3& position, float radius, const glm::vec4 color, float duration)
{
	const float PI = 3.1415926f;
	const int32_t SegmentsX = 7;
	const int32_t SegmentsY = 7;

	for (int32_t i = 0; i <= SegmentsX; i++)
	{
		float lat0 = PI * (-0.5f + (float)(i - 1) / SegmentsX);
		float z0 = sin(lat0);
		float zr0 = cos(lat0);

		float lat1 = PI * (-0.5f + (float)i / SegmentsX);
		float z1 = sin(lat1);
		float zr1 = cos(lat1);

		glm::vec3 v1;
		glm::vec3 v2;
		glm::vec3 v3;
		glm::vec3 v4;

		bool bContinuation = false;
		for (int32_t j = 0; j <= SegmentsY; j++)
		{
			float lng = 2 * PI * (float)(j - 1) / SegmentsY;
			float x = cos(lng);
			float y = sin(lng);

			v1 = position + glm::vec3(radius * x * zr0, radius * z0, radius * y * zr0);
			v2 = position + glm::vec3(radius * x * zr1, radius * z1, radius * y * zr1);

			if (!bContinuation)
			{
				bContinuation = true;
			}
			else
			{
				DrawLine(v1, v3, color, duration);
				DrawLine(v2, v4, color, duration);
			}

			DrawLine(v1, v2, color, duration);

			v3 = v1;
			v4 = v2;
		}
	}
}

void DebugContext::DrawPlane(const Math::Plane& plane, float size, const glm::vec4 color, float duration)
{
	Math::Plane normalizedPlane = plane;
	normalizedPlane.Normalize();

	const glm::vec3 center = normalizedPlane.GetNormal() * normalizedPlane.m_abcd.z;

	DrawLine(center, center + normalizedPlane.GetNormal() * size, color, duration);
	DrawLine(center, center + normalizedPlane.GetNormal() * size, color, duration);
}

void DebugContext::DrawLine(const glm::vec3& start, const glm::vec3& end, const glm::vec4 color, float duration)
{
	VertexP3C4 startVertex;
	VertexP3C4 endVertex;

	startVertex.m_position = start;
	startVertex.m_color = color;

	endVertex.m_position = end;
	endVertex.m_color = color;

	m_lineVertices.Add(startVertex);
	m_lineVertices.Add(endVertex);

	m_lifetimes.Add(duration);

	if (m_lineVerticesOffset == -1)
	{
		m_lineVerticesOffset = (int32_t)m_lifetimes.Num() - 1;
	}

	m_bShouldUpdateMeshThisFrame = true;
}

void DebugContext::DrawAABB(const Math::AABB& aabb, const glm::vec4 color, float duration)
{
	DrawLine(aabb.m_min, vec3(aabb.m_max.x, aabb.m_min.y, aabb.m_min.z), color, duration);
	DrawLine(aabb.m_min, vec3(aabb.m_min.x, aabb.m_max.y, aabb.m_min.z), color, duration);
	DrawLine(aabb.m_min, vec3(aabb.m_min.x, aabb.m_min.y, aabb.m_max.z), color, duration);

	DrawLine(aabb.m_max, vec3(aabb.m_min.x, aabb.m_max.y, aabb.m_max.z), color, duration);
	DrawLine(aabb.m_max, vec3(aabb.m_max.x, aabb.m_min.y, aabb.m_max.z), color, duration);
	DrawLine(aabb.m_max, vec3(aabb.m_max.x, aabb.m_max.y, aabb.m_min.z), color, duration);

	DrawLine(vec3(aabb.m_min.x, aabb.m_max.y, aabb.m_max.z), vec3(aabb.m_min.x, aabb.m_max.y, aabb.m_min.z), color, duration);
	DrawLine(vec3(aabb.m_min.x, aabb.m_max.y, aabb.m_max.z), vec3(aabb.m_min.x, aabb.m_min.y, aabb.m_max.z), color, duration);
	DrawLine(vec3(aabb.m_min.x, aabb.m_min.y, aabb.m_max.z), vec3(aabb.m_max.x, aabb.m_min.y, aabb.m_max.z), color, duration);
	DrawLine(vec3(aabb.m_max.x, aabb.m_min.y, aabb.m_max.z), vec3(aabb.m_max.x, aabb.m_min.y, aabb.m_min.z), color, duration);
	DrawLine(vec3(aabb.m_max.x, aabb.m_min.y, aabb.m_min.z), vec3(aabb.m_max.x, aabb.m_max.y, aabb.m_min.z), color, duration);
	DrawLine(vec3(aabb.m_max.x, aabb.m_max.y, aabb.m_min.z), vec3(aabb.m_min.x, aabb.m_max.y, aabb.m_min.z), color, duration);
}

void DebugContext::DrawArrow(const glm::vec3& start, const glm::vec3& end, const glm::vec4 color, float duration)
{
	const float length = glm::length(end - start) * 0.1f;

	DrawLine(start, end, color, duration);

	DrawLine(end + Math::vec3_Up * length, end + Math::vec3_Down * length, color, duration);
	DrawLine(end + Math::vec3_Left * length, end + Math::vec3_Right * length, color, duration);
	DrawLine(end + Math::vec3_Forward * length, end + Math::vec3_Backward * length, color, duration);
}

void DebugContext::DrawOrigin(const glm::vec3& position, const glm::mat4& origin, float size, float duration)
{
	glm::vec3 x = origin * glm::vec4(size, 0, 0, 0);
	glm::vec3 y = origin * glm::vec4(0, size, 0, 0);
	glm::vec3 z = origin * glm::vec4(0, 0, size, 0);

	DrawLine(position, position + x, glm::vec4(1, 0, 0, 1), duration);
	DrawLine(position, position + y, glm::vec4(0, 1, 0, 1), duration);
	DrawLine(position, position + z, glm::vec4(0, 0, 1, 1), duration);
}

void DebugContext::DrawFrustum(const glm::mat4& worldMatrix, float fovDegrees, float maxRange, float minRange, float aspect, const glm::vec4 color, float duration)
{
	float tanfov = tanf(glm::radians(fovDegrees / 2));

	glm::vec3 farEnd(0, 0, -maxRange);
	glm::vec3 endSizeHorizontal(maxRange * tanfov * aspect, 0, 0);
	glm::vec3 endSizeVertical(0, maxRange * tanfov, 0);

	glm::vec3 s1, s2, s3, s4;
	glm::vec3 e1 = worldMatrix * glm::vec4(farEnd + endSizeHorizontal + endSizeVertical, 1);
	glm::vec3 e2 = worldMatrix * glm::vec4(farEnd - endSizeHorizontal + endSizeVertical, 1);
	glm::vec3 e3 = worldMatrix * glm::vec4(farEnd - endSizeHorizontal - endSizeVertical, 1);
	glm::vec3 e4 = worldMatrix * glm::vec4(farEnd + endSizeHorizontal - endSizeVertical, 1);

	if (minRange <= 0.0f)
	{
		s1 = s2 = s3 = s4 = glm::vec3(0, 0, 0);
	}
	else
	{
		glm::vec3 startSizeX(minRange * tanfov * aspect, 0, 0);
		glm::vec3 startSizeY(0, minRange * tanfov, 0);
		glm::vec3 startPoint = glm::vec3(minRange, 0, 0);

		s1 = worldMatrix * glm::vec4(startPoint + startSizeX + startSizeY, 1);
		s2 = worldMatrix * glm::vec4(startPoint - startSizeX + startSizeY, 1);
		s3 = worldMatrix * glm::vec4(startPoint - startSizeX - startSizeY, 1);
		s4 = worldMatrix * glm::vec4(startPoint + startSizeX - startSizeY, 1);

		DrawLine(s1, s2, color, duration);
		DrawLine(s2, s3, color, duration);
		DrawLine(s3, s4, color, duration);
		DrawLine(s4, s1, color, duration);
	}

	DrawLine(e1, e2, color, duration);
	DrawLine(e2, e3, color, duration);
	DrawLine(e3, e4, color, duration);
	DrawLine(e4, e1, color, duration);

	DrawLine(s1, e1, color, duration);
	DrawLine(s2, e2, color, duration);
	DrawLine(s3, e3, color, duration);
	DrawLine(s4, e4, color, duration);
}

void DebugContext::DrawCone(const glm::vec3& start, const glm::vec3& end, float degrees, const glm::vec4 color, float duration)
{
}

void DebugContext::Tick(RHI::RHICommandListPtr transferCmd, float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_lineVertices.Num() == 0)
	{
		return;
	}
	
	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();

	if (!m_material)
	{
		RenderState renderState = RHI::RenderState(true, true, 0.0f, true, ECullMode::Back, EBlendMode::None, EFillMode::Line, GetHash(std::string("Debug")), true);

		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Gizmo.shader");
		ShaderSetPtr pShader;

		if (!App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader))
		{
			return;
		}

		m_cachedMesh = renderer->CreateMesh();
		m_cachedMesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3C4>();
		m_material = renderer->CreateMaterial(m_cachedMesh->m_vertexDescription, EPrimitiveTopology::LineList, renderState, pShader);
	}

	UpdateDebugMesh(transferCmd);

	m_numRenderedVertices = (uint32_t)m_lineVertices.Num();

	m_lineVerticesOffset = -1;
	for (uint32_t i = 0; i < m_lifetimes.Num(); i++)
	{
		m_lifetimes[i] -= deltaTime;

		if (m_lifetimes[i] < 0.0f)
		{
			m_lifetimes.RemoveAtSwap(i, 1);
			m_lineVertices.RemoveAtSwap(i * 2, 2);

			if (m_lineVerticesOffset == -1)
			{
				m_lineVerticesOffset = i * 2;
			}

			i--;
		}
	}

	if (m_lineVerticesOffset == m_lineVertices.Num())
	{
		m_lineVerticesOffset = -1;
	}
}

void DebugContext::UpdateDebugMesh(RHI::RHICommandListPtr transferCmdList)
{
	if (m_lineVertices.Num() == 0)
	{
		return;
	}

	auto commands = RHI::Renderer::GetDriverCommands();
	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();

	const bool bNeedUpdateIndexBuffer = m_cachedIndices.Num() < m_lineVertices.Num();
	if (bNeedUpdateIndexBuffer)
	{
		uint32_t start = (uint32_t)m_cachedIndices.Num();
		m_cachedIndices.Resize(m_lineVertices.Num());

		for (uint32_t i = start; i < m_lineVertices.Num(); i++)
		{
			m_cachedIndices[i] = i;
		}
	}

	const VkDeviceSize bufferSize = sizeof(RHI::VertexP3C4) * m_lineVertices.Num();
	const VkDeviceSize indexBufferSize = sizeof(uint32_t) * m_lineVertices.Num();

	const bool bShouldCreateVertexBuffer = !m_cachedMesh->m_vertexBuffer || m_cachedMesh->m_vertexBuffer->GetSize() < bufferSize;
	const bool bNeedUpdateVertexBuffer = m_lineVerticesOffset != -1 || bShouldCreateVertexBuffer || m_bShouldUpdateMeshThisFrame;

	if (bNeedUpdateVertexBuffer || bNeedUpdateIndexBuffer)
	{
		if (bShouldCreateVertexBuffer)
		{
			m_cachedMesh->m_vertexBuffer = renderer->CreateBuffer(transferCmdList,
				&m_lineVertices[0],
				bufferSize,
				EBufferUsageBit::VertexBuffer_Bit);
		}
		else
		{
			m_lineVerticesOffset = std::max(m_lineVerticesOffset, 0);

			RHI::Renderer::GetDriverCommands()->UpdateBuffer(transferCmdList, m_cachedMesh->m_vertexBuffer,
				&m_lineVertices[m_lineVerticesOffset],
				bufferSize - sizeof(VertexP3C4) * m_lineVerticesOffset,
				sizeof(VertexP3C4) * m_lineVerticesOffset);
		}

		if (bNeedUpdateIndexBuffer)
		{
			if (!m_cachedMesh->m_indexBuffer || m_cachedMesh->m_indexBuffer->GetSize() < indexBufferSize)
			{
				m_cachedMesh->m_indexBuffer = renderer->CreateBuffer(transferCmdList,
					&m_cachedIndices[0],
					indexBufferSize,
					EBufferUsageBit::IndexBuffer_Bit);
			}
			else
			{
				commands->UpdateBuffer(transferCmdList, m_cachedMesh->m_indexBuffer, &m_cachedIndices[0], indexBufferSize);
			}
		}
	}

	m_bShouldUpdateMeshThisFrame = false;
}

void DebugContext::DrawDebugMesh(RHI::RHICommandListPtr secondaryDrawCmdList, const glm::mat4x4& viewProjection) const
{
	if (m_numRenderedVertices == 0 || !m_cachedMesh || !m_cachedMesh->IsReady())
	{
		return;
	}

	auto commands = RHI::Renderer::GetDriverCommands();
	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();
	
	commands->SetDefaultViewport(secondaryDrawCmdList);
	commands->BindMaterial(secondaryDrawCmdList, m_material);
	commands->BindVertexBuffer(secondaryDrawCmdList, m_cachedMesh->m_vertexBuffer, m_cachedMesh->m_vertexBuffer->GetOffset());
	commands->BindIndexBuffer(secondaryDrawCmdList, m_cachedMesh->m_indexBuffer, m_cachedMesh->m_indexBuffer->GetOffset());
	commands->PushConstants(secondaryDrawCmdList, m_material, sizeof(viewProjection), &viewProjection);
	//commands->BindShaderBindings(secondaryDrawCmdList, m_material, { frameBindings /*m_material->GetBindings()*/ });
	commands->DrawIndexed(secondaryDrawCmdList, (uint32_t)m_numRenderedVertices, 1, 0, 0, 0);
}
