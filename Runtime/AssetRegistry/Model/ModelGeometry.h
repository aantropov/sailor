#pragma once

#include "RHI/VertexDescription.h"

namespace Sailor::ModelGeometry
{
	void SanitizeVertexFrame(RHI::VertexP3N3T3B3UV2C4I4W4& vertex);
}
