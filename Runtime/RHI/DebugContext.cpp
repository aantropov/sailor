#include "Types.h"
#include "Mesh.h"
#include "DebugContext.h"
#include "CommandList.h"
#include "VertexDescription.h"

#ifdef  VULKAN
#include "GraphicsDriver/Vulkan/VulkanPipeline.h"
#endif 

using namespace Sailor;
using namespace Sailor::RHI;

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

void DebugContext::DrawOrigin(const glm::vec3& position, float size, float duration)
{
	DrawLine(position, position + glm::vec3(size, 0, 0), glm::vec4(1, 0, 0, 1), duration);
	DrawLine(position, position + glm::vec3(0, size, 0), glm::vec4(0, 1, 0, 1), duration);
	DrawLine(position, position + glm::vec3(0, 0, size), glm::vec4(0, 0, 1, 1), duration);
}

void DebugContext::DrawFrustum(const glm::mat4& worldMatrix, float fovDegrees, float maxRange, float minRange, float aspect, const glm::vec4 color, float duration)
{
	float tanfov = tanf(glm::radians(fovDegrees / 2));

	glm::vec3 farEnd(maxRange, 0, 0);
	glm::vec3 endSizeHorizontal(0, 0, maxRange * tanfov * aspect);
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
		glm::vec3 startSizeX(0, 0, minRange * tanfov * aspect);
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

	DrawLine(s1, e1);
	DrawLine(s2, e2);
	DrawLine(s3, e3);
	DrawLine(s4, e4);
}

DebugFrame DebugContext::Tick(RHI::ShaderBindingSetPtr frameBindings, float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_lineVertices.Num() == 0)
	{
		return DebugFrame();
	}

	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();

	if (!m_material)
	{
		RenderState renderState = RHI::RenderState(true, true, 0.1f, ECullMode::FrontAndBack, EBlendMode::None, EFillMode::Line);

		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Gizmo.shader");
		ShaderSetPtr pShader;

		if (!App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader))
		{
			return DebugFrame();
		}

		m_cachedMesh = renderer->CreateMesh();
		m_cachedMesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3C4>();
		m_material = renderer->CreateMaterial(m_cachedMesh->m_vertexDescription, EPrimitiveTopology::LineList, renderState, pShader);
	}

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
	const bool bNeedUpdateVertexBuffer = m_lineVerticesOffset != -1 || bShouldCreateVertexBuffer;

	m_cachedFrame.m_signalSemaphore = nullptr;

	if (bNeedUpdateVertexBuffer || bNeedUpdateIndexBuffer)
	{
		RHI::CommandListPtr updateMeshCmd = renderer->CreateCommandList(false, true);
		RHI::Renderer::GetDriverCommands()->BeginCommandList(updateMeshCmd);

		if (bShouldCreateVertexBuffer)
		{
			m_cachedMesh->m_vertexBuffer = renderer->CreateBuffer(updateMeshCmd,
				&m_lineVertices[0],
				bufferSize,
				EBufferUsageBit::VertexBuffer_Bit);
		}
		else
		{
			RHI::Renderer::GetDriverCommands()->UpdateBuffer(updateMeshCmd, m_cachedMesh->m_vertexBuffer,
				&m_lineVertices[m_lineVerticesOffset],
				bufferSize - sizeof(VertexP3C4) * m_lineVerticesOffset,
				sizeof(VertexP3C4) * m_lineVerticesOffset);
		}

		if (bNeedUpdateIndexBuffer)
		{
			if (!m_cachedMesh->m_indexBuffer || m_cachedMesh->m_indexBuffer->GetSize() < indexBufferSize)
			{
				m_cachedMesh->m_indexBuffer = renderer->CreateBuffer(updateMeshCmd,
					&m_cachedIndices[0],
					indexBufferSize,
					EBufferUsageBit::IndexBuffer_Bit);
			}
			else
			{
				RHI::Renderer::GetDriverCommands()->UpdateBuffer(updateMeshCmd, m_cachedMesh->m_indexBuffer, &m_cachedIndices[0], indexBufferSize);
			}
		}

		RHI::Renderer::GetDriverCommands()->EndCommandList(updateMeshCmd);

		auto semaphore = App::GetSubmodule<Renderer>()->GetDriver()->CreateWaitSemaphore();
		m_cachedFrame.m_signalSemaphore = semaphore;

		//renderer->SubmitCommandList(updateMeshCmd, RHI::FencePtr::Make(), result.m_signalSemaphore);

		SAILOR_ENQUEUE_JOB_RENDER_THREAD("Create mesh",
			([&renderer, updateMeshCmd, semaphore]()
		{
			renderer->SubmitCommandList(updateMeshCmd, RHI::FencePtr::Make(), semaphore);
		}));
	}

	if (!m_cachedFrame.m_drawDebugMeshCmd ||
		!RHI::Renderer::GetDriverCommands()->FitsDefaultViewport(m_cachedFrame.m_drawDebugMeshCmd) ||
		m_cachedFrame.m_linesCount != m_lineVertices.Num() / 2)
	{
		m_cachedFrame.m_drawDebugMeshCmd = CreateRenderingCommandList(frameBindings, m_cachedMesh);
		m_cachedFrame.m_linesCount = (uint32_t)m_lineVertices.Num() / 2;
	}

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
				m_lineVerticesOffset = i;
			}

			i--;
		}
	}

	return m_cachedFrame;
}

RHI::CommandListPtr DebugContext::CreateRenderingCommandList(RHI::ShaderBindingSetPtr frameBindings, RHI::MeshPtr debugMesh) const
{
	if (m_lineVertices.Num() == 0)
	{
		return nullptr;
	}

	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();
	RHI::CommandListPtr graphicsCmd = renderer->CreateCommandList(true, false);

	RHI::Renderer::GetDriverCommands()->BeginCommandList(graphicsCmd);

	RHI::Renderer::GetDriverCommands()->BindMaterial(graphicsCmd, m_material);
	RHI::Renderer::GetDriverCommands()->BindVertexBuffers(graphicsCmd, { debugMesh->m_vertexBuffer });
	RHI::Renderer::GetDriverCommands()->BindIndexBuffer(graphicsCmd, debugMesh->m_indexBuffer);
	RHI::Renderer::GetDriverCommands()->SetDefaultViewport(graphicsCmd);
	RHI::Renderer::GetDriverCommands()->BindShaderBindings(graphicsCmd, m_material, { frameBindings /*m_material->GetBindings()*/ });
	RHI::Renderer::GetDriverCommands()->DrawIndexed(graphicsCmd, debugMesh->m_indexBuffer, (uint32_t)debugMesh->m_indexBuffer->GetSize() / sizeof(uint32_t), 1, 0, 0, 0);

	RHI::Renderer::GetDriverCommands()->EndCommandList(graphicsCmd);

	return graphicsCmd;
}