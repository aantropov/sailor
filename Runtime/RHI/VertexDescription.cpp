#include "VertexDescription.h"
#include "Types.h"
#include "GraphicsDriver/Vulkan/VulkanApi.h"

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

void RHIVertexDescription::AddAttribute(uint32_t location, uint32_t binding, EFormat format, uint32_t offset)
{
	AttributeDescription attribute;
	attribute.m_location = location;
	attribute.m_binding = binding;
	attribute.m_format = format;
	attribute.m_offset = offset;

	SetAttributeFormat(m_bits, binding, format);

	m_attributes.Emplace(attribute);
}
