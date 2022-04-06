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

void DebugContext::DrawLine(const glm::vec4& start, const glm::vec4& end, const glm::vec4 color, float duration)
{
	LineProxy proxy;

	proxy.m_startLocation = start;
	proxy.m_endLocation = end;
	proxy.m_lifetime = duration;
	proxy.m_color = color;

	m_lines.Add(std::move(proxy));
}
void DebugContext::DrawOrigin(const glm::vec4& position, float size, float duration)
{
	DrawLine(position, position + glm::vec4(size, 0, 0, 0), glm::vec4(1, 0, 0, 0), duration);
	DrawLine(position, position + glm::vec4(0, size, 0, 0), glm::vec4(0, 1, 0, 0), duration);
	DrawLine(position, position + glm::vec4(0, 0, size, 0), glm::vec4(0, 0, 1, 0), duration);
}

DebugFrame DebugContext::Tick(RHI::ShaderBindingSetPtr frameBindings, float deltaTime)
{
	DebugFrame result;

	SAILOR_PROFILE_FUNCTION();

	if (m_lines.Num() == 0)
	{
		return result;
	}

	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();

	MeshPtr debugMesh = renderer->CreateMesh();

	if (!m_material)
	{
		RenderState renderState = RHI::RenderState(true, false, 0.0f, ECullMode::FrontAndBack, EBlendMode::None, EFillMode::Line);

		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Gizmo.shader");
		ShaderSetPtr pShader;

		if (!App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader))
		{
			return result;
		}

		debugMesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3C4>();
		m_material = renderer->CreateMaterial(debugMesh->m_vertexDescription, EPrimitiveTopology::LineList, renderState, pShader);
	}

	TVector<VertexP3C4> vertices;
	TVector<uint32_t> indices;
	for (uint32_t i = 0; i < m_lines.Num(); i++)
	{
		VertexP3C4 start{};
		start.m_color = m_lines[i].m_color;
		start.m_position = m_lines[i].m_startLocation;

		VertexP3C4 end{};
		end.m_color = m_lines[i].m_color;
		end.m_position = m_lines[i].m_endLocation;

		vertices.Emplace(std::move(start));
		vertices.Emplace(std::move(end));

		indices.Add((uint32_t)indices.Num());
		indices.Add((uint32_t)indices.Num());
	}

	const VkDeviceSize bufferSize = sizeof(RHI::VertexP3C4) * m_lines.Num() * 2;
	const VkDeviceSize indexBufferSize = sizeof(uint32_t) * m_lines.Num() * 2;

	RHI::CommandListPtr updateMeshCmd = renderer->CreateCommandList(false, true);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(updateMeshCmd);

	debugMesh->m_vertexBuffer = renderer->CreateBuffer(updateMeshCmd,
		&vertices[0],
		bufferSize,
		EBufferUsageBit::VertexBuffer_Bit);

	debugMesh->m_indexBuffer = renderer->CreateBuffer(updateMeshCmd,
		&indices[0],
		indexBufferSize,
		EBufferUsageBit::IndexBuffer_Bit);

	RHI::Renderer::GetDriverCommands()->EndCommandList(updateMeshCmd);

	auto semaphore = App::GetSubmodule<Renderer>()->GetDriver()->CreateWaitSemaphore();
	result.m_signalSemaphore = semaphore;

	//renderer->SubmitCommandList(updateMeshCmd, RHI::FencePtr::Make(), result.m_signalSemaphore);

	SAILOR_ENQUEUE_JOB_RENDER_THREAD("Create mesh",
		([&renderer, updateMeshCmd, semaphore]()
	{
		renderer->SubmitCommandList(updateMeshCmd, RHI::FencePtr::Make(), semaphore);
	}));

	result.m_drawDebugMeshCmd = CreateRenderingCommandList(frameBindings, debugMesh);

	for (uint32_t i = 0; i < m_lines.Num(); i++)
	{
		m_lines[i].m_lifetime -= deltaTime;

		if (m_lines[i].m_lifetime < 0.0f)
		{
			m_lines.RemoveAtSwap(i, 1);
			i--;
		}
	}

	return result;
}

RHI::CommandListPtr DebugContext::CreateRenderingCommandList(RHI::ShaderBindingSetPtr frameBindings, RHI::MeshPtr debugMesh) const
{
	if (m_lines.Num() == 0 || !m_material)
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
	RHI::Renderer::GetDriverCommands()->DrawIndexed(graphicsCmd, debugMesh->m_indexBuffer, 1, 0, 0, 0);

	RHI::Renderer::GetDriverCommands()->EndCommandList(graphicsCmd);

	return graphicsCmd;
}