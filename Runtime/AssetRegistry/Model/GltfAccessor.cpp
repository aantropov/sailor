#include "AssetRegistry/Model/GltfModelImporterInternal.h"

#include <algorithm>
#include <cstring>

#include <tiny_gltf.h>

namespace Sailor::GltfImporterInternal
{
	static bool IsAccessorRangeValid(size_t offset, size_t stride, size_t count, size_t elementSize, size_t bufferSize)
	{
		if (count == 0)
		{
			return offset <= bufferSize;
		}

		if (offset > bufferSize || elementSize > bufferSize - offset)
		{
			return false;
		}

		if (count <= 1)
		{
			return true;
		}

		return stride > 0 && count - 1 <= (bufferSize - offset - elementSize) / stride;
	}

	static bool HasUnsupportedBufferViewCompression(const tinygltf::BufferView& bufferView)
	{
		return bufferView.extensions.find("EXT_meshopt_compression") != bufferView.extensions.end() ||
			   bufferView.extensions.find("KHR_meshopt_compression") != bufferView.extensions.end();
	}

	static bool TryGetBufferViewData(const tinygltf::Model& model,
		int32_t bufferViewIndex,
		size_t byteOffset,
		size_t stride,
		size_t count,
		size_t elementSize,
		const uint8_t*& outData)
	{
		outData = nullptr;
		if (bufferViewIndex < 0 || static_cast<size_t>(bufferViewIndex) >= model.bufferViews.size())
		{
			return false;
		}

		const tinygltf::BufferView& bufferView = model.bufferViews[bufferViewIndex];
		if (bufferView.buffer < 0 || static_cast<size_t>(bufferView.buffer) >= model.buffers.size() ||
			HasUnsupportedBufferViewCompression(bufferView) ||
			!IsAccessorRangeValid(byteOffset, stride, count, elementSize, bufferView.byteLength))
		{
			return false;
		}

		const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
		if (bufferView.byteOffset > buffer.data.size() ||
			bufferView.byteLength > buffer.data.size() - bufferView.byteOffset)
		{
			return false;
		}

		outData = buffer.data.data() + bufferView.byteOffset + byteOffset;
		return true;
	}

	template <typename T> T ReadUnaligned(const uint8_t* data)
	{
		T value{};
		std::memcpy(&value, data, sizeof(T));
		return value;
	}

	static bool TryReadSparseIndex(const GltfAccessorView& view, size_t sparseElement, uint32_t& outIndex)
	{
		const uint8_t* data = view.m_sparseIndices + sparseElement * view.m_sparseIndexStride;
		switch (view.m_sparseIndexComponentType)
		{
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
			outIndex = ReadUnaligned<uint8_t>(data);
			return true;
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
			outIndex = ReadUnaligned<uint16_t>(data);
			return true;
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
			outIndex = ReadUnaligned<uint32_t>(data);
			return true;
		default:
			return false;
		}
	}

