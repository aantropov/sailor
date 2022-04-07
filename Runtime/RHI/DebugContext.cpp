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
	LineProxy proxy;

	proxy.m_startLocation = start;
	proxy.m_endLocation = end;
	proxy.m_lifetime = duration;
	proxy.m_color = color;

	m_lines.Add(std::move(proxy));
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

DebugFrame DebugContext::Tick(RHI::ShaderBindingSetPtr frameBindings, float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_lines.Num() == 0)
	{
		return DebugFrame();
	}

	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();

	if (!m_material)
	{
		RenderState renderState = RHI::RenderState(true, false, 0.1f, ECullMode::FrontAndBack, EBlendMode::None, EFillMode::Line);

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

	TVector<VertexP3C4> vertices(m_lines.Num() * 2);

	const bool bNeedUpdateIndexBuffer = m_cachedIndices.Num() < m_lines.Num() * 2;
	if (bNeedUpdateIndexBuffer)
	{
		m_cachedIndices.Resize(m_lines.Num() * 2);
	}

	for (uint32_t i = 0; i < m_lines.Num(); i++)
	{
		VertexP3C4 start{};
		start.m_color = m_lines[i].m_color;
		start.m_position = m_lines[i].m_startLocation;

		VertexP3C4 end{};
		end.m_color = m_lines[i].m_color;
		end.m_position = m_lines[i].m_endLocation;

		vertices[i * 2] = start;
		vertices[i * 2 + 1] = end;

		if (bNeedUpdateIndexBuffer)
		{
			m_cachedIndices[i * 2] = i * 2;
			m_cachedIndices[i * 2 + 1] = i * 2 + 1;
		}
	}

	const VkDeviceSize bufferSize = sizeof(RHI::VertexP3C4) * m_lines.Num() * 2;
	const VkDeviceSize indexBufferSize = sizeof(uint32_t) * m_lines.Num() * 2;

	RHI::CommandListPtr updateMeshCmd = renderer->CreateCommandList(false, true);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(updateMeshCmd);

	if (!m_cachedMesh->m_vertexBuffer || m_cachedMesh->m_vertexBuffer->GetSize() < bufferSize)
	{
		m_cachedMesh->m_vertexBuffer = renderer->CreateBuffer(updateMeshCmd,
			&vertices[0],
			bufferSize,
			EBufferUsageBit::VertexBuffer_Bit);
	}
	else
	{
		RHI::Renderer::GetDriverCommands()->UpdateBuffer(updateMeshCmd, m_cachedMesh->m_vertexBuffer, &vertices[0], bufferSize);
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

	if (!m_cachedFrame.m_drawDebugMeshCmd ||
		!RHI::Renderer::GetDriverCommands()->FitsDefaultViewport(m_cachedFrame.m_drawDebugMeshCmd) ||
		m_cachedFrame.m_linesCount != m_lines.Num())
	{
		m_cachedFrame.m_drawDebugMeshCmd = CreateRenderingCommandList(frameBindings, m_cachedMesh);
		m_cachedFrame.m_linesCount = (uint32_t)m_lines.Num();
	}

	for (uint32_t i = 0; i < m_lines.Num(); i++)
	{
		m_lines[i].m_lifetime -= deltaTime;

		if (m_lines[i].m_lifetime < 0.0f)
		{
			m_lines.RemoveAtSwap(i, 1);
			i--;
		}
	}

	return m_cachedFrame;
}

RHI::CommandListPtr DebugContext::CreateRenderingCommandList(RHI::ShaderBindingSetPtr frameBindings, RHI::MeshPtr debugMesh) const
{
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