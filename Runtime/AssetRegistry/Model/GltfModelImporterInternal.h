#pragma once

#include <cstddef>
#include <cstdint>

namespace tinygltf
{
	class Accessor;
	class Model;
}

namespace Sailor::GltfImporterInternal
{
	struct GltfAccessorView final
	{
		const tinygltf::Accessor* m_accessor = nullptr;
		const uint8_t* m_data = nullptr;
		size_t m_stride = 0;
		size_t m_componentSize = 0;
		size_t m_componentCount = 0;
		const uint8_t* m_sparseIndices = nullptr;
		const uint8_t* m_sparseValues = nullptr;
		size_t m_sparseCount = 0;
		size_t m_sparseIndexStride = 0;
		size_t m_sparseValueStride = 0;
		int32_t m_sparseIndexComponentType = -1;
	};

	bool TryGetAccessorView(const tinygltf::Model& model,
		int32_t accessorIndex,
		int32_t expectedType,
		size_t requiredCount,
		GltfAccessorView& outView);
	float ReadAccessorFloat(const GltfAccessorView& view, size_t elementIndex, size_t componentIndex);
	bool TryReadAccessorIndex(const GltfAccessorView& view, size_t elementIndex, uint32_t& outIndex);
	bool HasGltfExtension(const tinygltf::Model& model, const char* extension);
	bool IsFloatAccessor(const tinygltf::Accessor& accessor);
	bool IsPositionAccessorSupported(const tinygltf::Accessor& accessor, bool bHasMeshQuantization);
	bool IsDirectionAccessorSupported(const tinygltf::Accessor& accessor, bool bHasMeshQuantization);
	bool IsTexcoordAccessorSupported(const tinygltf::Accessor& accessor, bool bHasMeshQuantization);
	bool IsColorAccessorSupported(const tinygltf::Accessor& accessor);
	bool IsJointsAccessorSupported(const tinygltf::Accessor& accessor);
	bool IsWeightsAccessorSupported(const tinygltf::Accessor& accessor);
}
