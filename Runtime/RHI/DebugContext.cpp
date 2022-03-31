#include "Types.h"
#include "Mesh.h"
#include "DebugContext.h"

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

void DebugContext::RenderAll(float deltaTime)
{
	auto& renderer = App::GetSubmodule<Renderer>()->GetDriver();

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

	m_mesh = renderer->CreateMesh();

	const VkDeviceSize bufferSize = sizeof(RHI::VertexP3C4) * m_lines.Num() * 2;
	const VkDeviceSize indexBufferSize = sizeof(uint32_t) * m_lines.Num() * 2;

	RHI::CommandListPtr cmdList = renderer->CreateCommandList(false, true);
	RHI::Renderer::GetDriverCommands()->BeginCommandList(cmdList);

	m_mesh->m_vertexBuffer = renderer->CreateBuffer(cmdList,
		&vertices[0],
		bufferSize,
		EBufferUsageBit::VertexBuffer_Bit);

	m_mesh->m_indexBuffer = renderer->CreateBuffer(cmdList,
		&indices[0],
		indexBufferSize,
		EBufferUsageBit::IndexBuffer_Bit);

	RHI::Renderer::GetDriverCommands()->EndCommandList(cmdList);
	renderer->SubmitCommandList(cmdList);

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
