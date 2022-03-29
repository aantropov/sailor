#include "Types.h"
#include "DebugContext.h"

using namespace Sailor;
using namespace Sailor::RHI;

void DebugContext::DrawLine(const glm::vec4& start, const glm::vec4& end, const glm::vec4 color)
{
}

void DebugContext::Prepare()
{
	m_transferCmd = App::GetSubmodule<Renderer>()->GetDriver()->CreateCommandList(false, true);
	m_graphicsCmd = App::GetSubmodule<Renderer>()->GetDriver()->CreateCommandList(false, false);
}

void DebugContext::SubmitBuffers()
{
}
