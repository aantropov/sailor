#pragma once

#include "AssetRegistry/Material/MaterialImporter.h"
#include "RHI/Mesh.h"
#include <array>

namespace Sailor::Tests
{
	using SurfacePixels = std::array<glm::vec4, 64>;
	SurfacePixels RenderSurface(MaterialPtr material, RHI::RHIMeshPtr mesh = {});
}