	bool TryGetAccessorView(const tinygltf::Model& model,
		int32_t accessorIndex,
		int32_t expectedType,
		size_t requiredCount,
		GltfAccessorView& outView)
	{
		outView = {};
		if (accessorIndex < 0 || static_cast<size_t>(accessorIndex) >= model.accessors.size())
		{
			return false;
		}

		const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
		if (accessor.count == 0 || accessor.count < requiredCount ||
			(expectedType >= 0 && accessor.type != expectedType) ||
			(!accessor.sparse.isSparse && accessor.bufferView < 0) ||
			(accessor.bufferView < 0 && accessor.byteOffset != 0))
		{
			return false;
		}

		const int32_t componentSize = tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
		const int32_t componentCount = tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
		if (componentSize <= 0 || componentCount <= 0)
		{
			return false;
		}

		const size_t elementSize = static_cast<size_t>(componentSize) * static_cast<size_t>(componentCount);
		size_t stride = elementSize;
		const uint8_t* data = nullptr;
		if (accessor.bufferView >= 0)
		{
			if (static_cast<size_t>(accessor.bufferView) >= model.bufferViews.size())
			{
				return false;
			}

			const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
			const int32_t accessorStride = accessor.ByteStride(bufferView);
			if (accessorStride <= 0 || static_cast<size_t>(accessorStride) < elementSize)
			{
				return false;
			}

			stride = static_cast<size_t>(accessorStride);
			if (!TryGetBufferViewData(
					model, accessor.bufferView, accessor.byteOffset, stride, accessor.count, elementSize, data))
			{
				return false;
			}
		}

		outView.m_accessor = &accessor;
		outView.m_data = data;
		outView.m_stride = stride;
		outView.m_componentSize = static_cast<size_t>(componentSize);
		outView.m_componentCount = static_cast<size_t>(componentCount);

		if (!accessor.sparse.isSparse)
		{
			return true;
		}

		if (accessor.sparse.count <= 0 || static_cast<size_t>(accessor.sparse.count) > accessor.count)
		{
			return false;
		}

		const int32_t sparseIndexSize =
			tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.sparse.indices.componentType));
		if (sparseIndexSize <= 0 ||
			(accessor.sparse.indices.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
				accessor.sparse.indices.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
				accessor.sparse.indices.componentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT))
		{
			return false;
		}

		const size_t sparseCount = static_cast<size_t>(accessor.sparse.count);
		if (accessor.sparse.indices.bufferView < 0 || accessor.sparse.values.bufferView < 0 ||
			static_cast<size_t>(accessor.sparse.indices.bufferView) >= model.bufferViews.size() ||
			static_cast<size_t>(accessor.sparse.values.bufferView) >= model.bufferViews.size())
		{
			return false;
		}

		const tinygltf::BufferView& sparseIndicesView = model.bufferViews[accessor.sparse.indices.bufferView];
		const tinygltf::BufferView& sparseValuesView = model.bufferViews[accessor.sparse.values.bufferView];
		const size_t sparseIndexStride = sparseIndicesView.byteStride > 0
											 ? static_cast<size_t>(sparseIndicesView.byteStride)
											 : static_cast<size_t>(sparseIndexSize);
		const size_t sparseValueStride =
			sparseValuesView.byteStride > 0 ? static_cast<size_t>(sparseValuesView.byteStride) : elementSize;
		if (sparseIndexStride < static_cast<size_t>(sparseIndexSize) || sparseValueStride < elementSize ||
			!TryGetBufferViewData(model,
				accessor.sparse.indices.bufferView,
				accessor.sparse.indices.byteOffset,
				sparseIndexStride,
				sparseCount,
				static_cast<size_t>(sparseIndexSize),
				outView.m_sparseIndices) ||
			!TryGetBufferViewData(model,
				accessor.sparse.values.bufferView,
				accessor.sparse.values.byteOffset,
				sparseValueStride,
				sparseCount,
				elementSize,
				outView.m_sparseValues))
		{
			return false;
		}

		outView.m_sparseCount = sparseCount;
		outView.m_sparseIndexStride = sparseIndexStride;
		outView.m_sparseValueStride = sparseValueStride;
		outView.m_sparseIndexComponentType = accessor.sparse.indices.componentType;

		uint32_t previousIndex = 0;
		for (size_t i = 0; i < sparseCount; ++i)
		{
			uint32_t sparseIndex = 0;
			if (!TryReadSparseIndex(outView, i, sparseIndex) || sparseIndex >= accessor.count ||
				(i > 0 && sparseIndex <= previousIndex))
			{
				return false;
			}
			previousIndex = sparseIndex;
		}

		return true;
	}

	static const uint8_t* GetAccessorElementData(const GltfAccessorView& view, size_t elementIndex)
	{
		size_t left = 0;
		size_t right = view.m_sparseCount;
		while (left < right)
		{
			const size_t middle = left + (right - left) / 2;
			uint32_t sparseIndex = 0;
			if (!TryReadSparseIndex(view, middle, sparseIndex))
			{
				return nullptr;
			}

			if (sparseIndex < elementIndex)
			{
				left = middle + 1;
			}
			else
			{
				right = middle;
			}
		}

		if (left < view.m_sparseCount)
		{
			uint32_t sparseIndex = 0;
			if (TryReadSparseIndex(view, left, sparseIndex) && sparseIndex == elementIndex)
			{
				return view.m_sparseValues + left * view.m_sparseValueStride;
			}
		}

		return view.m_data != nullptr ? view.m_data + elementIndex * view.m_stride : nullptr;
	}

	float ReadAccessorFloat(const GltfAccessorView& view, size_t elementIndex, size_t componentIndex)
	{
		const uint8_t* elementData = GetAccessorElementData(view, elementIndex);
		if (elementData == nullptr)
		{
			return 0.0f;
		}
		const uint8_t* data = elementData + componentIndex * view.m_componentSize;
		const bool bNormalized = view.m_accessor->normalized;

		switch (view.m_accessor->componentType)
		{
		case TINYGLTF_COMPONENT_TYPE_BYTE:
		{
			const int8_t value = ReadUnaligned<int8_t>(data);
			return bNormalized ? (std::max)(static_cast<float>(value) / 127.0f, -1.0f) : static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
		{
			const uint8_t value = ReadUnaligned<uint8_t>(data);
			return bNormalized ? static_cast<float>(value) / 255.0f : static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_SHORT:
		{
			const int16_t value = ReadUnaligned<int16_t>(data);
			return bNormalized ? (std::max)(static_cast<float>(value) / 32767.0f, -1.0f) : static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
		{
			const uint16_t value = ReadUnaligned<uint16_t>(data);
			return bNormalized ? static_cast<float>(value) / 65535.0f : static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_INT:
		{
			const int32_t value = ReadUnaligned<int32_t>(data);
			return bNormalized ? static_cast<float>((std::max)(static_cast<double>(value) / 2147483647.0, -1.0))
							   : static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
		{
			const uint32_t value = ReadUnaligned<uint32_t>(data);
			return bNormalized ? static_cast<float>(static_cast<double>(value) / 4294967295.0)
							   : static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_FLOAT:
			return ReadUnaligned<float>(data);
		case TINYGLTF_COMPONENT_TYPE_DOUBLE:
			return static_cast<float>(ReadUnaligned<double>(data));
		default:
			return 0.0f;
		}
	}
	bool TryReadAccessorIndex(const GltfAccessorView& view, size_t elementIndex, uint32_t& outIndex)
	{
		const uint8_t* data = GetAccessorElementData(view, elementIndex);
		if (data == nullptr)
		{
			outIndex = 0;
			return true;
		}
		switch (view.m_accessor->componentType)
		{
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
			outIndex = ReadUnaligned<uint8_t>(data);
			return true;
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
			outIndex = ReadUnaligned<uint16_t>(data);
			return true;
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
			outIndex = ReadUnaligned<uint32_t>(data);
			return true;
		default:
			return false;
		}
	}

	bool IsFloatAccessor(const tinygltf::Accessor& accessor)
	{
		return accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT && !accessor.normalized;
	}

	bool IsPositionAccessorSupported(const tinygltf::Accessor& accessor, bool bHasMeshQuantization)
	{
		if (IsFloatAccessor(accessor))
		{
			return true;
		}

		return bHasMeshQuantization && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
	}

	bool IsDirectionAccessorSupported(const tinygltf::Accessor& accessor, bool bHasMeshQuantization)
	{
		return IsFloatAccessor(accessor) || (bHasMeshQuantization && accessor.normalized &&
												(accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
													accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT));
	}

	bool IsTexcoordAccessorSupported(const tinygltf::Accessor& accessor, bool bHasMeshQuantization)
	{
		if (IsFloatAccessor(accessor))
		{
			return true;
		}

		if (accessor.normalized && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
									   accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT))
		{
			return true;
		}

		return bHasMeshQuantization && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
	}

	bool IsColorAccessorSupported(const tinygltf::Accessor& accessor)
	{
		return IsFloatAccessor(accessor) ||
			   (accessor.normalized && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT));
	}

	bool IsJointsAccessorSupported(const tinygltf::Accessor& accessor)
	{
		return !accessor.normalized && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
	}

	bool IsWeightsAccessorSupported(const tinygltf::Accessor& accessor)
	{
		return IsFloatAccessor(accessor) ||
			   (accessor.normalized && (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
										   accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT));
	}
}
