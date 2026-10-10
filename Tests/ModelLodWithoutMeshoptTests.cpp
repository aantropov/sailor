#include "AssetRegistry/Model/ModelLodGeneration.h"

#include <iostream>
#include <stdexcept>

#if defined(SAILOR_HAS_MESHOPT)
#error This target must compile LOD generation without meshoptimizer.
#endif

using namespace Sailor;

int main()
{
	try
	{
		TVector<ModelImporter::MeshContext> meshes(1);
		auto& mesh = meshes[0];
		for (uint32_t y = 0; y <= 4; ++y)
			for (uint32_t x = 0; x <= 4; ++x)
			{
				RHI::VertexP3N3T3B3UV2C4I4W4 vertex{};
				vertex.m_position = glm::vec3(x, y, 0);
				mesh.outVertices.Add(vertex);
			}
		for (uint32_t y = 0; y < 4; ++y)
			for (uint32_t x = 0; x < 4; ++x)
			{
				const uint32_t first = y * 5 + x;
				mesh.outIndices.AddRange({ first, first + 1, first + 5, first + 1, first + 6, first + 5 });
			}
		const auto source = mesh;
		ModelLodGeneration::Generate(meshes, 8, 0.5f);
		if (mesh.lods.Num() != 8 || mesh.outVertices != source.outVertices || mesh.outIndices != source.outIndices)
			throw std::runtime_error("disabled meshoptimizer must preserve source geometry and logical LOD count");
		for (const auto& lod : mesh.lods)
			if (!lod.m_vertices.IsEmpty() || !lod.m_indices.IsEmpty() ||
				lod.m_vertices.Capacity() != 0 || lod.m_indices.Capacity() != 0)
				throw std::runtime_error("disabled meshoptimizer must not allocate duplicate LOD geometry");
		std::cout << "No-meshoptimizer generation: 8 logical LODs, zero duplicate geometry bytes\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
