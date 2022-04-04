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

void DebugContext::Tick(float deltaTime)
{
	SAILOR_PROFILE_FUNCTION();

	if (m_lines.Num() == 0)
	{
		return;
	}

	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();

	m_mesh = renderer->CreateMesh();

	if (!m_material)
	{
		RenderState renderState = RHI::RenderState(true, false, 0.001f, ECullMode::FrontAndBack, EBlendMode::None, EFillMode::Line);

		auto shaderUID = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr("Shaders/Gizmo.shader");
		ShaderSetPtr pShader;

		if (!App::GetSubmodule<ShaderCompiler>()->LoadShader_Immediate(shaderUID->GetUID(), pShader))
		{
			return;
		}

		m_mesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3C4>();
		m_material = renderer->CreateMaterial(m_mesh->m_vertexDescription, EPrimitiveTopology::LineList, renderState, pShader);
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

	m_transferCmd = renderer->CreateCommandList(false, true);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(m_transferCmd);

	m_mesh->m_vertexBuffer = renderer->CreateBuffer(m_transferCmd,
		&vertices[0],
		bufferSize,
		EBufferUsageBit::VertexBuffer_Bit);

	m_mesh->m_indexBuffer = renderer->CreateBuffer(m_transferCmd,
		&indices[0],
		indexBufferSize,
		EBufferUsageBit::IndexBuffer_Bit);

	RHI::Renderer::GetDriverCommands()->EndCommandList(m_transferCmd);

	m_syncSemaphore = RHI::SemaphorePtr::Make();
	renderer->SubmitCommandList(m_transferCmd, nullptr, m_syncSemaphore);

	for (uint32_t i = 0; i < m_lines.Num(); i++)
	{
		m_lines[i].m_lifetime -= deltaTime;

		if (m_lines[i].m_lifetime < 0.0f)
		{
			m_lines.RemoveAtSwap(i, 1);
			i--;
		}
	}
}

RHI::CommandListPtr DebugContext::CreateRenderingCommandList() const
{
	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();
	RHI::CommandListPtr graphicsCmd = renderer->CreateCommandList(true, false);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(m_graphicsCmd);

	// TODO: Adjust high level command lists API
#ifdef  VULKAN
	auto vulkamCmd = graphicsCmd->m_vulkan.m_commandBuffer;
	vulkamCmd->BindPipeline(m_material->m_vulkan.m_pipeline);
	vulkamCmd->BindVertexBuffers({ m_mesh->m_vertexBuffer->m_vulkan.m_buffer });
	vulkamCmd->BindIndexBuffer(m_mesh->m_indexBuffer->m_vulkan.m_buffer);

	// TODO: Parse missing descriptor sets
	TVector<VulkanDescriptorSetPtr> sets;
	sets.Add(m_material->GetBindings()->m_vulkan.m_descriptorSet);

	vulkamCmd->BindDescriptorSet(m_material->m_vulkan.m_pipeline->m_layout, sets);
	vulkamCmd->DrawIndexed(m_mesh->m_indexBuffer->m_vulkan.m_buffer, 1, 0, 0, 0);
#endif //  VULKAN

	RHI::Renderer::GetDriverCommands()->EndCommandList(graphicsCmd);

	return graphicsCmd;
}