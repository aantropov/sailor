#include <filesystem>
#include <fstream>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "ModelImporter.h"
#include "GltfImporterUtils.h"
#include "AssetRegistry/FileId.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Material/MaterialImporter.h"
#include "AssetRegistry/Model/GeneratedModelAssetMetadata.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "ModelAssetInfo.h"
#include "Core/Utils.h"
#include "YamlExceptionBoundary.h"
#include "Math/Math.h"
#include "RHI/VertexDescription.h"
#include "RHI/Types.h"
#include "RHI/Renderer.h"
#include "Raytracing/MaterialUtils.h"
#include "Raytracing/PathTracer.h"
#include "Memory/ObjectAllocator.hpp"
#include "Memory/UniquePtr.hpp"
#include "Workspace/WorkspaceCacheContract.h"

#ifndef TINYGLTF_IMPLEMENTATION
#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>
#endif

#if defined(SAILOR_HAS_DRACO)
#include <draco/compression/decode.h>
#endif

#if defined(SAILOR_HAS_MESHOPT)
#include <meshoptimizer.h>
#endif

using namespace Sailor;

namespace
{
	constexpr uint64_t MaxMeshoptPlaceholderBytes = 256ull * 1024ull * 1024ull;
	constexpr size_t MaxFingerprintDecodedImageBytes =
		256ull * 1024ull * 1024ull;
	constexpr size_t MaxFingerprintEncodedImageBytes =
		4ull * 1024ull * 1024ull;
	constexpr int32_t MaxFingerprintTextureDimension = 256;
	constexpr int32_t FingerprintImageDimension = 256;

	bool IsFiniteGltfMatrix(const glm::mat4& matrix)
	{
		for (int32_t column = 0; column < 4; ++column)
		{
			if (!Math::AllFinite(matrix[column]))
			{
				return false;
			}
		}

		return true;
	}

	bool IsFiniteGltfMatrix(const glm::mat3& matrix)
	{
		for (int32_t column = 0; column < 3; ++column)
		{
			if (!Math::AllFinite(matrix[column]))
			{
				return false;
			}
		}

		return true;
	}

	struct FingerprintRequest
	{
		uint64_t m_generation = 0;
		FileRevision m_sourceRevision{};
	};

	struct FingerprintTaskChain
	{
		std::mutex m_mutex;
		uint64_t m_nextRequestGeneration = 0;
		TMap<FileId, FingerprintRequest> m_requests;
	};

	struct ActiveFingerprintRequest
	{
		uint64_t m_generation = 0;
		FileRevision m_sourceRevision{};
		bool m_bValid = false;
	};

	thread_local ActiveFingerprintRequest g_activeFingerprintRequest;

	class ActiveFingerprintRequestScope final
	{
	public:
		ActiveFingerprintRequestScope(
			uint64_t generation,
			const FileRevision& sourceRevision) :
			m_previous(g_activeFingerprintRequest)
		{
			g_activeFingerprintRequest = {
				generation,
				sourceRevision,
				true
			};
		}

		~ActiveFingerprintRequestScope()
		{
			g_activeFingerprintRequest = m_previous;
		}

	private:
		ActiveFingerprintRequest m_previous{};
	};

	FingerprintTaskChain& GetFingerprintTaskChain()
	{
		static FingerprintTaskChain taskChain;
		return taskChain;
	}

	class StbiImageData final
	{
	public:
		explicit StbiImageData(stbi_uc* data = nullptr) noexcept :
			m_data(data)
		{
		}

		~StbiImageData()
		{
			Clear();
		}

		StbiImageData(const StbiImageData&) = delete;
		StbiImageData& operator=(const StbiImageData&) = delete;

		stbi_uc* GetData() const noexcept
		{
			return m_data;
		}

		explicit operator bool() const noexcept
		{
			return m_data != nullptr;
		}

		void Clear() noexcept
		{
			if (m_data != nullptr)
			{
				stbi_image_free(m_data);
				m_data = nullptr;
			}
		}

	private:
		stbi_uc* m_data = nullptr;
	};

	std::filesystem::path GetFingerprintPath(const FileId& fileId)
	{
		const std::filesystem::path filename = fileId.ToString() + ".png";
		if (!fileId || filename != filename.filename())
		{
			return {};
		}

		return std::filesystem::path(AssetRegistry::GetCacheFolder()) /
			"Fingerprints" /
			filename;
	}

	bool IsFingerprintRequestCurrent(
		const FileId& fileId,
		uint64_t generation,
		const FileRevision& sourceRevision)
	{
		FingerprintTaskChain& taskChain = GetFingerprintTaskChain();
		const std::lock_guard<std::mutex> lock(taskChain.m_mutex);
		FingerprintRequest* request = nullptr;
		return taskChain.m_requests.Find(fileId, request) &&
			request != nullptr &&
			request->m_generation == generation &&
			request->m_sourceRevision == sourceRevision;
	}

	struct EncodedFingerprint
	{
		TVector<uint8_t> m_bytes;
		bool m_bValid = true;
	};

	void AppendEncodedFingerprintBytes(
		void* context,
		void* data,
		int32_t size) noexcept
	{
		EncodedFingerprint* encoded =
			static_cast<EncodedFingerprint*>(context);
		if (encoded == nullptr ||
			!encoded->m_bValid ||
			data == nullptr ||
			size <= 0 ||
			encoded->m_bytes.Num() > MaxFingerprintEncodedImageBytes ||
			static_cast<size_t>(size) >
				MaxFingerprintEncodedImageBytes -
				encoded->m_bytes.Num())
		{
			if (encoded != nullptr)
			{
				encoded->m_bValid = false;
			}
			return;
		}

		const size_t previousSize = encoded->m_bytes.Num();
		encoded->m_bytes.Resize(
			previousSize + static_cast<size_t>(size));
		std::memcpy(
			encoded->m_bytes.GetData() + previousSize,
			data,
			static_cast<size_t>(size));
	}

	bool TryMultiplySize(size_t lhs, size_t rhs, size_t& outResult)
	{
		if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs)
		{
			return false;
		}

		outResult = lhs * rhs;
		return true;
	}

	bool TryCalculateFingerprintTextureSize(
		int32_t sourceWidth,
		int32_t sourceHeight,
		int32_t& outWidth,
		int32_t& outHeight,
		size_t& outBytes)
	{
		if (sourceWidth <= 0 || sourceHeight <= 0)
		{
			return false;
		}

		outWidth = sourceWidth;
		outHeight = sourceHeight;
		if (sourceWidth > MaxFingerprintTextureDimension ||
			sourceHeight > MaxFingerprintTextureDimension)
		{
			if (sourceWidth >= sourceHeight)
			{
				outWidth = MaxFingerprintTextureDimension;
				outHeight = static_cast<int32_t>((std::max)(
					uint64_t{ 1 },
					(static_cast<uint64_t>(sourceHeight) *
						MaxFingerprintTextureDimension +
						static_cast<uint64_t>(sourceWidth) / 2) /
						static_cast<uint64_t>(sourceWidth)));
			}
			else
			{
				outHeight = MaxFingerprintTextureDimension;
				outWidth = static_cast<int32_t>((std::max)(
					uint64_t{ 1 },
					(static_cast<uint64_t>(sourceWidth) *
						MaxFingerprintTextureDimension +
						static_cast<uint64_t>(sourceHeight) / 2) /
						static_cast<uint64_t>(sourceHeight)));
			}
		}

		size_t pixelCount = 0;
		return outWidth > 0 && outHeight > 0 &&
			outWidth <= MaxFingerprintTextureDimension &&
			outHeight <= MaxFingerprintTextureDimension &&
			TryMultiplySize(
				static_cast<size_t>(outWidth),
				static_cast<size_t>(outHeight),
				pixelCount) &&
			TryMultiplySize(pixelCount, sizeof(u8vec4), outBytes);
	}

	bool LoadAsciiWithMeshoptPlaceholders(
		tinygltf::TinyGLTF& loader,
		tinygltf::Model& model,
		std::string& error,
		std::string& warning,
		const std::string& assetFilepath,
		bool& outHandled)
	{
		outHandled = false;
		std::ifstream input(assetFilepath, std::ios::binary);
		if (!input.is_open())
		{
			return false;
		}

		const std::string source{
			std::istreambuf_iterator<char>(input),
			std::istreambuf_iterator<char>()};
		if (source.find("meshopt_compression") == std::string::npos)
		{
			return false;
		}

		nlohmann::json document = nlohmann::json::parse(
			source,
			nullptr,
			false);
		if (document.is_discarded())
		{
			error = "Cannot parse glTF JSON for meshopt fallback.";
			return false;
		}

		if (!document.contains("buffers") ||
			!document["buffers"].is_array())
		{
			return false;
		}

		TVector<size_t> placeholderBuffers;
		auto& buffers = document["buffers"];
		for (size_t i = 0; i < buffers.size(); ++i)
		{
			auto& buffer = buffers[i];
			if (!buffer.is_object() || buffer.contains("uri") ||
				!buffer.contains("byteLength") ||
				!buffer["byteLength"].is_number_integer() ||
				!buffer.contains("extensions") ||
				!buffer["extensions"].is_object())
			{
				continue;
			}

			const auto& extensions = buffer["extensions"];
			const nlohmann::json* meshoptExtension = nullptr;
			if (extensions.contains("EXT_meshopt_compression"))
			{
				meshoptExtension = &extensions["EXT_meshopt_compression"];
			}
			else if (extensions.contains("KHR_meshopt_compression"))
			{
				meshoptExtension = &extensions["KHR_meshopt_compression"];
			}

			if (meshoptExtension == nullptr ||
				!meshoptExtension->is_object() ||
				!meshoptExtension->contains("fallback") ||
				!(*meshoptExtension)["fallback"].is_boolean() ||
				!(*meshoptExtension)["fallback"].get<bool>())
			{
				continue;
			}

			uint64_t byteLength = 0;
			if (buffer["byteLength"].is_number_unsigned())
			{
				byteLength = buffer["byteLength"].get<uint64_t>();
			}
			else
			{
				const int64_t signedByteLength =
					buffer["byteLength"].get<int64_t>();
				if (signedByteLength > 0)
				{
					byteLength = static_cast<uint64_t>(signedByteLength);
				}
			}

			if (byteLength == 0 ||
				byteLength > MaxMeshoptPlaceholderBytes ||
				byteLength > std::numeric_limits<unsigned int>::max())
			{
				error = "Invalid meshopt fallback buffer size.";
				outHandled = true;
				return false;
			}

			TVector<uint8_t> placeholder;
			placeholder.Resize(static_cast<size_t>(byteLength));
			buffer["uri"] =
				"data:application/octet-stream;base64," +
				tinygltf::base64_encode(
					placeholder.GetData(),
					static_cast<unsigned int>(placeholder.Num()));
			placeholderBuffers.Add(i);
		}

		if (placeholderBuffers.IsEmpty())
		{
			return false;
		}

		outHandled = true;
		const std::string patchedSource = document.dump();
		if (patchedSource.size() > std::numeric_limits<unsigned int>::max())
		{
			error = "Patched glTF document is too large.";
			return false;
		}

		const bool loaded = loader.LoadASCIIFromString(
			&model,
			&error,
			&warning,
			patchedSource.data(),
			static_cast<unsigned int>(patchedSource.size()),
			std::filesystem::path(assetFilepath).parent_path().string());
		if (loaded)
		{
			for (size_t bufferIndex : placeholderBuffers)
			{
				if (bufferIndex < model.buffers.size())
				{
					model.buffers[bufferIndex].data.clear();
				}
			}
		}

		return loaded;
	}

	struct GltfAccessorView
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

	bool IsAccessorRangeValid(
		size_t offset,
		size_t stride,
		size_t count,
		size_t elementSize,
		size_t bufferSize)
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

		return stride > 0 &&
			count - 1 <= (bufferSize - offset - elementSize) / stride;
	}

	bool HasUnsupportedBufferViewCompression(
		const tinygltf::BufferView& bufferView)
	{
		return bufferView.extensions.find("EXT_meshopt_compression") !=
				bufferView.extensions.end() ||
			bufferView.extensions.find("KHR_meshopt_compression") !=
				bufferView.extensions.end();
	}

	bool TryGetBufferViewData(
		const tinygltf::Model& model,
		int32_t bufferViewIndex,
		size_t byteOffset,
		size_t stride,
		size_t count,
		size_t elementSize,
		const uint8_t*& outData)
	{
		outData = nullptr;
		if (bufferViewIndex < 0 ||
			static_cast<size_t>(bufferViewIndex) >= model.bufferViews.size())
		{
			return false;
		}

		const tinygltf::BufferView& bufferView =
			model.bufferViews[bufferViewIndex];
		if (bufferView.buffer < 0 ||
			static_cast<size_t>(bufferView.buffer) >= model.buffers.size() ||
			HasUnsupportedBufferViewCompression(bufferView) ||
			!IsAccessorRangeValid(
				byteOffset,
				stride,
				count,
				elementSize,
				bufferView.byteLength))
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

	template<typename T>
	T ReadUnaligned(const uint8_t* data)
	{
		T value{};
		std::memcpy(&value, data, sizeof(T));
		return value;
	}

	bool TryReadSparseIndex(
		const GltfAccessorView& view,
		size_t sparseElement,
		uint32_t& outIndex)
	{
		const uint8_t* data = view.m_sparseIndices +
			sparseElement * view.m_sparseIndexStride;
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

	bool TryGetAccessorView(
		const tinygltf::Model& model,
		int32_t accessorIndex,
		int32_t expectedType,
		size_t requiredCount,
		GltfAccessorView& outView)
	{
		outView = {};
		if (accessorIndex < 0 ||
			static_cast<size_t>(accessorIndex) >= model.accessors.size())
		{
			return false;
		}

		const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
		if (accessor.count == 0 ||
			accessor.count < requiredCount ||
			(expectedType >= 0 && accessor.type != expectedType) ||
			(!accessor.sparse.isSparse && accessor.bufferView < 0) ||
			(accessor.bufferView < 0 && accessor.byteOffset != 0))
		{
			return false;
		}

		const int32_t componentSize = tinygltf::GetComponentSizeInBytes(
			static_cast<uint32_t>(accessor.componentType));
		const int32_t componentCount = tinygltf::GetNumComponentsInType(
			static_cast<uint32_t>(accessor.type));
		if (componentSize <= 0 || componentCount <= 0)
		{
			return false;
		}

		const size_t elementSize =
			static_cast<size_t>(componentSize) *
			static_cast<size_t>(componentCount);
		size_t stride = elementSize;
		const uint8_t* data = nullptr;
		if (accessor.bufferView >= 0)
		{
			if (static_cast<size_t>(accessor.bufferView) >=
				model.bufferViews.size())
			{
				return false;
			}

			const tinygltf::BufferView& bufferView =
				model.bufferViews[accessor.bufferView];
			const int32_t accessorStride = accessor.ByteStride(bufferView);
			if (accessorStride <= 0 ||
				static_cast<size_t>(accessorStride) < elementSize)
			{
				return false;
			}

			stride = static_cast<size_t>(accessorStride);
			if (!TryGetBufferViewData(
					model,
					accessor.bufferView,
					accessor.byteOffset,
					stride,
					accessor.count,
					elementSize,
					data))
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

		if (accessor.sparse.count <= 0 ||
			static_cast<size_t>(accessor.sparse.count) > accessor.count)
		{
			return false;
		}

		const int32_t sparseIndexSize = tinygltf::GetComponentSizeInBytes(
			static_cast<uint32_t>(accessor.sparse.indices.componentType));
		if (sparseIndexSize <= 0 ||
			(accessor.sparse.indices.componentType !=
				TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
			 accessor.sparse.indices.componentType !=
				TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
			 accessor.sparse.indices.componentType !=
				TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT))
		{
			return false;
		}

		const size_t sparseCount =
			static_cast<size_t>(accessor.sparse.count);
		if (accessor.sparse.indices.bufferView < 0 ||
			accessor.sparse.values.bufferView < 0 ||
			static_cast<size_t>(accessor.sparse.indices.bufferView) >=
				model.bufferViews.size() ||
			static_cast<size_t>(accessor.sparse.values.bufferView) >=
				model.bufferViews.size())
		{
			return false;
		}

		const tinygltf::BufferView& sparseIndicesView =
			model.bufferViews[accessor.sparse.indices.bufferView];
		const tinygltf::BufferView& sparseValuesView =
			model.bufferViews[accessor.sparse.values.bufferView];
		const size_t sparseIndexStride = sparseIndicesView.byteStride > 0 ?
			static_cast<size_t>(sparseIndicesView.byteStride) :
			static_cast<size_t>(sparseIndexSize);
		const size_t sparseValueStride = sparseValuesView.byteStride > 0 ?
			static_cast<size_t>(sparseValuesView.byteStride) : elementSize;
		if (sparseIndexStride < static_cast<size_t>(sparseIndexSize) ||
			sparseValueStride < elementSize ||
			!TryGetBufferViewData(
				model,
				accessor.sparse.indices.bufferView,
				accessor.sparse.indices.byteOffset,
				sparseIndexStride,
				sparseCount,
				static_cast<size_t>(sparseIndexSize),
				outView.m_sparseIndices) ||
			!TryGetBufferViewData(
				model,
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
		outView.m_sparseIndexComponentType =
			accessor.sparse.indices.componentType;

		uint32_t previousIndex = 0;
		for (size_t i = 0; i < sparseCount; ++i)
		{
			uint32_t sparseIndex = 0;
			if (!TryReadSparseIndex(outView, i, sparseIndex) ||
				sparseIndex >= accessor.count ||
				(i > 0 && sparseIndex <= previousIndex))
			{
				return false;
			}
			previousIndex = sparseIndex;
		}

		return true;
	}

	const uint8_t* GetAccessorElementData(
		const GltfAccessorView& view,
		size_t elementIndex)
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
			if (TryReadSparseIndex(view, left, sparseIndex) &&
				sparseIndex == elementIndex)
			{
				return view.m_sparseValues + left * view.m_sparseValueStride;
			}
		}

		return view.m_data != nullptr ?
			view.m_data + elementIndex * view.m_stride : nullptr;
	}

	float ReadAccessorFloat(
		const GltfAccessorView& view,
		size_t elementIndex,
		size_t componentIndex)
	{
		const uint8_t* elementData = GetAccessorElementData(view, elementIndex);
		if (elementData == nullptr)
		{
			return 0.0f;
		}
		const uint8_t* data =
			elementData + componentIndex * view.m_componentSize;
		const bool bNormalized = view.m_accessor->normalized;

		switch (view.m_accessor->componentType)
		{
		case TINYGLTF_COMPONENT_TYPE_BYTE:
		{
			const int8_t value = ReadUnaligned<int8_t>(data);
			return bNormalized ?
				(std::max)(static_cast<float>(value) / 127.0f, -1.0f) :
				static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
		{
			const uint8_t value = ReadUnaligned<uint8_t>(data);
			return bNormalized ?
				static_cast<float>(value) / 255.0f :
				static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_SHORT:
		{
			const int16_t value = ReadUnaligned<int16_t>(data);
			return bNormalized ?
				(std::max)(static_cast<float>(value) / 32767.0f, -1.0f) :
				static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
		{
			const uint16_t value = ReadUnaligned<uint16_t>(data);
			return bNormalized ?
				static_cast<float>(value) / 65535.0f :
				static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_INT:
		{
			const int32_t value = ReadUnaligned<int32_t>(data);
			return bNormalized ?
				static_cast<float>((std::max)(
					static_cast<double>(value) / 2147483647.0,
					-1.0)) :
				static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
		{
			const uint32_t value = ReadUnaligned<uint32_t>(data);
			return bNormalized ?
				static_cast<float>(
					static_cast<double>(value) / 4294967295.0) :
				static_cast<float>(value);
		}
		case TINYGLTF_COMPONENT_TYPE_FLOAT:
			return ReadUnaligned<float>(data);
		case TINYGLTF_COMPONENT_TYPE_DOUBLE:
			return static_cast<float>(ReadUnaligned<double>(data));
		default:
			return 0.0f;
		}
	}

	bool TryReadAccessorIndex(
		const GltfAccessorView& view,
		size_t elementIndex,
		uint32_t& outIndex)
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

#if defined(SAILOR_HAS_DRACO)
	bool IsDracoAttributeLayoutValid(
		const draco::PointAttribute& attribute)
	{
		const int32_t dataTypeLength =
			draco::DataTypeLength(attribute.data_type());
		const draco::DataBuffer* buffer = attribute.buffer();
		const int64_t byteOffset = attribute.byte_offset();
		const int64_t byteStride = attribute.byte_stride();
		if (dataTypeLength <= 0 || buffer == nullptr ||
			attribute.num_components() == 0 || attribute.size() == 0 ||
			byteOffset < 0 || byteStride < 0)
		{
			return false;
		}

		const uint64_t elementSize =
			static_cast<uint64_t>(dataTypeLength) *
			static_cast<uint64_t>(attribute.num_components());
		const uint64_t offset = static_cast<uint64_t>(byteOffset);
		const uint64_t stride = static_cast<uint64_t>(byteStride);
		const uint64_t lastIndex =
			static_cast<uint64_t>(attribute.size() - 1);
		if (stride < elementSize ||
			lastIndex > (std::numeric_limits<uint64_t>::max() - offset) /
				stride)
		{
			return false;
		}

		const uint64_t lastElementOffset = offset + lastIndex * stride;
		if (elementSize >
				std::numeric_limits<uint64_t>::max() - lastElementOffset ||
			lastElementOffset + elementSize >
				static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
			lastElementOffset + elementSize >
				static_cast<uint64_t>(buffer->data_size()))
		{
			return false;
		}

		return true;
	}

	template<typename T>
	bool DecodeDracoIndices(
		const draco::Mesh& mesh,
		size_t pointCount,
		TVector<uint8_t>& outData)
	{
		const size_t indexCount =
			static_cast<size_t>(mesh.num_faces()) * 3;
		outData.Resize(indexCount * sizeof(T));
		size_t outputIndex = 0;
		for (draco::FaceIndex faceIndex(0);
			faceIndex < mesh.num_faces();
			++faceIndex)
		{
			const draco::Mesh::Face& face = mesh.face(faceIndex);
			for (size_t corner = 0; corner < 3; ++corner)
			{
				const uint64_t index =
					static_cast<uint64_t>(face[corner].value());
				if (index >= pointCount ||
					index > std::numeric_limits<T>::max())
				{
					return false;
				}

				const T value = static_cast<T>(index);
				std::memcpy(
					outData.GetData() + outputIndex * sizeof(T),
					&value,
					sizeof(value));
				++outputIndex;
			}
		}

		return true;
	}

	template<typename T>
	bool DecodeDracoAttribute(
		const draco::Mesh& mesh,
		const draco::PointAttribute& attribute,
		size_t componentCount,
		TVector<uint8_t>& outData)
	{
		const size_t pointCount =
			static_cast<size_t>(mesh.num_points());
		if (!attribute.is_mapping_identity() &&
			attribute.indices_map_size() < pointCount)
		{
			return false;
		}
		if (!IsDracoAttributeLayoutValid(attribute))
		{
			return false;
		}

		const size_t elementSize = componentCount * sizeof(T);
		outData.Resize(pointCount * elementSize);
		std::array<T, 4> values{};
		for (size_t point = 0; point < pointCount; ++point)
		{
			const draco::AttributeValueIndex valueIndex =
				attribute.mapped_index(draco::PointIndex(
					static_cast<draco::PointIndex::ValueType>(point)));
			const uint64_t rawValueIndex =
				static_cast<uint64_t>(valueIndex.value());
			if (rawValueIndex >= attribute.size() ||
				!attribute.ConvertValue<T>(
					valueIndex,
					static_cast<int8_t>(componentCount),
					values.data()))
			{
				return false;
			}

			std::memcpy(
				outData.GetData() + point * elementSize,
				values.data(),
				elementSize);
		}

		return true;
	}

	bool DecodeDracoAttribute(
		int32_t componentType,
		const draco::Mesh& mesh,
		const draco::PointAttribute& attribute,
		size_t componentCount,
		TVector<uint8_t>& outData)
	{
		switch (componentType)
		{
		case TINYGLTF_COMPONENT_TYPE_BYTE:
			return DecodeDracoAttribute<int8_t>(
				mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
			return DecodeDracoAttribute<uint8_t>(
				mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_SHORT:
			return DecodeDracoAttribute<int16_t>(
				mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
			return DecodeDracoAttribute<uint16_t>(
				mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
			return DecodeDracoAttribute<uint32_t>(
				mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_FLOAT:
			return DecodeDracoAttribute<float>(
				mesh, attribute, componentCount, outData);
		default:
			return false;
		}
	}
#endif

	bool DecodeDracoPrimitives(
		tinygltf::Model& model,
		std::string& outError)
	{
		outError.clear();
#if defined(SAILOR_HAS_DRACO)
		TMap<size_t, TUniquePtr<draco::Mesh>> decodedMeshes;
#endif
		for (tinygltf::Mesh& gltfMesh : model.meshes)
		{
			for (tinygltf::Primitive& primitive : gltfMesh.primitives)
			{
				auto extensionIt = primitive.extensions.find(
					"KHR_draco_mesh_compression");
				if (extensionIt == primitive.extensions.end())
				{
					continue;
				}

#if defined(SAILOR_HAS_DRACO)
				const tinygltf::Value& extension = extensionIt->second;
				if (primitive.mode != TINYGLTF_MODE_TRIANGLES ||
					primitive.indices < 0 ||
					!extension.IsObject() ||
					!extension.Has("bufferView") ||
					!extension.Get("bufferView").IsInt() ||
					extension.Get("bufferView").Get<int>() < 0 ||
					!extension.Has("attributes") ||
					!extension.Get("attributes").IsObject())
				{
					outError = "Invalid Draco primitive metadata.";
					return false;
				}

				const size_t compressedViewIndex = static_cast<size_t>(
					extension.Get("bufferView").Get<int>());
				if (compressedViewIndex >= model.bufferViews.size())
				{
					outError = "Draco buffer view is out of range.";
					return false;
				}

				const tinygltf::BufferView& compressedView =
					model.bufferViews[compressedViewIndex];
				if (compressedView.buffer < 0 ||
					static_cast<size_t>(compressedView.buffer) >=
						model.buffers.size())
				{
					outError = "Draco source buffer is out of range.";
					return false;
				}

				const tinygltf::Buffer& compressedBuffer =
					model.buffers[compressedView.buffer];
				if (compressedView.byteLength == 0 ||
					compressedView.byteLength > static_cast<size_t>(
						std::numeric_limits<int64_t>::max()) ||
					compressedView.byteOffset > compressedBuffer.data.size() ||
					compressedView.byteLength >
						compressedBuffer.data.size() - compressedView.byteOffset)
				{
					outError = "Draco source data is out of range.";
					return false;
				}

				TUniquePtr<draco::Mesh>* decodedMeshPtr = nullptr;
				if (!decodedMeshes.Find(
					compressedViewIndex,
					decodedMeshPtr))
				{
					draco::DecoderBuffer decoderBuffer;
					decoderBuffer.Init(
						reinterpret_cast<const char*>(
							compressedBuffer.data.data() +
							compressedView.byteOffset),
						compressedView.byteLength);
					draco::Decoder decoder;
					auto decodedMesh =
						TUniquePtr<draco::Mesh>::Make();
					const draco::Status decodeStatus =
						decoder.DecodeBufferToGeometry(
							&decoderBuffer,
							decodedMesh.GetRawPtr());
					if (!decodeStatus.ok())
					{
						outError = "Cannot decode Draco mesh: " +
							decodeStatus.error_msg_string();
						return false;
					}

					if (!decodedMesh)
					{
						outError = "Cannot decode Draco mesh.";
						return false;
					}

					decodedMeshes[compressedViewIndex] =
						std::move(decodedMesh);
					if (!decodedMeshes.Find(
						compressedViewIndex,
						decodedMeshPtr))
					{
						outError = "Cannot cache decoded Draco mesh.";
						return false;
					}
				}

				if (decodedMeshPtr == nullptr || !(*decodedMeshPtr))
				{
					outError = "Decoded Draco mesh is unavailable.";
					return false;
				}
				const draco::Mesh& decodedMesh =
					*decodedMeshPtr->GetRawPtr();
				const size_t pointCount =
					static_cast<size_t>(decodedMesh.num_points());
				const size_t faceCount =
					static_cast<size_t>(decodedMesh.num_faces());
				if (pointCount == 0 || faceCount == 0 ||
					pointCount > std::numeric_limits<uint32_t>::max() ||
					faceCount > std::numeric_limits<size_t>::max() / 3)
				{
					outError = "Invalid decoded Draco mesh size.";
					return false;
				}

				if (primitive.indices >= 0)
				{
					if (static_cast<size_t>(primitive.indices) >=
						model.accessors.size())
					{
						outError = "Draco index accessor is out of range.";
						return false;
					}

					tinygltf::Accessor& accessor =
						model.accessors[primitive.indices];
					if (accessor.type != TINYGLTF_TYPE_SCALAR ||
						accessor.normalized || accessor.sparse.isSparse ||
						accessor.count != faceCount * 3)
					{
						outError = "Invalid Draco index accessor.";
						return false;
					}

					TVector<uint8_t> decodedIndices;
					bool bIndicesDecoded = false;
					switch (accessor.componentType)
					{
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
						bIndicesDecoded = DecodeDracoIndices<uint8_t>(
							decodedMesh, pointCount, decodedIndices);
						break;
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
						bIndicesDecoded = DecodeDracoIndices<uint16_t>(
							decodedMesh, pointCount, decodedIndices);
						break;
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
						bIndicesDecoded = DecodeDracoIndices<uint32_t>(
							decodedMesh, pointCount, decodedIndices);
						break;
					default:
						break;
					}

					if (!bIndicesDecoded || decodedIndices.IsEmpty() ||
						decodedIndices.Num() >
							std::numeric_limits<int>::max())
					{
						outError = "Cannot decode Draco indices.";
						return false;
					}

					tinygltf::Buffer decodedBuffer;
					decodedBuffer.data.assign(
						decodedIndices.GetData(),
						decodedIndices.GetData() + decodedIndices.Num());
					model.buffers.emplace_back(std::move(decodedBuffer));
					tinygltf::BufferView decodedView;
					decodedView.buffer =
						static_cast<int>(model.buffers.size() - 1);
					decodedView.byteLength =
						model.buffers.back().data.size();
					decodedView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
					model.bufferViews.emplace_back(std::move(decodedView));
					accessor.bufferView =
						static_cast<int>(model.bufferViews.size() - 1);
					accessor.byteOffset = 0;
					accessor.count = faceCount * 3;
				}

				const tinygltf::Value::Object attributes =
					extension.Get("attributes").Get<tinygltf::Value::Object>();
				for (const auto& [semantic, attributeValue] : attributes)
				{
					if (!attributeValue.IsInt() ||
						attributeValue.Get<int>() < 0)
					{
						outError = "Invalid Draco attribute identifier.";
						return false;
					}

					const auto primitiveAttribute =
						primitive.attributes.find(semantic);
					if (primitiveAttribute == primitive.attributes.end() ||
						primitiveAttribute->second < 0 ||
						static_cast<size_t>(primitiveAttribute->second) >=
							model.accessors.size())
					{
						outError = "Draco attribute accessor is out of range.";
						return false;
					}

					tinygltf::Accessor& accessor =
						model.accessors[primitiveAttribute->second];
					const int32_t componentSize =
						tinygltf::GetComponentSizeInBytes(
							static_cast<uint32_t>(accessor.componentType));
					const int32_t componentCount =
						tinygltf::GetNumComponentsInType(
							static_cast<uint32_t>(accessor.type));
					if (componentSize <= 0 || componentCount <= 0 ||
						componentCount > 4 ||
						accessor.sparse.isSparse ||
						accessor.count != pointCount ||
						pointCount > std::numeric_limits<size_t>::max() /
							(static_cast<size_t>(componentSize) *
							 static_cast<size_t>(componentCount)))
					{
						outError = "Invalid Draco attribute accessor.";
						return false;
					}

					const draco::PointAttribute* attribute =
						decodedMesh.GetAttributeByUniqueId(static_cast<uint32_t>(
							attributeValue.Get<int>()));
					if (attribute == nullptr ||
						attribute->num_components() != componentCount)
					{
						outError = "Draco attribute is missing or incompatible.";
						return false;
					}

					TVector<uint8_t> decodedAttribute;
					if (!DecodeDracoAttribute(
							accessor.componentType,
							decodedMesh,
							*attribute,
							static_cast<size_t>(componentCount),
							decodedAttribute) ||
						decodedAttribute.IsEmpty() ||
						decodedAttribute.Num() >
							std::numeric_limits<int>::max())
					{
						outError = "Cannot decode Draco attribute.";
						return false;
					}

					tinygltf::Buffer decodedBuffer;
					decodedBuffer.data.assign(
						decodedAttribute.GetData(),
						decodedAttribute.GetData() + decodedAttribute.Num());
					model.buffers.emplace_back(std::move(decodedBuffer));
					tinygltf::BufferView decodedView;
					decodedView.buffer =
						static_cast<int>(model.buffers.size() - 1);
					decodedView.byteLength =
						model.buffers.back().data.size();
					decodedView.byteStride = 0;
					decodedView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
					model.bufferViews.emplace_back(std::move(decodedView));
					accessor.bufferView =
						static_cast<int>(model.bufferViews.size() - 1);
					accessor.byteOffset = 0;
					accessor.count = pointCount;
				}

				primitive.extensions.erase(extensionIt);
#else
				outError = "Draco decoder is unavailable.";
				return false;
#endif
			}
		}

		return true;
	}

	bool HasGltfExtension(
		const tinygltf::Model& model,
		const char* extension)
	{
		return std::find(
			model.extensionsUsed.begin(),
			model.extensionsUsed.end(),
			extension) != model.extensionsUsed.end() ||
			std::find(
				model.extensionsRequired.begin(),
				model.extensionsRequired.end(),
				extension) != model.extensionsRequired.end();
	}

	bool HasBufferViewFallback(
		const tinygltf::Model& model,
		const tinygltf::BufferView& bufferView)
	{
		if (bufferView.buffer < 0 ||
			static_cast<size_t>(bufferView.buffer) >= model.buffers.size())
		{
			return false;
		}

		const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
		return bufferView.byteOffset <= buffer.data.size() &&
			bufferView.byteLength <= buffer.data.size() - bufferView.byteOffset;
	}

	bool TryGetMeshoptSize(
		const tinygltf::Value& extension,
		const char* property,
		size_t& outValue,
		bool bRequired)
	{
		outValue = 0;
		if (!extension.IsObject() || !extension.Has(property))
		{
			return !bRequired;
		}

		const tinygltf::Value& value = extension.Get(property);
		if (!value.IsInt() || value.Get<int>() < 0)
		{
			return false;
		}

		outValue = static_cast<size_t>(value.Get<int>());
		return true;
	}

	template<typename T>
	int32_t SignExtendMeshoptColor(T value)
	{
		constexpr uint32_t numBits = sizeof(T) * 8;
		constexpr uint32_t signBit = 1u << (numBits - 1);
		constexpr uint32_t range = 1u << numBits;
		const uint32_t raw = static_cast<uint32_t>(value);
		return (raw & signBit) != 0 ?
			static_cast<int32_t>(raw - range) :
			static_cast<int32_t>(raw);
	}

	template<typename T>
	bool DecodeMeshoptColorFilter(T* data, size_t count)
	{
		const double maxValue =
			static_cast<double>(std::numeric_limits<T>::max());
		for (size_t i = 0; i < count; ++i)
		{
			const size_t offset = i * 4;
			int32_t alphaScale = static_cast<int32_t>(data[offset + 3]);
			alphaScale |= alphaScale >> 1;
			alphaScale |= alphaScale >> 2;
			alphaScale |= alphaScale >> 4;
			alphaScale |= alphaScale >> 8;
			if (alphaScale <= 0)
			{
				return false;
			}

			const int32_t y = static_cast<int32_t>(data[offset]);
			const int32_t co =
				SignExtendMeshoptColor(data[offset + 1]);
			const int32_t cg =
				SignExtendMeshoptColor(data[offset + 2]);
			const int32_t red = y + co - cg;
			const int32_t green = y + cg;
			const int32_t blue = y - co - cg;
			const int32_t encodedAlpha =
				static_cast<int32_t>(data[offset + 3]);
			const int32_t alpha =
				((encodedAlpha << 1) & alphaScale) |
				(encodedAlpha & 1);
			const double scale = maxValue / alphaScale;
			auto decodeComponent = [&](int32_t component)
				{
					const double decoded =
						static_cast<double>(component) * scale + 0.5;
					return static_cast<T>(std::clamp(
						decoded,
						0.0,
						maxValue));
				};

			data[offset] = decodeComponent(red);
			data[offset + 1] = decodeComponent(green);
			data[offset + 2] = decodeComponent(blue);
			data[offset + 3] = decodeComponent(alpha);
		}

		return true;
	}

	bool DecodeMeshoptBufferViews(
		tinygltf::Model& model,
		std::string& outError)
	{
		outError.clear();
		for (tinygltf::BufferView& bufferView : model.bufferViews)
		{
			auto extensionIt =
				bufferView.extensions.find("EXT_meshopt_compression");
			if (extensionIt == bufferView.extensions.end())
			{
				extensionIt =
					bufferView.extensions.find("KHR_meshopt_compression");
			}

			if (extensionIt == bufferView.extensions.end())
			{
				continue;
			}

			const std::string extensionName = extensionIt->first;
			const tinygltf::Value extension = extensionIt->second;
			const bool bHasFallback =
				HasBufferViewFallback(model, bufferView);
			auto useFallbackOrFail = [&](const char* reason)
				{
					if (bHasFallback)
					{
						bufferView.extensions.erase(extensionName);
						return true;
					}

					outError = reason;
					return false;
				};

			size_t sourceBufferIndex = 0;
			size_t sourceOffset = 0;
			size_t sourceLength = 0;
			size_t stride = 0;
			size_t count = 0;
			if (!TryGetMeshoptSize(
					extension,
					"buffer",
					sourceBufferIndex,
					true) ||
				!TryGetMeshoptSize(
					extension,
					"byteOffset",
					sourceOffset,
					false) ||
				!TryGetMeshoptSize(
					extension,
					"byteLength",
					sourceLength,
					true) ||
				!TryGetMeshoptSize(
					extension,
					"byteStride",
					stride,
					true) ||
				!TryGetMeshoptSize(
					extension,
					"count",
					count,
					true) ||
				sourceLength == 0 || stride == 0 || count == 0 ||
				count > std::numeric_limits<size_t>::max() / stride ||
				count * stride != bufferView.byteLength ||
				sourceBufferIndex >= model.buffers.size())
			{
				if (useFallbackOrFail("Invalid meshopt buffer-view metadata."))
				{
					continue;
				}
				return false;
			}

			const tinygltf::Buffer& sourceBuffer =
				model.buffers[sourceBufferIndex];
			if (sourceOffset > sourceBuffer.data.size() ||
				sourceLength > sourceBuffer.data.size() - sourceOffset)
			{
				if (useFallbackOrFail("Meshopt source data is out of range."))
				{
					continue;
				}
				return false;
			}

			if (!extension.Has("mode") ||
				!extension.Get("mode").IsString())
			{
				if (useFallbackOrFail("Meshopt compression mode is missing."))
				{
					continue;
				}
				return false;
			}

			const std::string& mode =
				extension.Get("mode").Get<std::string>();
			std::string filter = "NONE";
			if (extension.Has("filter"))
			{
				if (!extension.Get("filter").IsString())
				{
					if (useFallbackOrFail("Invalid meshopt compression filter."))
					{
						continue;
					}
					return false;
				}
				filter = extension.Get("filter").Get<std::string>();
			}

			const bool bAttributesLayoutValid =
				mode == "ATTRIBUTES" &&
				stride >= 4 && stride <= 256 && stride % 4 == 0;
			const bool bTrianglesLayoutValid =
				mode == "TRIANGLES" && count % 3 == 0 &&
				(stride == 2 || stride == 4);
			const bool bIndicesLayoutValid =
				mode == "INDICES" && (stride == 2 || stride == 4);
			const bool bFilterLayoutValid =
				(filter == "NONE" &&
				 (bAttributesLayoutValid ||
				  bTrianglesLayoutValid ||
				  bIndicesLayoutValid)) ||
				(mode == "ATTRIBUTES" &&
				 ((filter == "OCTAHEDRAL" && (stride == 4 || stride == 8)) ||
				  (filter == "QUATERNION" && stride == 8) ||
				  (filter == "EXPONENTIAL" && stride % 4 == 0) ||
				  (filter == "COLOR" && (stride == 4 || stride == 8))));
			if (!bFilterLayoutValid)
			{
				if (useFallbackOrFail("Invalid meshopt mode, filter, or layout."))
				{
					continue;
				}
				return false;
			}

#if defined(SAILOR_HAS_MESHOPT)
			TVector<uint8_t> decoded;
			decoded.Resize(bufferView.byteLength);
			const uint8_t* source =
				sourceBuffer.data.data() + sourceOffset;
			int decodeResult = -1;
			if (mode == "ATTRIBUTES")
			{
				decodeResult = meshopt_decodeVertexBuffer(
					decoded.GetData(),
					count,
					stride,
					source,
					sourceLength);
			}
			else if (mode == "TRIANGLES")
			{
				decodeResult = meshopt_decodeIndexBuffer(
					decoded.GetData(),
					count,
					stride,
					source,
					sourceLength);
			}
			else if (mode == "INDICES")
			{
				decodeResult = meshopt_decodeIndexSequence(
					decoded.GetData(),
					count,
					stride,
					source,
					sourceLength);
			}

			if (decodeResult != 0)
			{
				if (useFallbackOrFail("Cannot decode meshopt buffer view."))
				{
					continue;
				}
				return false;
			}

			if (filter == "OCTAHEDRAL")
			{
				meshopt_decodeFilterOct(decoded.GetData(), count, stride);
			}
			else if (filter == "QUATERNION")
			{
				meshopt_decodeFilterQuat(decoded.GetData(), count, stride);
			}
			else if (filter == "EXPONENTIAL")
			{
				meshopt_decodeFilterExp(decoded.GetData(), count, stride);
			}
			else if (filter == "COLOR")
			{
				const bool bColorDecoded = stride == 4 ?
					DecodeMeshoptColorFilter(
						reinterpret_cast<uint8_t*>(decoded.GetData()),
						count) :
					DecodeMeshoptColorFilter(
						reinterpret_cast<uint16_t*>(decoded.GetData()),
						count);
				if (!bColorDecoded)
				{
					if (useFallbackOrFail("Cannot decode meshopt color filter."))
					{
						continue;
					}
					return false;
				}
			}
			else if (filter != "NONE")
			{
				if (useFallbackOrFail("Unsupported meshopt compression filter."))
				{
					continue;
				}
				return false;
			}

			if (bufferView.buffer < 0 ||
				static_cast<size_t>(bufferView.buffer) >= model.buffers.size() ||
				bufferView.byteOffset >
					std::numeric_limits<size_t>::max() - bufferView.byteLength)
			{
				if (useFallbackOrFail("Invalid meshopt destination buffer."))
				{
					continue;
				}
				return false;
			}

			tinygltf::Buffer& destinationBuffer =
				model.buffers[bufferView.buffer];
			const size_t destinationEnd =
				bufferView.byteOffset + bufferView.byteLength;
			if (destinationBuffer.data.size() < destinationEnd)
			{
				destinationBuffer.data.resize(destinationEnd);
			}
			std::memcpy(
				destinationBuffer.data.data() + bufferView.byteOffset,
				decoded.GetData(),
				decoded.Num());
			bufferView.extensions.erase(extensionName);
#else
			if (useFallbackOrFail("Meshopt decoder is unavailable."))
			{
				continue;
			}
			return false;
#endif
		}

		return true;
	}

	bool IsFloatAccessor(const tinygltf::Accessor& accessor)
	{
		return accessor.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT &&
			!accessor.normalized;
	}

	bool IsPositionAccessorSupported(
		const tinygltf::Accessor& accessor,
		bool bHasMeshQuantization)
	{
		if (IsFloatAccessor(accessor))
		{
			return true;
		}

		return bHasMeshQuantization &&
			(accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
			 accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
			 accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT ||
			 accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
	}

	bool IsDirectionAccessorSupported(
		const tinygltf::Accessor& accessor,
		bool bHasMeshQuantization)
	{
		return IsFloatAccessor(accessor) ||
			(bHasMeshQuantization && accessor.normalized &&
			 (accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
			  accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT));
	}

	bool IsTexcoordAccessorSupported(
		const tinygltf::Accessor& accessor,
		bool bHasMeshQuantization)
	{
		if (IsFloatAccessor(accessor))
		{
			return true;
		}

		if (accessor.normalized &&
			(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
			 accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT))
		{
			return true;
		}

		return bHasMeshQuantization &&
			(accessor.componentType == TINYGLTF_COMPONENT_TYPE_BYTE ||
			 accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
			 accessor.componentType == TINYGLTF_COMPONENT_TYPE_SHORT ||
			 accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
	}

	bool IsColorAccessorSupported(const tinygltf::Accessor& accessor)
	{
		return IsFloatAccessor(accessor) ||
			(accessor.normalized &&
			 (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
			  accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT));
	}

	bool IsJointsAccessorSupported(const tinygltf::Accessor& accessor)
	{
		return !accessor.normalized &&
			(accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
			 accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
	}

	bool IsWeightsAccessorSupported(const tinygltf::Accessor& accessor)
	{
		return IsFloatAccessor(accessor) ||
			(accessor.normalized &&
			 (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE ||
			  accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT));
	}

	bool TryNormalizeDirection(
		const glm::vec3& value,
		glm::vec3& outDirection)
	{
		const glm::vec3 source = value;
		outDirection = glm::vec3(0.0f);
		if (!Math::AllFinite(source))
		{
			return false;
		}

		const float maxComponent = (std::max)(
			std::abs(source.x),
			(std::max)(std::abs(source.y), std::abs(source.z)));
		if (maxComponent <= 1e-4f)
		{
			return false;
		}

		const glm::vec3 scaled = source / maxComponent;
		const float lengthSquared = glm::dot(scaled, scaled);
		if (!std::isfinite(lengthSquared) || lengthSquared <= 0.0f)
		{
			return false;
		}

		outDirection = scaled / std::sqrt(lengthSquared);
		return Math::AllFinite(outDirection);
	}

	void SanitizeVertexFrame(
		Sailor::RHI::VertexP3N3T3B3UV2C4I4W4& vertex)
	{
		glm::vec3 normal;
		if (!TryNormalizeDirection(vertex.m_normal, normal))
		{
			normal = glm::vec3(0.0f, 1.0f, 0.0f);
		}

		glm::vec3 tangentInput;
		glm::vec3 tangent(0.0f);
		if (TryNormalizeDirection(vertex.m_tangent, tangentInput))
		{
			TryNormalizeDirection(
				tangentInput - normal * glm::dot(tangentInput, normal),
				tangent);
		}

		if (!TryNormalizeDirection(tangent, tangent))
		{
			const glm::vec3 axis = std::abs(normal.y) < 0.999f ?
				glm::vec3(0.0f, 1.0f, 0.0f) :
				glm::vec3(1.0f, 0.0f, 0.0f);
			if (!TryNormalizeDirection(glm::cross(axis, normal), tangent))
			{
				tangent = glm::vec3(1.0f, 0.0f, 0.0f);
			}
		}

		glm::vec3 sourceBitangent;
		const float handedness =
			TryNormalizeDirection(vertex.m_bitangent, sourceBitangent) &&
			glm::dot(
				glm::cross(normal, tangent),
				sourceBitangent) < 0.0f ? -1.0f : 1.0f;

		glm::vec3 bitangent;
		if (!TryNormalizeDirection(glm::cross(normal, tangent), bitangent))
		{
			bitangent = glm::vec3(0.0f, 0.0f, 1.0f);
		}

		vertex.m_normal = normal;
		vertex.m_tangent = tangent;
		vertex.m_bitangent = bitangent * handedness;
	}
}

GltfImporterUtils::MeshInstanceTransforms GltfImporterUtils::ResolveMeshInstanceTransforms(
	const MeshInstance& instance,
	float unitScale)
{
	const glm::mat4 sourceTransform = instance.m_skinIndex >= 0 ?
		glm::mat4(1.0f) : instance.m_worldTransform;
	const float directionScale = unitScale < 0.0f ? -1.0f : 1.0f;

	MeshInstanceTransforms result;
	result.m_geometryTransform =
		glm::scale(glm::mat4(1.0f), glm::vec3(unitScale)) * sourceTransform;
	result.m_directionTransform = glm::mat3(
		glm::scale(glm::mat4(1.0f), glm::vec3(directionScale)) * sourceTransform);
	const glm::mat3 sourceLinear(sourceTransform);
	result.m_bakedVolumeScale = glm::vec3(
		glm::length(sourceLinear[0]),
		glm::length(sourceLinear[1]),
		glm::length(sourceLinear[2]));
	return result;
}

GltfImporterUtils::MaterialAlphaModeSettings GltfImporterUtils::ResolveMaterialAlphaMode(
	const std::string& alphaMode,
	bool bHasTransmission)
{
	if (bHasTransmission)
	{
		return {
			"Transparent",
			false,
			alphaMode == "MASK",
			alphaMode == "BLEND" ?
				RHI::EBlendMode::AlphaBlending :
				RHI::EBlendMode::None
		};
	}

	if (alphaMode == "BLEND")
	{
		return {
			"Transparent",
			false,
			false,
			RHI::EBlendMode::AlphaBlending
		};
	}

	if (alphaMode == "MASK")
	{
		return {
			"Masked",
			true,
			true,
			RHI::EBlendMode::None
		};
	}

	return {};
}

GltfImporterUtils::MaterialTransmissionSettings GltfImporterUtils::ResolveMaterialTransmission(
	const tinygltf::Material& material,
	size_t numTextures,
	float unitScale)
{
	MaterialTransmissionSettings result;
	auto tryReadFiniteNumber = [](
		const tinygltf::Value& object,
		const char* property,
		double& outValue)
		{
			if (!object.IsObject() || !object.Has(property))
			{
				return false;
			}

			const tinygltf::Value& value = object.Get(property);
			if (!value.IsNumber())
			{
				return false;
			}

			const double parsedValue = value.GetNumberAsDouble();
			if (!std::isfinite(parsedValue) ||
				parsedValue > std::numeric_limits<float>::max() ||
				parsedValue < -std::numeric_limits<float>::max())
			{
				return false;
			}

			outValue = parsedValue;
			return true;
		};

	auto tryReadTextureIndex = [numTextures](
		const tinygltf::Value& object,
		const char* property,
		int32_t& outIndex)
		{
			if (!object.IsObject() || !object.Has(property))
			{
				return false;
			}

			const tinygltf::Value& textureInfo = object.Get(property);
			if (!textureInfo.IsObject() || !textureInfo.Has("index"))
			{
				return false;
			}

			const tinygltf::Value& indexValue = textureInfo.Get("index");
			if (!indexValue.IsInt())
			{
				return false;
			}

			const int32_t index = indexValue.GetNumberAsInt();
			if (index < 0 || static_cast<size_t>(index) >= numTextures)
			{
				return false;
			}

			outIndex = index;
			return true;
		};

	const auto extensionIt = material.extensions.find(
		"KHR_materials_transmission");
	if (extensionIt == material.extensions.end() ||
		!extensionIt->second.IsObject())
	{
		return result;
	}

	const tinygltf::Value& extension = extensionIt->second;
	double parsedValue = 0.0;
	if (tryReadFiniteNumber(
			extension,
			"transmissionFactor",
			parsedValue))
	{
		result.m_factor = static_cast<float>(std::clamp(
			parsedValue,
			0.0,
			1.0));
	}

	tryReadTextureIndex(
		extension,
		"transmissionTexture",
		result.m_textureIndex);

	const auto volumeIt = material.extensions.find(
		"KHR_materials_volume");
	if (volumeIt != material.extensions.end() &&
		volumeIt->second.IsObject())
	{
		const tinygltf::Value& volume = volumeIt->second;
		if (tryReadFiniteNumber(
				volume,
				"thicknessFactor",
				parsedValue))
		{
			result.m_thicknessFactor = static_cast<float>(
				(std::max)(0.0, parsedValue));
		}

		tryReadTextureIndex(
			volume,
			"thicknessTexture",
			result.m_thicknessTextureIndex);

		if (tryReadFiniteNumber(
				volume,
				"attenuationDistance",
				parsedValue) &&
			parsedValue > 0.0)
		{
			result.m_attenuationDistance = static_cast<float>(
				parsedValue);
		}

		if (volume.Has("attenuationColor"))
		{
			const tinygltf::Value& color = volume.Get(
				"attenuationColor");
			if (color.IsArray() && color.ArrayLen() >= 3)
			{
				glm::vec3 parsedColor(1.0f);
				bool bValidColor = true;
				for (size_t component = 0; component < 3; ++component)
				{
					const tinygltf::Value& value = color.Get(component);
					if (!value.IsNumber() ||
						!std::isfinite(value.GetNumberAsDouble()))
					{
						bValidColor = false;
						break;
					}

					parsedColor[static_cast<int32_t>(component)] =
						static_cast<float>(std::clamp(
							value.GetNumberAsDouble(),
							0.0,
							1.0));
				}

				if (bValidColor)
				{
					result.m_attenuationColor = parsedColor;
				}
			}
		}
	}

	const auto iorIt = material.extensions.find("KHR_materials_ior");
	if (iorIt != material.extensions.end() &&
		tryReadFiniteNumber(iorIt->second, "ior", parsedValue))
	{
		result.m_indexOfRefraction = static_cast<float>(
			(std::max)(1.0, parsedValue));
	}

	const double lengthScale = std::isfinite(unitScale) ?
		std::abs(static_cast<double>(unitScale)) :
		1.0;
	auto scaleLength = [lengthScale](float value)
		{
			return static_cast<float>((std::min)(
				static_cast<double>((std::numeric_limits<float>::max)()),
				static_cast<double>(value) * lengthScale));
		};

	result.m_thicknessFactor = scaleLength(result.m_thicknessFactor);
	if (result.m_attenuationDistance <
		(std::numeric_limits<float>::max)())
	{
		result.m_attenuationDistance = scaleLength(
			result.m_attenuationDistance);
	}

	return result;
}

bool GltfImporterUtils::MergeGeneratedMaterialProperties(
	YAML::Node& inOutMaterial,
	const YAML::Node& generatedProperties)
{
	if (!inOutMaterial.IsMap() || !generatedProperties.IsMap())
	{
		return false;
	}

	YAML::Node merged = YAML::Clone(inOutMaterial);
	for (const char* property : {
		"renderQueue",
		"bEnableZWrite",
		"bCustomDepthShader",
		"blendMode" })
	{
		if (!generatedProperties[property] ||
			!generatedProperties[property].IsScalar())
		{
			return false;
		}
		merged[property] = YAML::Clone(generatedProperties[property]);
	}

	auto isManagedDefine = [](const std::string& define)
		{
			return define == "TRANSMISSION" || define == "ALPHA_CUTOUT";
		};

	YAML::Node mergedDefines(YAML::NodeType::Sequence);
	const YAML::Node existingDefines = merged["defines"];
	if (existingDefines && !existingDefines.IsNull())
	{
		if (!existingDefines.IsSequence())
		{
			return false;
		}

		for (const YAML::Node& defineNode : existingDefines)
		{
			if (!defineNode.IsScalar())
			{
				return false;
			}

			const std::string define = defineNode.as<std::string>();
			if (!isManagedDefine(define))
			{
				mergedDefines.push_back(define);
			}
		}
	}

	const YAML::Node generatedDefines = generatedProperties["defines"];
	if (generatedDefines && !generatedDefines.IsNull())
	{
		if (!generatedDefines.IsSequence())
		{
			return false;
		}

		bool bHasTransmission = false;
		bool bHasAlphaCutout = false;
		for (const YAML::Node& defineNode : generatedDefines)
		{
			if (!defineNode.IsScalar())
			{
				return false;
			}

			const std::string define = defineNode.as<std::string>();
			if (define == "TRANSMISSION" && !bHasTransmission)
			{
				mergedDefines.push_back(define);
				bHasTransmission = true;
			}
			else if (define == "ALPHA_CUTOUT" && !bHasAlphaCutout)
			{
				mergedDefines.push_back(define);
				bHasAlphaCutout = true;
			}
		}
	}
	merged["defines"] = mergedDefines.size() > 0 ?
		mergedDefines : YAML::Node();

	struct ManagedPropertyGroup final
	{
		const char* m_group;
		const char* const* m_properties;
		size_t m_numProperties;
	};

	static const char* FloatProperties[] = {
		"material.alphaCutoff",
		"material.transmissionFactor",
		"material.thicknessFactor",
		"material.attenuationDistance",
		"material.indexOfRefraction"
	};
	static const char* Vec4Properties[] = {
		"material.attenuationColor"
	};
	static const char* SamplerProperties[] = {
		"transmissionSampler",
		"thicknessSampler"
	};
	const ManagedPropertyGroup groups[] = {
		{ "uniformsFloat", FloatProperties, std::size(FloatProperties) },
		{ "uniformsVec4", Vec4Properties, std::size(Vec4Properties) },
		{ "samplers", SamplerProperties, std::size(SamplerProperties) }
	};

	for (const ManagedPropertyGroup& group : groups)
	{
		YAML::Node targetGroup = merged[group.m_group];
		const YAML::Node generatedGroup = generatedProperties[group.m_group];
		if ((targetGroup && !targetGroup.IsNull() && !targetGroup.IsMap()) ||
			(generatedGroup && !generatedGroup.IsNull() &&
				!generatedGroup.IsMap()))
		{
			return false;
		}

		if (!targetGroup || targetGroup.IsNull())
		{
			targetGroup = YAML::Node(YAML::NodeType::Map);
			merged[group.m_group] = targetGroup;
		}

		for (size_t index = 0; index < group.m_numProperties; ++index)
		{
			const char* property = group.m_properties[index];
			if (generatedGroup && generatedGroup[property])
			{
				targetGroup[property] = YAML::Clone(
					generatedGroup[property]);
			}
			else
			{
				targetGroup.remove(property);
			}
		}
	}

	inOutMaterial = std::move(merged);
	return true;
}

bool GltfImporterUtils::TryComposeNodeMatrix(
	const tinygltf::Node& node,
	glm::mat4& outMatrix)
{
	outMatrix = glm::mat4(1.0f);
	auto tryConvert = [](double value, float& outValue)
		{
			if (!std::isfinite(value) ||
				value > std::numeric_limits<float>::max() ||
				value < -std::numeric_limits<float>::max())
			{
				return false;
			}

			outValue = static_cast<float>(value);
			return true;
		};
	if (!node.matrix.empty())
	{
		if (node.matrix.size() != 16)
		{
			return false;
		}

		for (int32_t column = 0; column < 4; ++column)
		{
			for (int32_t row = 0; row < 4; ++row)
			{
				if (!tryConvert(
					node.matrix[static_cast<size_t>(column * 4 + row)],
					outMatrix[column][row]))
				{
					return false;
				}
			}
		}

		return IsFiniteGltfMatrix(outMatrix);
	}

	if ((!node.translation.empty() && node.translation.size() != 3) ||
		(!node.rotation.empty() && node.rotation.size() != 4) ||
		(!node.scale.empty() && node.scale.size() != 3))
	{
		return false;
	}

	glm::vec3 translation(0.0f);
	glm::vec3 scale(1.0f);
	glm::quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
	for (int32_t component = 0; component < 3; ++component)
	{
		if ((!node.translation.empty() &&
				!tryConvert(
					node.translation[static_cast<size_t>(component)],
					translation[component])) ||
			(!node.scale.empty() &&
				!tryConvert(
					node.scale[static_cast<size_t>(component)],
					scale[component])))
		{
			return false;
		}
	}

	if (!node.rotation.empty())
	{
		if (!tryConvert(node.rotation[3], rotation.w) ||
			!tryConvert(node.rotation[0], rotation.x) ||
			!tryConvert(node.rotation[1], rotation.y) ||
			!tryConvert(node.rotation[2], rotation.z))
		{
			return false;
		}

		const float lengthSquared = glm::dot(rotation, rotation);
		if (!std::isfinite(lengthSquared) ||
			lengthSquared <= std::numeric_limits<float>::epsilon())
		{
			return false;
		}
		rotation *= glm::inversesqrt(lengthSquared);
	}

	outMatrix = glm::translate(glm::mat4(1.0f), translation) *
		glm::mat4_cast(rotation) *
		glm::scale(glm::mat4(1.0f), scale);
	return IsFiniteGltfMatrix(outMatrix);
}

bool GltfImporterUtils::CollectSceneNodes(
	const tinygltf::Model& model,
	float unitScale,
	TVector<SceneNode>& outNodes)
{
	outNodes.Clear();
	if (!std::isfinite(unitScale))
	{
		return false;
	}

	if (model.meshes.size() >
		static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
		model.nodes.size() >
		static_cast<size_t>(std::numeric_limits<int32_t>::max()))
	{
		return false;
	}

	if (model.nodes.empty())
	{
		outNodes.Reserve(model.meshes.size());
		for (size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex)
		{
			SceneNode node{};
			node.m_name = model.meshes[meshIndex].name.empty() ?
				"Mesh_" + std::to_string(meshIndex) :
				model.meshes[meshIndex].name;
			node.m_meshIndex = static_cast<int32_t>(meshIndex);
			outNodes.Add(std::move(node));
		}
		return true;
	}

	TVector<uint8_t> traversalState(model.nodes.size());
	struct PendingNode
	{
		int32_t m_nodeIndex = -1;
		int32_t m_parentIndex = -1;
		glm::mat4 m_parentWorldMatrix{ 1.0f };
		bool m_bExit = false;
	};
	auto traverseRoot = [&](int32_t rootNode) -> bool
		{
			TVector<PendingNode> pendingNodes;
			pendingNodes.Add({ rootNode, -1, glm::mat4(1.0f), false });
			while (!pendingNodes.IsEmpty())
			{
				PendingNode pending = std::move(*pendingNodes.Last());
				pendingNodes.RemoveLast();
				if (pending.m_nodeIndex < 0 ||
					static_cast<size_t>(pending.m_nodeIndex) >=
						model.nodes.size())
				{
					return false;
				}

				uint8_t& state = traversalState[
					static_cast<size_t>(pending.m_nodeIndex)];
				if (pending.m_bExit)
				{
					state = 2;
					continue;
				}
				if (state == 1)
				{
					return false;
				}
				if (state == 2)
				{
					return false;
				}

				state = 1;
				const tinygltf::Node& node = model.nodes[
					static_cast<size_t>(pending.m_nodeIndex)];
				glm::mat4 localTransform(1.0f);
				if (!TryComposeNodeMatrix(node, localTransform))
				{
					return false;
				}

				glm::mat4 scaledLocalMatrix = localTransform;
				scaledLocalMatrix[3].x *= unitScale;
				scaledLocalMatrix[3].y *= unitScale;
				scaledLocalMatrix[3].z *= unitScale;
				const glm::mat4 worldMatrix =
					pending.m_parentWorldMatrix * scaledLocalMatrix;
				if (!IsFiniteGltfMatrix(scaledLocalMatrix) ||
					!IsFiniteGltfMatrix(worldMatrix))
				{
					return false;
				}

				if (node.mesh < -1 ||
					(node.mesh >= 0 &&
						static_cast<size_t>(node.mesh) >= model.meshes.size()) ||
					node.skin < -1 ||
					(node.skin >= 0 &&
						static_cast<size_t>(node.skin) >= model.skins.size()))
				{
					return false;
				}

				SceneNode sceneNode{};
				sceneNode.m_name = node.name.empty() ?
					"Node_" + std::to_string(pending.m_nodeIndex) :
					node.name;
				sceneNode.m_sourceNodeIndex = pending.m_nodeIndex;
				sceneNode.m_parentIndex = pending.m_parentIndex;
				sceneNode.m_meshIndex = node.mesh;
				sceneNode.m_skinIndex = node.skin;
				sceneNode.m_localMatrix = scaledLocalMatrix;
				sceneNode.m_worldMatrix = worldMatrix;
				sceneNode.m_localTransform =
					Math::Transform::FromMatrix(scaledLocalMatrix);

				const glm::mat4 reconstructed =
					sceneNode.m_localTransform.Matrix();
				float maxDifference = 0.0f;
				float maxMagnitude = 1.0f;
				for (int32_t column = 0; column < 4; ++column)
				{
					for (int32_t row = 0; row < 4; ++row)
					{
						maxDifference = (std::max)(
							maxDifference,
							std::abs(
								reconstructed[column][row] -
								scaledLocalMatrix[column][row]));
						maxMagnitude = (std::max)(
							maxMagnitude,
							std::abs(scaledLocalMatrix[column][row]));
					}
				}
				sceneNode.m_bTransformDecomposable =
					maxDifference <= maxMagnitude * 1e-4f;

				const int32_t outputNodeIndex =
					static_cast<int32_t>(outNodes.Num());
				outNodes.Add(std::move(sceneNode));

				pendingNodes.Add({
					pending.m_nodeIndex,
					pending.m_parentIndex,
					glm::mat4(1.0f),
					true
				});
				for (size_t child = node.children.size(); child > 0; --child)
				{
					pendingNodes.Add({
						node.children[child - 1],
						outputNodeIndex,
						worldMatrix,
						false
					});
				}
			}

			return true;
		};

	bool bTraversedRoot = false;
	if (!model.scenes.empty())
	{
		const int32_t sceneIndex = model.defaultScene >= 0 ?
			model.defaultScene : 0;
		if (sceneIndex < 0 ||
			static_cast<size_t>(sceneIndex) >= model.scenes.size())
		{
			return false;
		}

		bTraversedRoot = true;
		for (int32_t rootNode :
			model.scenes[static_cast<size_t>(sceneIndex)].nodes)
		{
			if (!traverseRoot(rootNode))
			{
				outNodes.Clear();
				return false;
			}
		}
	}
	else
	{
		TVector<uint8_t> hasParent(model.nodes.size());
		for (const tinygltf::Node& node : model.nodes)
		{
			for (int32_t childIndex : node.children)
			{
				if (childIndex < 0 ||
					static_cast<size_t>(childIndex) >= model.nodes.size() ||
					hasParent[static_cast<size_t>(childIndex)] != 0)
				{
					return false;
				}
				hasParent[static_cast<size_t>(childIndex)] = 1;
			}
		}

		for (size_t nodeIndex = 0; nodeIndex < model.nodes.size(); ++nodeIndex)
		{
			if (hasParent[nodeIndex] == 0)
			{
				bTraversedRoot = true;
				if (!traverseRoot(static_cast<int32_t>(nodeIndex)))
				{
					outNodes.Clear();
					return false;
				}
			}
		}
	}

	if (!bTraversedRoot)
	{
		outNodes.Clear();
		return false;
	}

	return true;
}

bool GltfImporterUtils::CollectMeshInstances(
	const tinygltf::Model& model,
	TVector<MeshInstance>& outInstances)
{
	outInstances.Clear();
	TVector<SceneNode> nodes;
	if (!CollectSceneNodes(model, 1.0f, nodes))
	{
		return false;
	}

	outInstances.Reserve(nodes.Num());
	for (const SceneNode& node : nodes)
	{
		if (node.m_meshIndex < 0)
		{
			continue;
		}

		MeshInstance instance{};
		instance.m_nodeIndex = node.m_sourceNodeIndex;
		instance.m_meshIndex = node.m_meshIndex;
		instance.m_skinIndex = node.m_skinIndex;
		instance.m_worldTransform = node.m_worldMatrix;
		outInstances.Add(std::move(instance));
	}

	return true;
}

bool GltfImporterUtils::LoadModel(
	const std::string& assetFilepath,
	bool bImagesAsIs,
	tinygltf::Model& outModel,
	std::string& outError,
	std::string& outWarning)
{
	outModel = tinygltf::Model();
	outError.clear();
	outWarning.clear();

	tinygltf::TinyGLTF loader;
	if (bImagesAsIs)
	{
		loader.SetImagesAsIs(true);
	}

	const bool bIsGlb =
		Utils::GetFileExtension(assetFilepath.c_str()) == "glb";
	const bool bLoadedNormally = bIsGlb ?
		loader.LoadBinaryFromFile(
			&outModel,
			&outError,
			&outWarning,
			assetFilepath.c_str()) :
		loader.LoadASCIIFromFile(
			&outModel,
			&outError,
			&outWarning,
			assetFilepath.c_str());

	bool bParsed = bLoadedNormally;
	if (!bParsed && !bIsGlb)
	{
		tinygltf::Model patchedModel;
		std::string patchedError;
		std::string patchedWarning;
		bool bHandled = false;
		bParsed = LoadAsciiWithMeshoptPlaceholders(
			loader,
			patchedModel,
			patchedError,
			patchedWarning,
			assetFilepath,
			bHandled);
		if (bHandled)
		{
			outModel = std::move(patchedModel);
			outError = std::move(patchedError);
			outWarning = std::move(patchedWarning);
		}
	}

	if (!bParsed)
	{
		return false;
	}

	std::string decodeError;
	if (!DecodeDracoPrimitives(outModel, decodeError))
	{
		outError = "Cannot decode Draco primitives: " + decodeError;
		return false;
	}

	if (!DecodeMeshoptBufferViews(outModel, decodeError))
	{
		outError = "Cannot decode glTF buffer views: " + decodeError;
		return false;
	}

	return true;
}

YAML::Node Model::Serialize() const
{
	YAML::Node res;
	SERIALIZE_PROPERTY(res, m_fileId);
	return res;
}

void Model::Deserialize(const YAML::Node& inData)
{
	DESERIALIZE_PROPERTY(inData, m_fileId);
}

bool Model::IsSourceMeshIndexValid(int32_t meshIndex) const
{
	return meshIndex >= 0 &&
		static_cast<size_t>(meshIndex) < m_sourceMeshes.Num() &&
		!m_sourceMeshes[static_cast<size_t>(meshIndex)].m_renderMeshIndices.IsEmpty();
}

const Math::AABB& Model::GetBoundsAABB(int32_t meshIndex) const
{
	if (meshIndex == AllMeshes)
	{
		return m_boundsAabb;
	}

	static const Math::AABB emptyBounds{};
	return IsSourceMeshIndexValid(meshIndex) ?
		m_sourceMeshes[static_cast<size_t>(meshIndex)].m_bounds :
		emptyBounds;
}

bool Model::HasBLAS(int32_t meshIndex) const
{
	if (meshIndex == AllMeshes)
	{
		return HasBLAS();
	}

	return meshIndex >= 0 &&
		static_cast<size_t>(meshIndex) < m_sourceMeshBlases.Num() &&
		m_sourceMeshBlases[static_cast<size_t>(meshIndex)].IsValid();
}

const TSharedPtr<Raytracing::BVH>& Model::GetBLAS(int32_t meshIndex) const
{
	if (meshIndex == AllMeshes)
	{
		return m_blas;
	}

	static const TSharedPtr<Raytracing::BVH> emptyBlas{};
	return meshIndex >= 0 &&
		static_cast<size_t>(meshIndex) < m_sourceMeshBlases.Num() ?
		m_sourceMeshBlases[static_cast<size_t>(meshIndex)].m_blas :
		emptyBlas;
}

const TVector<Math::Triangle>& Model::GetBLASTriangles(
	int32_t meshIndex) const
{
	if (meshIndex == AllMeshes)
	{
		return m_blasTriangles;
	}

	static const TVector<Math::Triangle> emptyTriangles{};
	return meshIndex >= 0 &&
		static_cast<size_t>(meshIndex) < m_sourceMeshBlases.Num() ?
		m_sourceMeshBlases[static_cast<size_t>(meshIndex)].m_triangles :
		emptyTriangles;
}

bool Model::CollectRenderData(
	int32_t meshIndex,
	TVector<RHI::RHIMeshPtr>& outMeshes,
	TVector<glm::mat4>& outModelMatrices,
	Math::AABB& outBounds) const
{
	outMeshes.Clear();
	outModelMatrices.Clear();
	outBounds = Math::AABB();

	if (meshIndex == AllMeshes)
	{
		outMeshes.Reserve(m_renderInstances.Num());
		outModelMatrices.Reserve(m_renderInstances.Num());
		for (const RenderInstance& instance : m_renderInstances)
		{
			if (instance.m_renderMeshIndex >= m_meshes.Num() ||
				!m_meshes[instance.m_renderMeshIndex])
			{
				continue;
			}

			const RHI::RHIMeshPtr& mesh =
				m_meshes[instance.m_renderMeshIndex];
			outMeshes.Add(mesh);
			outModelMatrices.Add(instance.m_modelMatrix);
			Math::AABB instanceBounds = mesh->m_bounds;
			instanceBounds.Apply(instance.m_modelMatrix);
			outBounds.Extend(instanceBounds);
		}
	}
	else if (IsSourceMeshIndexValid(meshIndex))
	{
		const SourceMesh& sourceMesh =
			m_sourceMeshes[static_cast<size_t>(meshIndex)];
		outMeshes.Reserve(sourceMesh.m_renderMeshIndices.Num());
		outModelMatrices.Reserve(sourceMesh.m_renderMeshIndices.Num());
		for (uint32_t renderMeshIndex : sourceMesh.m_renderMeshIndices)
		{
			if (renderMeshIndex >= m_meshes.Num() ||
				!m_meshes[renderMeshIndex])
			{
				continue;
			}

			outMeshes.Add(m_meshes[renderMeshIndex]);
			outModelMatrices.Add(glm::mat4(1.0f));
		}
		outBounds = sourceMesh.m_bounds;
	}

	return !outMeshes.IsEmpty() &&
		outMeshes.Num() == outModelMatrices.Num() &&
		outBounds.IsValid();
}

void Model::Flush()
{
	m_bGpuReady.store(false, std::memory_order_release);

	if (m_meshes.Num() == 0)
	{
		m_bIsReady.store(false, std::memory_order_release);
		return;
	}

	for (const auto& mesh : m_meshes)
	{
		if (!mesh)
		{
			m_bIsReady.store(false, std::memory_order_release);
			return;
		}
	}

	// Flush publishes a structurally complete model. GPU uploads are asynchronous,
	// so IsReady() also checks every RHIMesh until its upload fence is finished.
	m_bIsReady.store(true, std::memory_order_release);
}

bool Model::BuildBLASData(
	const TVector<RenderInstance>& blasInstances,
	BLASData& outData) const
{
	outData = BLASData{};

	if (m_cpuMeshes.Num() == 0 || blasInstances.IsEmpty())
	{
		return false;
	}

	size_t expectedNumTriangles = 0;
	constexpr size_t maxNumTriangles =
		(static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1) / 2;
	for (const RenderInstance& instance : blasInstances)
	{
		if (instance.m_renderMeshIndex >= m_cpuMeshes.Num())
		{
			return false;
		}
		const MeshCpuData& mesh = m_cpuMeshes[instance.m_renderMeshIndex];
		if (mesh.m_indices.Num() % 3 != 0)
		{
			return false;
		}

		const size_t meshTriangles = mesh.m_indices.Num() / 3;
		if (meshTriangles > maxNumTriangles - expectedNumTriangles)
		{
			return false;
		}

		expectedNumTriangles += meshTriangles;
	}

	if (expectedNumTriangles == 0)
	{
		return false;
	}

	outData.m_triangles.Reserve(expectedNumTriangles);

	for (const RenderInstance& instance : blasInstances)
	{
		const MeshCpuData& mesh = m_cpuMeshes[instance.m_renderMeshIndex];
		const glm::mat3 linearMatrix(instance.m_modelMatrix);
		glm::mat3 normalMatrix = linearMatrix;
		const float determinant = glm::determinant(linearMatrix);
		if (std::isfinite(determinant) && determinant != 0.0f)
		{
			const glm::mat3 inverseTranspose =
				glm::transpose(glm::inverse(linearMatrix));
			if (IsFiniteGltfMatrix(inverseTranspose))
			{
				normalMatrix = inverseTranspose;
			}
		}

		for (size_t i = 0; i + 2 < mesh.m_indices.Num(); i += 3)
		{
			const uint32_t i0 = mesh.m_indices[i + 0];
			const uint32_t i1 = mesh.m_indices[i + 1];
			const uint32_t i2 = mesh.m_indices[i + 2];
			if (i0 >= mesh.m_vertices.Num() ||
				i1 >= mesh.m_vertices.Num() ||
				i2 >= mesh.m_vertices.Num())
			{
				outData.m_triangles.Clear();
				return false;
			}

			auto v0 = mesh.m_vertices[i0];
			auto v1 = mesh.m_vertices[i1];
			auto v2 = mesh.m_vertices[i2];
			auto applyInstanceTransform = [&](auto& vertex)
				{
					vertex.m_position = glm::vec3(
						instance.m_modelMatrix *
						glm::vec4(vertex.m_position, 1.0f));
					vertex.m_normal = normalMatrix * vertex.m_normal;
					vertex.m_tangent = linearMatrix * vertex.m_tangent;
					vertex.m_bitangent = linearMatrix * vertex.m_bitangent;
				};
			applyInstanceTransform(v0);
			applyInstanceTransform(v1);
			applyInstanceTransform(v2);
			if (!Math::AllFinite(v0.m_position) ||
				!Math::AllFinite(v1.m_position) ||
				!Math::AllFinite(v2.m_position))
			{
				outData.m_triangles.Clear();
				return false;
			}

			const glm::vec3 edge1 = v1.m_position - v0.m_position;
			const glm::vec3 edge2 = v2.m_position - v0.m_position;
			const glm::vec3 triangleNormal = glm::cross(edge1, edge2);
			if (!Math::AllFinite(edge1) ||
				!Math::AllFinite(edge2) ||
				!Math::AllFinite(triangleNormal) ||
				!std::isfinite(glm::dot(triangleNormal, triangleNormal)))
			{
				outData.m_triangles.Clear();
				return false;
			}

			SanitizeVertexFrame(v0);
			SanitizeVertexFrame(v1);
			SanitizeVertexFrame(v2);
			if (!Math::AllFinite(v0.m_normal) ||
				!Math::AllFinite(v1.m_normal) ||
				!Math::AllFinite(v2.m_normal) ||
				!Math::AllFinite(v0.m_tangent) ||
				!Math::AllFinite(v1.m_tangent) ||
				!Math::AllFinite(v2.m_tangent) ||
				!Math::AllFinite(v0.m_bitangent) ||
				!Math::AllFinite(v1.m_bitangent) ||
				!Math::AllFinite(v2.m_bitangent))
			{
				outData.m_triangles.Clear();
				return false;
			}

			Math::Triangle tri{};
			tri.m_vertices[0] = v0.m_position;
			tri.m_vertices[1] = v1.m_position;
			tri.m_vertices[2] = v2.m_position;

			tri.m_normals[0] = v0.m_normal;
			tri.m_normals[1] = v1.m_normal;
			tri.m_normals[2] = v2.m_normal;

			tri.m_tangent[0] = v0.m_tangent;
			tri.m_tangent[1] = v1.m_tangent;
			tri.m_tangent[2] = v2.m_tangent;

			tri.m_bitangent[0] = v0.m_bitangent;
			tri.m_bitangent[1] = v1.m_bitangent;
			tri.m_bitangent[2] = v2.m_bitangent;

			tri.m_uvs[0] = Math::AllFinite(v0.m_texcoord) ?
				v0.m_texcoord : glm::vec2(0.0f);
			tri.m_uvs[1] = Math::AllFinite(v1.m_texcoord) ?
				v1.m_texcoord : glm::vec2(0.0f);
			tri.m_uvs[2] = Math::AllFinite(v2.m_texcoord) ?
				v2.m_texcoord : glm::vec2(0.0f);
			tri.m_uvs2[0] = tri.m_uvs[0];
			tri.m_uvs2[1] = tri.m_uvs[1];
			tri.m_uvs2[2] = tri.m_uvs[2];

			tri.m_materialIndex = static_cast<uint8_t>((std::max)(0, (std::min)(mesh.m_materialIndex, 255)));
			tri.m_centroid = tri.m_vertices[0] / 3.0f +
				tri.m_vertices[1] / 3.0f +
				tri.m_vertices[2] / 3.0f;
			if (!Math::AllFinite(tri.m_centroid))
			{
				outData.m_triangles.Clear();
				return false;
			}

			outData.m_triangles.Add(tri);
		}
	}

	if (outData.m_triangles.Num() == 0)
	{
		return false;
	}

	outData.m_blas = TSharedPtr<Raytracing::BVH>::Make(
		static_cast<uint32_t>(outData.m_triangles.Num()));
	outData.m_blas->BuildBVH(outData.m_triangles);
	return true;
}

bool Model::BuildBLAS()
{
	m_blas.Clear();
	m_blasTriangles.Clear();
	m_sourceMeshBlases.Clear();

	TVector<RenderInstance> fullInstances = m_renderInstances;
	if (fullInstances.IsEmpty())
	{
		fullInstances.Reserve(m_cpuMeshes.Num());
		for (size_t meshIndex = 0; meshIndex < m_cpuMeshes.Num(); ++meshIndex)
		{
			RenderInstance instance{};
			instance.m_renderMeshIndex = static_cast<uint32_t>(meshIndex);
			fullInstances.Add(std::move(instance));
		}
	}

	BLASData fullBlas;
	if (!BuildBLASData(fullInstances, fullBlas))
	{
		return false;
	}

	m_blas = std::move(fullBlas.m_blas);
	m_blasTriangles = std::move(fullBlas.m_triangles);
	m_sourceMeshBlases.Resize(m_sourceMeshes.Num());
	for (size_t sourceMeshIndex = 0;
		sourceMeshIndex < m_sourceMeshes.Num();
		++sourceMeshIndex)
	{
		TVector<RenderInstance> sourceInstances;
		const SourceMesh& sourceMesh = m_sourceMeshes[sourceMeshIndex];
		sourceInstances.Reserve(sourceMesh.m_renderMeshIndices.Num());
		for (uint32_t renderMeshIndex : sourceMesh.m_renderMeshIndices)
		{
			RenderInstance instance{};
			instance.m_renderMeshIndex = renderMeshIndex;
			sourceInstances.Add(std::move(instance));
		}

		BuildBLASData(
			sourceInstances,
			m_sourceMeshBlases[sourceMeshIndex]);
	}

	return true;
}

void Model::ProceedCpuMeshes(bool bShouldGenerateBLAS, bool bShouldKeepCpuBuffers)
{
	if (bShouldGenerateBLAS)
	{
		BuildBLAS();
	}
	else
	{
		m_blas.Clear();
		m_blasTriangles.Clear();
		m_sourceMeshBlases.Clear();
	}

	if (!bShouldKeepCpuBuffers)
	{
		m_cpuMeshes.Clear();
	}
}

bool Model::IsReady() const
{
	if (!IsStructurallyReady())
	{
		return false;
	}

	// GPU upload completion is monotonic until the next Flush(). Large editable
	// model hierarchies can reference one Model from thousands of renderers, so
	// rescanning every RHIMesh for every component would be O(N^2) per frame.
	if (m_bGpuReady.load(std::memory_order_acquire))
	{
		return true;
	}

	for (const auto& mesh : m_meshes)
	{
		if (!mesh || !mesh->IsReady())
		{
			return false;
		}
	}

	m_bGpuReady.store(true, std::memory_order_release);
	return true;
}

bool Model::IsStructurallyReady() const
{
	return m_bIsReady.load(std::memory_order_acquire);
}

ModelImporter::ModelImporter(ModelAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

ModelImporter::~ModelImporter()
{
	for (auto& model : m_loadedModels)
	{
		model.m_second.DestroyObject(m_allocator);
	}
}

void ModelImporter::OnUpdateAssetInfo(AssetInfoPtr assetInfo, bool bWasExpired)
{
	SAILOR_PROFILE_FUNCTION();
	SAILOR_PROFILE_TEXT(assetInfo->GetAssetFilepath().c_str());
	auto areGeneratedAssetsValid = [](
		const TVector<FileId>& fileIds,
		bool bRequireUniqueFileIds)
		{
			AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
			if (assetRegistry == nullptr)
			{
				return false;
			}

			TSet<FileId> uniqueFileIds;
			for (const FileId& fileId : fileIds)
			{
				if (!fileId ||
					(bRequireUniqueFileIds &&
						uniqueFileIds.Contains(fileId)) ||
					assetRegistry->GetAssetInfoPtr(fileId) == nullptr)
				{
					return false;
				}
				uniqueFileIds.Insert(fileId);
			}
			return true;
		};

	if (ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo))
	{
		if (modelAssetInfo->IsWritable())
		{
			const TVector<FileId>& materials =
				modelAssetInfo->GetDefaultMaterials();
			const bool bMaterialsNeedRepair =
				materials.Num() > 0 &&
				!areGeneratedAssetsValid(
					materials,
					modelAssetInfo->ShouldBatchByMaterial());
			const bool bShouldRegenerateMaterials =
				modelAssetInfo->ShouldGenerateMaterials() &&
				((bWasExpired && materials.Num() == 0) ||
					bMaterialsNeedRepair);
			if (bShouldRegenerateMaterials &&
				GenerateMaterialAssets(modelAssetInfo))
			{
				assetInfo->SaveMetaFile();
			}
			else if (modelAssetInfo->ShouldGenerateMaterials() &&
				bWasExpired &&
				materials.Num() > 0 &&
				!bMaterialsNeedRepair)
			{
				UpdateGeneratedMaterialProperties(modelAssetInfo);
			}

			const TVector<FileId>& animations =
				modelAssetInfo->GetAnimations();
			const bool bAnimationsNeedRepair =
				animations.Num() > 0 &&
				!areGeneratedAssetsValid(animations, true);
			if (((bWasExpired && animations.Num() == 0) ||
				bAnimationsNeedRepair) &&
				GenerateAnimationAssets(modelAssetInfo))
			{
				assetInfo->SaveMetaFile();
			}
		}

		if (bWasExpired)
		{
			GenerateFingerprintAsync(modelAssetInfo);
		}
	}
}

void ModelImporter::OnImportAsset(AssetInfoPtr assetInfo)
{
	ModelAssetInfoPtr modelAssetInfo = dynamic_cast<ModelAssetInfoPtr>(assetInfo);
	if (!modelAssetInfo)
	{
		return;
	}

	if (modelAssetInfo->IsWritable())
	{
		if (modelAssetInfo->ShouldGenerateMaterials() &&
			modelAssetInfo->GetDefaultMaterials().Num() == 0 &&
			GenerateMaterialAssets(modelAssetInfo))
		{
			assetInfo->SaveMetaFile();
		}

		if (modelAssetInfo->GetAnimations().Num() == 0 &&
			GenerateAnimationAssets(modelAssetInfo))
		{
			assetInfo->SaveMetaFile();
		}
	}

	GenerateFingerprintAsync(modelAssetInfo);

}

void ModelImporter::GenerateFingerprintAsync(ModelAssetInfoPtr modelAssetInfo)
{
	if (!modelAssetInfo || !modelAssetInfo->GetFileId())
	{
		return;
	}

	const FileId fileId = modelAssetInfo->GetFileId();
	const std::filesystem::path outputPath = GetFingerprintPath(fileId);
	if (outputPath.empty())
	{
		SAILOR_LOG_ERROR(
			"Cannot generate fingerprint for invalid FileId: %s",
			fileId.ToString().c_str());
		return;
	}

	const std::string assetFilepath = modelAssetInfo->GetAssetFilepath();
	const float unitScale = modelAssetInfo->GetUnitScale();
	const bool bShouldBatchByMaterial = modelAssetInfo->ShouldBatchByMaterial();
	FileRevision sourceRevision;
	if (!Utils::TryGetFileRevision(assetFilepath, sourceRevision))
	{
		SAILOR_LOG_ERROR(
			"Cannot capture model source revision for fingerprint: %s",
			assetFilepath.c_str());
		return;
	}

	if (App::GetSubmodule<Tasks::Scheduler>() == nullptr)
	{
		SAILOR_LOG_ERROR(
			"Cannot schedule model fingerprint without a task scheduler: %s",
			assetFilepath.c_str());
		return;
	}

	FingerprintTaskChain& taskChain = GetFingerprintTaskChain();
	uint64_t generation = 0;
	{
		const std::lock_guard<std::mutex> lock(taskChain.m_mutex);
		generation = ++taskChain.m_nextRequestGeneration;
		taskChain.m_requests[fileId] = {
			generation,
			sourceRevision
		};

		std::error_code removeError;
		std::filesystem::remove(outputPath, removeError);
		if (removeError)
		{
			SAILOR_LOG_ERROR(
				"Cannot invalidate previous model fingerprint '%s': %s",
				outputPath.string().c_str(),
				removeError.message().c_str());
		}
	}

	Tasks::CreateTask(
		"Generate model fingerprint",
		[
			fileId,
			assetFilepath,
			unitScale,
			bShouldBatchByMaterial,
			outputPath,
			generation,
			sourceRevision
		]()
		{
			if (IsFingerprintRequestCurrent(
				fileId,
				generation,
				sourceRevision))
			{
				const ActiveFingerprintRequestScope requestScope(
					generation,
					sourceRevision);
				GenerateFingerprint(
					fileId,
					assetFilepath,
					unitScale,
					bShouldBatchByMaterial,
					outputPath.string());
			}

			FingerprintTaskChain& completedTaskChain =
				GetFingerprintTaskChain();
			const std::lock_guard<std::mutex> completedLock(
				completedTaskChain.m_mutex);
			FingerprintRequest* completedRequest = nullptr;
			if (completedTaskChain.m_requests.Find(
				fileId,
				completedRequest) &&
				completedRequest != nullptr &&
				completedRequest->m_generation == generation &&
				completedRequest->m_sourceRevision == sourceRevision)
			{
				completedTaskChain.m_requests.Remove(fileId);
			}
		},
		EThreadType::Background)->Run();
}

bool ModelImporter::GenerateFingerprint(
	const FileId& fileId,
	const std::string& assetFilepath,
	float unitScale,
	bool bShouldBatchByMaterial,
	const std::string& outputPath)
{
	TVector<MeshContext> parsedMeshes;
	TVector<glm::mat4> inverseBind;
	Math::AABB boundsAabb;
	Math::Sphere boundsSphere;
	tinygltf::Model gltfModel;
	if (!ImportModel(
			assetFilepath,
			unitScale,
			bShouldBatchByMaterial,
			parsedMeshes,
			boundsAabb,
			boundsSphere,
			inverseBind,
			&gltfModel) ||
		parsedMeshes.Num() == 0 ||
		!boundsAabb.IsValid())
	{
		SAILOR_LOG_ERROR(
			"Cannot prepare model fingerprint: %s",
			assetFilepath.c_str());
		return false;
	}
	TVector<GltfImporterUtils::SceneNode> sceneNodes;
	if (!GltfImporterUtils::CollectSceneNodes(
			gltfModel,
			unitScale,
			sceneNodes))
	{
		SAILOR_LOG_ERROR(
			"Cannot resolve model fingerprint hierarchy: %s",
			assetFilepath.c_str());
		return false;
	}

	ObjectAllocatorPtr allocator =
		ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	const size_t previewMaterialCount = (std::min)(
		gltfModel.materials.size(),
		size_t{ 256 });
	TVector<MaterialPtr> previewMaterials(previewMaterialCount);
	TMap<int32_t, int32_t> previewTextureSources;
	TMap<int32_t, TexturePtr> previewImages;

	auto resolvePreviewImageSource = [&](int32_t textureIndex) -> int32_t
		{
			int32_t* cachedSource = nullptr;
			if (previewTextureSources.Find(textureIndex, cachedSource) &&
				cachedSource != nullptr)
			{
				return *cachedSource;
			}

			int32_t imageIndex = -1;
			if (textureIndex >= 0 &&
				static_cast<size_t>(textureIndex) < gltfModel.textures.size())
			{
				const tinygltf::Texture& sourceTexture =
					gltfModel.textures[textureIndex];
				if (sourceTexture.source >= 0 &&
					static_cast<size_t>(sourceTexture.source) <
						gltfModel.images.size())
				{
					imageIndex = sourceTexture.source;
				}
			}

			previewTextureSources[textureIndex] = imageIndex;
			return imageIndex;
		};

	auto loadPreviewTexture = [&](int32_t textureIndex) -> TexturePtr
		{
			const int32_t imageIndex =
				resolvePreviewImageSource(textureIndex);
			if (imageIndex < 0)
			{
				return TexturePtr();
			}

			TexturePtr* cachedTexture = nullptr;
			if (previewImages.Find(imageIndex, cachedTexture) &&
				cachedTexture != nullptr)
			{
				return *cachedTexture;
			}

			const tinygltf::Image& image =
				gltfModel.images[imageIndex];
			auto cacheFailure = [&]() -> TexturePtr
				{
					previewImages[imageIndex] = TexturePtr();
					return TexturePtr();
				};

			if (!image.as_is ||
				image.image.empty() ||
				image.image.size() >
					static_cast<size_t>(std::numeric_limits<int>::max()))
			{
				return cacheFailure();
			}

			int32_t sourceWidth = 0;
			int32_t sourceHeight = 0;
			int32_t sourceChannels = 0;
			if (!stbi_info_from_memory(
				image.image.data(),
				static_cast<int>(image.image.size()),
				&sourceWidth,
				&sourceHeight,
				&sourceChannels) ||
				sourceWidth <= 0 ||
				sourceHeight <= 0 ||
				sourceChannels <= 0)
			{
				return cacheFailure();
			}

			size_t sourcePixelCount = 0;
			size_t decodedBytes = 0;
			if (!TryMultiplySize(
				static_cast<size_t>(sourceWidth),
				static_cast<size_t>(sourceHeight),
				sourcePixelCount) ||
				!TryMultiplySize(
					sourcePixelCount,
					static_cast<size_t>(STBI_rgb_alpha),
					decodedBytes) ||
				decodedBytes > MaxFingerprintDecodedImageBytes)
			{
				return cacheFailure();
			}

			int32_t decodedWidth = 0;
			int32_t decodedHeight = 0;
			int32_t decodedChannels = 0;
			StbiImageData decodedPixels(stbi_load_from_memory(
					image.image.data(),
					static_cast<int>(image.image.size()),
					&decodedWidth,
					&decodedHeight,
					&decodedChannels,
					STBI_rgb_alpha));
			if (!decodedPixels ||
				decodedWidth != sourceWidth ||
				decodedHeight != sourceHeight)
			{
				return cacheFailure();
			}

			int32_t previewWidth = 0;
			int32_t previewHeight = 0;
			size_t previewBytes = 0;
			if (!TryCalculateFingerprintTextureSize(
				decodedWidth,
				decodedHeight,
				previewWidth,
				previewHeight,
				previewBytes))
			{
				return cacheFailure();
			}

			TVector<uint8_t> previewPixels;
			previewPixels.Resize(previewBytes);
			u8vec4* destination = reinterpret_cast<u8vec4*>(
				previewPixels.GetData());
			for (int32_t y = 0; y < previewHeight; ++y)
			{
				const size_t sourceY = (std::min)(
					static_cast<size_t>(decodedHeight - 1),
					static_cast<size_t>(
						(static_cast<uint64_t>(y) * decodedHeight) /
						previewHeight));
				for (int32_t x = 0; x < previewWidth; ++x)
				{
					const size_t sourceX = (std::min)(
						static_cast<size_t>(decodedWidth - 1),
						static_cast<size_t>(
							(static_cast<uint64_t>(x) * decodedWidth) /
							previewWidth));
					const size_t sourceOffset =
						(sourceY * static_cast<size_t>(decodedWidth) +
							sourceX) *
						static_cast<size_t>(STBI_rgb_alpha);
					const size_t destinationIndex =
						static_cast<size_t>(y) *
							static_cast<size_t>(previewWidth) +
						static_cast<size_t>(x);
					destination[destinationIndex] = u8vec4(
						decodedPixels.GetData()[sourceOffset + 0],
						decodedPixels.GetData()[sourceOffset + 1],
						decodedPixels.GetData()[sourceOffset + 2],
						decodedPixels.GetData()[sourceOffset + 3]);
				}
			}

			decodedPixels.Clear();
			TexturePtr texture = TexturePtr::Make(
				allocator,
				FileId::CreateNewFileId());
			texture->m_decodedData = std::move(previewPixels);
			texture->m_width = previewWidth;
			texture->m_height = previewHeight;
			texture->m_mipLevels = 1;

			previewImages[imageIndex] = texture;
			return texture;
		};

	for (size_t i = 0; i < previewMaterialCount; i++)
	{
		const tinygltf::Material& sourceMaterial =
			gltfModel.materials[i];
		const auto transmissionSettings =
			GltfImporterUtils::ResolveMaterialTransmission(
				sourceMaterial,
				gltfModel.textures.size(),
				unitScale);
		const auto alphaModeSettings =
			GltfImporterUtils::ResolveMaterialAlphaMode(
				sourceMaterial.alphaMode,
				transmissionSettings.IsEnabled());

		MaterialPtr material = MaterialPtr::Make(
			allocator,
			FileId::CreateNewFileId());
		material->SetRenderState(RHI::RenderState(
			true,
			alphaModeSettings.m_bEnableZWrite,
			0.0f,
			alphaModeSettings.m_bAlphaCutout,
			sourceMaterial.doubleSided ?
				RHI::ECullMode::None :
				RHI::ECullMode::Back,
			alphaModeSettings.m_blendMode,
			RHI::EFillMode::Fill,
			GetHash(alphaModeSettings.m_renderQueue)));

		const auto& pbr = sourceMaterial.pbrMetallicRoughness;
		material->SetUniform(
			"material.baseColorFactor",
			vec4(
				pbr.baseColorFactor[0],
				pbr.baseColorFactor[1],
				pbr.baseColorFactor[2],
				pbr.baseColorFactor[3]));
		material->SetUniform(
			"material.emissiveFactor",
			vec4(
				sourceMaterial.emissiveFactor[0],
				sourceMaterial.emissiveFactor[1],
				sourceMaterial.emissiveFactor[2],
				0.0f));
		material->SetUniform(
			"material.roughnessFactor",
			static_cast<float>(pbr.roughnessFactor));
		material->SetUniform(
			"material.metallicFactor",
			static_cast<float>(pbr.metallicFactor));
		material->SetUniform(
			"material.alphaCutoff",
			static_cast<float>(sourceMaterial.alphaCutoff));
		if (transmissionSettings.IsEnabled())
		{
			material->SetUniform(
				"material.transmissionFactor",
				transmissionSettings.m_factor);
			material->SetUniform(
				"material.thicknessFactor",
				transmissionSettings.m_thicknessFactor);
			material->SetUniform(
				"material.attenuationDistance",
				transmissionSettings.m_attenuationDistance);
			material->SetUniform(
				"material.indexOfRefraction",
				transmissionSettings.m_indexOfRefraction);
			material->SetUniform(
				"material.attenuationColor",
				glm::vec4(
					transmissionSettings.m_attenuationColor,
					1.0f));
		}

		auto bindTexture = [&](
			const char* samplerName,
			int32_t textureIndex)
			{
				if (TexturePtr texture =
					loadPreviewTexture(textureIndex))
				{
					material->SetSampler(
						samplerName,
						texture);
				}
			};
		bindTexture(
			"baseColorSampler",
			pbr.baseColorTexture.index);
		bindTexture(
			"normalSampler",
			sourceMaterial.normalTexture.index);
		bindTexture(
			"ormSampler",
			pbr.metallicRoughnessTexture.index);
		bindTexture(
			"emissiveSampler",
			sourceMaterial.emissiveTexture.index);
		bindTexture(
			"occlusionSampler",
			sourceMaterial.occlusionTexture.index);
		if (transmissionSettings.IsEnabled())
		{
			bindTexture(
				"transmissionSampler",
				transmissionSettings.m_textureIndex);
			bindTexture(
				"thicknessSampler",
				transmissionSettings.m_thicknessTextureIndex);
		}
		previewMaterials[i] = std::move(material);
	}
	ModelPtr model = ModelPtr::Make(allocator, fileId);
	model->m_boundsAabb = boundsAabb;
	model->m_boundsSphere = boundsSphere;
	model->m_inverseBind = std::move(inverseBind);
	model->m_cpuMeshes.Reserve(parsedMeshes.Num());
	model->m_sourceMeshes.Resize(gltfModel.meshes.size());

	for (MeshContext& mesh : parsedMeshes)
	{
		if (!mesh.HasGeometry())
		{
			continue;
		}

		Model::MeshCpuData cpuMesh{};
		cpuMesh.m_vertices = std::move(mesh.outVertices);
		for (auto& vertex : cpuMesh.m_vertices)
		{
			SanitizeVertexFrame(vertex);
		}
		cpuMesh.m_indices = std::move(mesh.outIndices);
		cpuMesh.m_bounds = mesh.bounds;
		cpuMesh.m_materialIndex = mesh.materialIndex;
		const uint32_t renderMeshIndex =
			static_cast<uint32_t>(model->m_cpuMeshes.Num());
		model->m_cpuMeshes.Add(std::move(cpuMesh));
		if (mesh.sourceMeshIndex >= 0 &&
			static_cast<size_t>(mesh.sourceMeshIndex) <
				model->m_sourceMeshes.Num())
		{
			auto& sourceMesh = model->m_sourceMeshes[
				static_cast<size_t>(mesh.sourceMeshIndex)];
			sourceMesh.m_renderMeshIndices.Add(renderMeshIndex);
			sourceMesh.m_bounds.Extend(mesh.bounds);
		}
	}

	PopulateModelSceneHierarchy(*model, sceneNodes);
	gltfModel = tinygltf::Model();

	if (!model->BuildBLAS())
	{
		SAILOR_LOG_ERROR(
			"Cannot build model fingerprint BLAS: %s",
			assetFilepath.c_str());
		return false;
	}

	Raytracing::PathTracer::TLASInstance instance{};
	instance.m_model = model;
	instance.m_worldBounds = boundsAabb;

	Raytracing::PathTracer pathTracer;
	if (!pathTracer.InitializeScene(
			TVector<Raytracing::PathTracer::TLASInstance>{ instance },
			previewMaterials,
			{}))
	{
		SAILOR_LOG_ERROR(
			"Cannot initialize model fingerprint scene: %s",
			assetFilepath.c_str());
		return false;
	}

	const float radius = (std::max)(boundsSphere.m_radius, 0.1f);
	Raytracing::PathTracer::Params params{};
	params.m_height = FingerprintImageDimension;
	params.m_numSamples = 1;
	params.m_numAmbientSamples = 1;
	params.m_maxBounces = 4;
	params.m_msaa = 1;
	params.m_ambient = vec3(0.08f);
	params.m_rayBiasScale = 3e-4f;
	params.m_bUseRuntimeCamera = true;
	params.m_runtimeCameraPos =
		boundsSphere.m_center + vec3(0.0f, radius * 0.6f, radius * 2.5f);
	params.m_runtimeCameraForward =
		glm::normalize(boundsSphere.m_center - params.m_runtimeCameraPos);
	params.m_runtimeAspectRatio = 1.0f;
	params.m_bRunTasksInline = true;

	if (!pathTracer.RenderPreparedScene(params))
	{
		SAILOR_LOG_ERROR(
			"Cannot render model fingerprint: %s",
			assetFilepath.c_str());
		return false;
	}

	const glm::uvec2 extent = pathTracer.GetLastRenderedExtent();
	const TVector<u8vec4>& renderedImage =
		pathTracer.GetLastRenderedImage();
	size_t expectedPixelCount = 0;
	if (extent.x != FingerprintImageDimension ||
		extent.y != FingerprintImageDimension ||
		!TryMultiplySize(
			static_cast<size_t>(extent.x),
			static_cast<size_t>(extent.y),
			expectedPixelCount) ||
		renderedImage.Num() != expectedPixelCount)
	{
		SAILOR_LOG_ERROR(
			"Model fingerprint renderer returned an invalid image: %s",
			assetFilepath.c_str());
		return false;
	}

	EncodedFingerprint encoded;
	const int32_t channels = 4;
	if (!stbi_write_png_to_func(
		AppendEncodedFingerprintBytes,
		&encoded,
		static_cast<int32_t>(extent.x),
		static_cast<int32_t>(extent.y),
		channels,
		renderedImage.GetData(),
		static_cast<int32_t>(extent.x) * channels) ||
		!encoded.m_bValid ||
		encoded.m_bytes.IsEmpty() ||
		encoded.m_bytes.Num() > MaxFingerprintEncodedImageBytes)
	{
		SAILOR_LOG_ERROR(
			"Cannot encode model fingerprint: %s",
			assetFilepath.c_str());
		return false;
	}

	int32_t encodedWidth = 0;
	int32_t encodedHeight = 0;
	int32_t encodedChannels = 0;
	if (!stbi_info_from_memory(
		encoded.m_bytes.GetData(),
		static_cast<int>(encoded.m_bytes.Num()),
		&encodedWidth,
		&encodedHeight,
		&encodedChannels) ||
		encodedWidth != FingerprintImageDimension ||
		encodedHeight != FingerprintImageDimension ||
		encodedChannels <= 0)
	{
		SAILOR_LOG_ERROR(
			"Encoded model fingerprint is invalid: %s",
			assetFilepath.c_str());
		return false;
	}

	if (!g_activeFingerprintRequest.m_bValid)
	{
		SAILOR_LOG_ERROR(
			"Cannot publish a model fingerprint without request context: %s",
			assetFilepath.c_str());
		return false;
	}

	FileRevision currentSourceRevision;
	if (!Utils::TryGetFileRevision(
		assetFilepath,
		currentSourceRevision) ||
		currentSourceRevision !=
			g_activeFingerprintRequest.m_sourceRevision)
	{
		SAILOR_LOG(
			"Discarded stale model fingerprint: %s",
			assetFilepath.c_str());
		return false;
	}

	FingerprintTaskChain& taskChain = GetFingerprintTaskChain();
	const std::lock_guard<std::mutex> lock(taskChain.m_mutex);
	FingerprintRequest* request = nullptr;
	if (!taskChain.m_requests.Find(fileId, request) ||
		request == nullptr ||
		request->m_generation !=
			g_activeFingerprintRequest.m_generation ||
		request->m_sourceRevision !=
			g_activeFingerprintRequest.m_sourceRevision)
	{
		SAILOR_LOG(
			"Discarded superseded model fingerprint: %s",
			assetFilepath.c_str());
		return false;
	}

	std::string diagnostic;
	if (!Workspace::AtomicReplaceWorkspaceCacheBinary(
		std::filesystem::path(outputPath),
		encoded.m_bytes.GetData(),
		static_cast<uint64_t>(encoded.m_bytes.Num()),
		diagnostic))
	{
		SAILOR_LOG_ERROR(
			"Cannot atomically publish model fingerprint '%s': %s",
			outputPath.c_str(),
			diagnostic.c_str());
		return false;
	}

	SAILOR_LOG("Generated model fingerprint: %s", outputPath.c_str());
	return true;
}

static bool TryLoadYamlFile(
	const std::filesystem::path& filepath,
	YAML::Node& outDocument,
	std::string& outDiagnostic)
{
	std::string payload;
	if (!AssetRegistry::ReadAllTextFile(filepath.string(), payload))
	{
		outDiagnostic = "cannot read the file";
		return false;
	}

	return External::TryLoadYaml(payload, outDocument, outDiagnostic);
}

FileId ModelImporter::CreateTextureAsset(const std::string& filepath,
	const std::string& sourceFilename,
	uint32_t sourceTextureIndex,
	bool bShouldGenerateMips,
	RHI::EFormat format,
	RHI::ETextureClamping clamping,
	RHI::ETextureFiltration filtration,
	bool bShouldKeepCpuBuffers)
{
	AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
	if (assetRegistry == nullptr)
	{
		return FileId::Invalid;
	}

	std::error_code statusError;
	const std::filesystem::file_status metadataStatus =
		std::filesystem::symlink_status(filepath, statusError);
	if (statusError == std::errc::no_such_file_or_directory ||
		statusError == std::errc::not_a_directory)
	{
		statusError.clear();
	}
	if (statusError)
	{
		SAILOR_LOG_ERROR(
			"Cannot inspect generated texture metadata path '%s': %s",
			filepath.c_str(),
			statusError.message().c_str());
		return FileId::Invalid;
	}

	const bool bMetadataExists = std::filesystem::exists(metadataStatus);
	if (bMetadataExists &&
		!std::filesystem::is_regular_file(metadataStatus))
	{
		SAILOR_LOG_ERROR(
			"Generated texture metadata path is not a regular file: %s",
			filepath.c_str());
		return FileId::Invalid;
	}

	FileId fileId = bMetadataExists ?
		assetRegistry->RegisterGeneratedSecondaryAssetInfo(filepath) :
		FileId::CreateNewFileId();
	if (!fileId)
	{
		return FileId::Invalid;
	}

	if (bMetadataExists)
	{
		TextureAssetInfoPtr existingTextureInfo =
			assetRegistry->GetAssetInfoPtr<TextureAssetInfoPtr>(fileId);
		if (existingTextureInfo == nullptr ||
			existingTextureInfo->GetAssetFilename() != sourceFilename ||
			existingTextureInfo->GetGlbTextureIndex() !=
				static_cast<int32_t>(sourceTextureIndex))
		{
			SAILOR_LOG_ERROR(
				"Existing generated texture metadata is incompatible: %s",
				filepath.c_str());
			return FileId::Invalid;
		}

		if (existingTextureInfo->ShouldGenerateMips() ==
				bShouldGenerateMips &&
			existingTextureInfo->GetFormat() == format &&
			existingTextureInfo->GetClamping() == clamping &&
			existingTextureInfo->GetFiltration() == filtration &&
			existingTextureInfo->ShouldKeepCpuBuffers() ==
				bShouldKeepCpuBuffers)
		{
			return fileId;
		}
	}

	YAML::Node newTexture = GeneratedModelAssetMetadata::CreateTexture(
		fileId,
		sourceFilename,
		sourceTextureIndex,
		bShouldGenerateMips,
		format,
		clamping,
		filtration,
		bShouldKeepCpuBuffers);

	std::ostringstream serialized;
	serialized << newTexture;
	if (!serialized)
	{
		SAILOR_LOG_ERROR(
			"Cannot serialize generated texture metadata: %s",
			filepath.c_str());
		return {};
	}

	std::string diagnostic;
	if (!Workspace::AtomicReplaceWorkspaceCacheText(
			std::filesystem::path(filepath),
			serialized.str(),
			diagnostic))
	{
		SAILOR_LOG_ERROR(
			"Cannot save generated texture metadata '%s': %s",
			filepath.c_str(),
			diagnostic.c_str());
		return {};
	}

	if (assetRegistry->RegisterGeneratedSecondaryAssetInfo(filepath) != fileId)
	{
		SAILOR_LOG_ERROR(
			"Cannot register generated texture metadata for immediate model processing: %s",
			filepath.c_str());
		return FileId::Invalid;
	}

	return fileId;
}

FileId CreateAnimationAsset(const std::string& filepath,
	const std::string& glbFilename,
	uint32_t animationIndex,
	uint32_t skinIndex)
{
	FileId newFileId = FileId::CreateNewFileId();

	YAML::Node newAnimation = GeneratedModelAssetMetadata::CreateAnimation(
		newFileId,
		glbFilename,
		animationIndex,
		skinIndex);

	std::ostringstream serialized;
	serialized << newAnimation;
	if (!serialized)
	{
		SAILOR_LOG_ERROR(
			"Cannot serialize generated animation metadata: %s",
			filepath.c_str());
		return {};
	}

	std::string diagnostic;
	if (!Workspace::AtomicReplaceWorkspaceCacheText(
			std::filesystem::path(filepath),
			serialized.str(),
			diagnostic))
	{
		SAILOR_LOG_ERROR(
			"Cannot save generated animation metadata '%s': %s",
			filepath.c_str(),
			diagnostic.c_str());
		return {};
	}

	return newFileId;
}

bool ModelImporter::GenerateAnimationAssets(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	tinygltf::Model gltfModel;
	std::string err, warn;
	const bool bGltfParsed = GltfImporterUtils::LoadModel(
		assetInfo->GetAssetFilepath(),
		true,
		gltfModel,
		err,
		warn);

	if (!bGltfParsed)
	{
		return false;
	}

	if (gltfModel.animations.empty())
	{
		const bool bChanged = assetInfo->GetAnimations().Num() > 0;
		assetInfo->GetAnimations().Clear();
		return bChanged;
	}

	const std::string animationsFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());
	TVector<FileId> generatedAnimations;
	generatedAnimations.Reserve(gltfModel.animations.size());

	for (size_t i = 0; i < gltfModel.animations.size(); ++i)
	{
		std::filesystem::path outputPath;
		if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
				animationsFolder + assetInfo->GetAssetFilename() +
					"_animation_" + std::to_string(i) + ".anim.asset",
				outputPath))
		{
			SAILOR_LOG_ERROR("Cannot resolve generated animation output for %s.", assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		const FileId id = CreateAnimationAsset(outputPath.string(),
			assetInfo->GetAssetFilename(), (uint32_t)i, 0);
		if (!id)
		{
			return false;
		}
		generatedAnimations.Add(id);
	}

	assetInfo->GetAnimations() = std::move(generatedAnimations);
	return true;
}

bool ModelImporter::GenerateMaterialAssets(ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();

	tinygltf::Model gltfModel;
	std::string err;
	std::string warn;
	const bool bGltfParsed = GltfImporterUtils::LoadModel(
		assetInfo->GetAssetFilepath(),
		true,
		gltfModel,
		err,
		warn);

	if (!err.empty())
	{
		SAILOR_LOG_ERROR("Parsing gltf %s error: %s", assetInfo->GetAssetFilepath().c_str(), err.c_str());
	}

	if (!warn.empty())
	{
		SAILOR_LOG("Parsing gltf %s warning: %s", assetInfo->GetAssetFilepath().c_str(), warn.c_str());
	}

	if (!bGltfParsed)
	{
		return false;
	}

	const std::string texturesFolder = Utils::GetFileFolder(assetInfo->GetRelativeAssetFilepath());

	TVector<MaterialAsset::Data> materials(gltfModel.materials.size());

	for (size_t i = 0; i < gltfModel.materials.size(); ++i)
	{
		const auto& material = gltfModel.materials[i];

		MaterialAsset::Data& data = materials[i];
		data.m_name = !material.name.empty() ? material.name : ("material" + std::to_string(i));

		std::filesystem::path materialNamePath;
		if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
				texturesFolder + assetInfo->GetAssetFilename() +
					"_material_" + std::to_string(i),
				materialNamePath))
		{
			SAILOR_LOG_ERROR("Cannot resolve generated material output for %s.", assetInfo->GetAssetFilepath().c_str());
			return false;
		}
		const std::string materialName = materialNamePath.string();

		if (material.pbrMetallicRoughness.baseColorTexture.index != -1)
		{
			data.m_samplers.Add("baseColorSampler",
				CreateTextureAsset(materialName + "_baseColorTexture.png.asset", assetInfo->GetAssetFilename(), material.pbrMetallicRoughness.baseColorTexture.index, true, RHI::ETextureFormat::R8G8B8A8_SRGB, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
		}

		if (material.normalTexture.index != -1)
		{
			data.m_samplers.Add("normalSampler",
				CreateTextureAsset(materialName + "_normalTexture.png.asset", assetInfo->GetAssetFilename(), material.normalTexture.index, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
		}

		if (material.emissiveTexture.index != -1)
		{
			data.m_samplers.Add("emissiveSampler",
				CreateTextureAsset(materialName + "_emissionTexture.png.asset", assetInfo->GetAssetFilename(), material.emissiveTexture.index, true, RHI::ETextureFormat::R8G8B8A8_SRGB, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
		}

		if (material.pbrMetallicRoughness.metallicRoughnessTexture.index != -1)
		{
			data.m_samplers.Add("ormSampler",
				CreateTextureAsset(materialName + "_ormTexture.png.asset", assetInfo->GetAssetFilename(), material.pbrMetallicRoughness.metallicRoughnessTexture.index, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
		}

		if (material.occlusionTexture.index != -1)
		{
			data.m_samplers.Add("occlusionSampler",
				CreateTextureAsset(materialName + "_occlusionTexture.png.asset", assetInfo->GetAssetFilename(), material.occlusionTexture.index, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
		}

		auto tryReadNumberProperty = [](
			const tinygltf::Value& object,
			const char* property,
			double& outValue)
			{
				if (!object.IsObject() || !object.Has(property))
				{
					return false;
				}

				const tinygltf::Value& value = object.Get(property);
				if (!value.IsNumber())
				{
					return false;
				}

				const double parsedValue = value.GetNumberAsDouble();
				if (!std::isfinite(parsedValue) ||
					parsedValue > std::numeric_limits<float>::max() ||
					parsedValue < -std::numeric_limits<float>::max())
				{
					return false;
				}

				outValue = parsedValue;
				return true;
			};

		auto tryReadTextureIndex = [&gltfModel](
			const tinygltf::Value& object,
			const char* property,
			int32_t& outIndex)
			{
				if (!object.IsObject() || !object.Has(property))
				{
					return false;
				}

				const tinygltf::Value& textureInfo = object.Get(property);
				if (!textureInfo.IsObject() || !textureInfo.Has("index"))
				{
					return false;
				}

				const tinygltf::Value& indexValue = textureInfo.Get("index");
				if (!indexValue.IsInt())
				{
					return false;
				}

				const int32_t index = indexValue.GetNumberAsInt();
				if (index < 0 ||
					static_cast<size_t>(index) >= gltfModel.textures.size())
				{
					return false;
				}

				outIndex = index;
				return true;
			};

		auto tryReadVec3Property = [](
			const tinygltf::Value& object,
			const char* property,
			glm::vec3& outValue)
			{
				if (!object.IsObject() || !object.Has(property))
				{
					return false;
				}

				const tinygltf::Value& array = object.Get(property);
				if (!array.IsArray() || array.ArrayLen() < 3)
				{
					return false;
				}

				glm::vec3 parsedValue(0.0f);
				for (size_t component = 0; component < 3; ++component)
				{
					const tinygltf::Value& value = array.Get(component);
					if (!value.IsNumber())
					{
						return false;
					}

					const double parsedComponent = value.GetNumberAsDouble();
					if (!std::isfinite(parsedComponent) ||
						parsedComponent > std::numeric_limits<float>::max() ||
						parsedComponent < -std::numeric_limits<float>::max())
					{
						return false;
					}
					parsedValue[static_cast<int32_t>(component)] =
						static_cast<float>(parsedComponent);
				}

				outValue = parsedValue;
				return true;
			};

		const auto transmissionSettings =
			GltfImporterUtils::ResolveMaterialTransmission(
				material,
				gltfModel.textures.size(),
				assetInfo->GetUnitScale());
		if (transmissionSettings.IsEnabled())
		{
			data.m_uniformsFloat.Add(
				"material.transmissionFactor",
				transmissionSettings.m_factor);
			data.m_uniformsFloat.Add(
				"material.thicknessFactor",
				transmissionSettings.m_thicknessFactor);
			data.m_uniformsFloat.Add(
				"material.attenuationDistance",
				transmissionSettings.m_attenuationDistance);
			data.m_uniformsFloat.Add(
				"material.indexOfRefraction",
				transmissionSettings.m_indexOfRefraction);
			data.m_uniformsVec4.Add(
				"material.attenuationColor",
				glm::vec4(
					transmissionSettings.m_attenuationColor,
					1.0f));
			if (transmissionSettings.m_textureIndex >= 0)
			{
				data.m_samplers.Add("transmissionSampler",
					CreateTextureAsset(materialName + "_transmissionTexture.png.asset", assetInfo->GetAssetFilename(), transmissionSettings.m_textureIndex, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
			}
			if (transmissionSettings.m_thicknessTextureIndex >= 0)
			{
				data.m_samplers.Add("thicknessSampler",
					CreateTextureAsset(materialName + "_thicknessTexture.png.asset", assetInfo->GetAssetFilename(), transmissionSettings.m_thicknessTextureIndex, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
			}
			data.m_shaderDefines.Add("TRANSMISSION");
		}

		int32_t textureIndex = -1;
		auto ccIt = material.extensions.find("KHR_materials_clearcoat");
		if (ccIt != material.extensions.end() && ccIt->second.IsObject())
		{
			const tinygltf::Value& cc = ccIt->second;

			double ccFactor = 0.0;
			tryReadNumberProperty(cc, "clearcoatFactor", ccFactor);

			double ccRoughness = 0.0;
			tryReadNumberProperty(
				cc,
				"clearcoatRoughnessFactor",
				ccRoughness);

			data.m_uniformsFloat.Add("material.clearcoatFactor", (float)ccFactor);
			data.m_uniformsFloat.Add("material.clearcoatRoughnessFactor", (float)ccRoughness);

			textureIndex = -1;
			if (tryReadTextureIndex(cc, "clearcoatTexture", textureIndex))
			{
				data.m_samplers.Add("clearcoatSampler",
					CreateTextureAsset(materialName + "_clearcoatTexture.png.asset", assetInfo->GetAssetFilename(), textureIndex, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
			}

			textureIndex = -1;
			if (tryReadTextureIndex(
				cc,
				"clearcoatRoughnessTexture",
				textureIndex))
			{
				data.m_samplers.Add("clearcoatRoughnessSampler",
					CreateTextureAsset(materialName + "_clearcoatRoughnessTexture.png.asset", assetInfo->GetAssetFilename(), textureIndex, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
			}

			if (cc.Has("clearcoatNormalTexture") &&
				cc.Get("clearcoatNormalTexture").IsObject())
			{
				const tinygltf::Value& tex = cc.Get("clearcoatNormalTexture");
				double scale = 1.0;
				tryReadNumberProperty(tex, "scale", scale);

				textureIndex = -1;
				if (tryReadTextureIndex(
					cc,
					"clearcoatNormalTexture",
					textureIndex))
				{
					data.m_samplers.Add("clearcoatNormalSampler",
						CreateTextureAsset(materialName + "_clearcoatNormalTexture.png.asset", assetInfo->GetAssetFilename(), textureIndex, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
				}
				data.m_uniformsFloat.Add("material.clearcoatNormalScale", (float)scale);
			}

			data.m_shaderDefines.Add("CLEAR_COAT");
		}

		auto sheenIt = material.extensions.find("KHR_materials_sheen");
		if (sheenIt != material.extensions.end() &&
			sheenIt->second.IsObject())
		{
			const tinygltf::Value& sheen = sheenIt->second;

			glm::vec3 color = glm::vec3(0.0f);
			tryReadVec3Property(sheen, "sheenColorFactor", color);

			double roughness = 0.0;
			tryReadNumberProperty(
				sheen,
				"sheenRoughnessFactor",
				roughness);

			data.m_uniformsVec4.Add("material.sheenColorFactor", glm::vec4(color, 0.0f));
			data.m_uniformsFloat.Add("material.sheenRoughnessFactor", (float)roughness);

			textureIndex = -1;
			if (tryReadTextureIndex(
				sheen,
				"sheenColorTexture",
				textureIndex))
			{
				data.m_samplers.Add("sheenColorSampler",
					CreateTextureAsset(materialName + "_sheenColorTexture.png.asset", assetInfo->GetAssetFilename(), textureIndex, true, RHI::ETextureFormat::R8G8B8A8_SRGB, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
			}

			textureIndex = -1;
			if (tryReadTextureIndex(
				sheen,
				"sheenRoughnessTexture",
				textureIndex))
			{
				data.m_samplers.Add("sheenRoughnessSampler",
					CreateTextureAsset(materialName + "_sheenRoughnessTexture.png.asset", assetInfo->GetAssetFilename(), textureIndex, true, RHI::ETextureFormat::R8G8B8A8_UNORM, RHI::ETextureClamping::Repeat, RHI::ETextureFiltration::Linear, assetInfo->ShouldKeepCpuBuffers()));
			}

			data.m_shaderDefines.Add("SHEEN");
		}

		const vec4 baseColor = vec4((float)material.pbrMetallicRoughness.baseColorFactor[0],
			(float)material.pbrMetallicRoughness.baseColorFactor[1],
			(float)material.pbrMetallicRoughness.baseColorFactor[2],
			(float)material.pbrMetallicRoughness.baseColorFactor[3]);

		const vec4 emissiveFactor = vec4((float)material.emissiveFactor[0], (float)material.emissiveFactor[1], (float)material.emissiveFactor[2], 0.0f);

		data.m_uniformsVec4.Add("material.baseColorFactor", baseColor);
		data.m_uniformsVec4.Add("material.emissiveFactor", emissiveFactor);

		data.m_uniformsFloat.Add("material.roughnessFactor", (float)material.pbrMetallicRoughness.roughnessFactor);
		data.m_uniformsFloat.Add("material.metallicFactor", (float)material.pbrMetallicRoughness.metallicFactor);
		data.m_uniformsFloat.Add("material.normalScale", (float)material.normalTexture.scale);
		data.m_uniformsFloat.Add("material.alphaCutoff", (float)material.alphaCutoff);
		data.m_uniformsFloat.Add("material.occlusionStrength", (float)material.occlusionTexture.strength);

		const auto alphaModeSettings =
			GltfImporterUtils::ResolveMaterialAlphaMode(
				material.alphaMode,
				transmissionSettings.IsEnabled());
		data.m_renderQueue = alphaModeSettings.m_renderQueue;

		if (alphaModeSettings.m_bAlphaCutout)
		{
			data.m_shaderDefines.Add("ALPHA_CUTOUT");
		}

		data.m_renderState = RHI::RenderState(true,
			alphaModeSettings.m_bEnableZWrite,
			0.0f,
			alphaModeSettings.m_bAlphaCutout,
			material.doubleSided ? RHI::ECullMode::None : RHI::ECullMode::Back,
			alphaModeSettings.m_blendMode,
			RHI::EFillMode::Fill,
			GetHash(data.m_renderQueue));

		data.m_shader = App::GetSubmodule<AssetRegistry>()->GetOrLoadFile("Shaders/Standard_glTF.shader");
		for (const auto& sampler : data.m_samplers)
		{
			if (sampler.m_second == nullptr || !*sampler.m_second)
			{
				SAILOR_LOG_ERROR(
					"Cannot create generated texture metadata for %s.",
					assetInfo->GetAssetFilepath().c_str());
				return false;
			}
		}
	}

	std::filesystem::path materialsFolder;
	if (!App::GetSubmodule<AssetRegistry>()->ResolveWorkspaceContentPathForWrite(
			texturesFolder + "materials",
			materialsFolder))
	{
		SAILOR_LOG_ERROR("Cannot resolve generated materials folder for %s.", assetInfo->GetAssetFilepath().c_str());
		return false;
	}
	std::error_code directoryError;
	std::filesystem::create_directories(materialsFolder, directoryError);
	if (directoryError)
	{
		SAILOR_LOG_ERROR("Cannot create generated materials folder for %s: %s",
			assetInfo->GetAssetFilepath().c_str(),
			directoryError.message().c_str());
		return false;
	}

	TVector<FileId> materialFiles;
	materialFiles.Reserve(materials.Num());
	for (size_t i = 0; i < materials.Num(); ++i)
	{
		const MaterialAsset::Data& material = materials[i];

		const FileId materialFileId = App::GetSubmodule<MaterialImporter>()->CreateMaterialAsset(
			(materialsFolder /
				(assetInfo->GetAssetFilename() + "_material_" +
					std::to_string(i) + ".mat")).string(),
			material);
		if (!materialFileId)
		{
			return false;
		}
		materialFiles.Add(materialFileId);
	}

	TVector<FileId> generatedMaterials;
	if (assetInfo->ShouldBatchByMaterial())
	{
		generatedMaterials = std::move(materialFiles);
	}
	else
	{
		for (const tinygltf::Mesh& mesh : gltfModel.meshes)
		{
			for (const tinygltf::Primitive& primitive : mesh.primitives)
			{
				if (primitive.material < 0 ||
					static_cast<size_t>(primitive.material) >= materialFiles.Num())
				{
					SAILOR_LOG_ERROR(
						"Cannot resolve primitive material for %s.",
						assetInfo->GetAssetFilepath().c_str());
					return false;
				}
				generatedMaterials.Add(materialFiles[primitive.material]);
			}
		}
	}

	assetInfo->GetDefaultMaterials() = std::move(generatedMaterials);
	bool& bMigrationComplete =
		m_generatedMaterialMigrationComplete.At_Lock(
			assetInfo->GetFileId(),
			false);
	bMigrationComplete = true;
	m_generatedMaterialMigrationComplete.Unlock(assetInfo->GetFileId());
	return true;
}

bool ModelImporter::UpdateGeneratedMaterialProperties(
	ModelAssetInfoPtr assetInfo)
{
	SAILOR_PROFILE_FUNCTION();
	if (assetInfo == nullptr || !assetInfo->IsWritable())
	{
		return false;
	}

	tinygltf::Model gltfModel;
	std::string error;
	std::string warning;
	if (!GltfImporterUtils::LoadModel(
			assetInfo->GetAssetFilepath(),
			true,
			gltfModel,
			error,
			warning))
	{
		SAILOR_LOG_ERROR(
			"Cannot update generated materials for %s: %s",
			assetInfo->GetAssetFilepath().c_str(),
			error.c_str());
		return false;
	}

	if (!warning.empty())
	{
		SAILOR_LOG(
			"Parsing gltf %s warning: %s",
			assetInfo->GetAssetFilepath().c_str(),
			warning.c_str());
	}

	const FileId modelId = assetInfo->GetFileId();
	bool& bMigrationComplete = m_generatedMaterialMigrationComplete.At_Lock(
		modelId,
		false);
	const bool bUpdated = UpdateGeneratedMaterialProperties(
		assetInfo,
		gltfModel);
	bMigrationComplete = bUpdated;
	m_generatedMaterialMigrationComplete.Unlock(modelId);
	return bUpdated;
}

bool ModelImporter::UpdateGeneratedMaterialPropertiesOnDemand(
	ModelAssetInfoPtr assetInfo,
	const tinygltf::Model& gltfModel)
{
	if (assetInfo == nullptr ||
		!assetInfo->IsWritable() ||
		!assetInfo->ShouldGenerateMaterials() ||
		assetInfo->GetDefaultMaterials().IsEmpty())
	{
		return true;
	}

	const FileId modelId = assetInfo->GetFileId();
	bool& bMigrationComplete = m_generatedMaterialMigrationComplete.At_Lock(
		modelId,
		false);
	if (bMigrationComplete)
	{
		m_generatedMaterialMigrationComplete.Unlock(modelId);
		return true;
	}

	const bool bUpdated = UpdateGeneratedMaterialProperties(
		assetInfo,
		gltfModel);
	bMigrationComplete = bUpdated;
	m_generatedMaterialMigrationComplete.Unlock(modelId);
	return bUpdated;
}

bool ModelImporter::UpdateGeneratedMaterialProperties(
	ModelAssetInfoPtr assetInfo,
	const tinygltf::Model& gltfModel)
{
	SAILOR_PROFILE_FUNCTION();
	if (assetInfo == nullptr || !assetInfo->IsWritable())
	{
		return false;
	}

	AssetRegistry* assetRegistry = App::GetSubmodule<AssetRegistry>();
	if (assetRegistry == nullptr)
	{
		return false;
	}

	const std::string relativeFolder = Utils::GetFileFolder(
		assetInfo->GetRelativeAssetFilepath());
	std::filesystem::path materialsFolder;
	if (!assetRegistry->ResolveWorkspaceContentPathForWrite(
			relativeFolder + "materials",
			materialsFolder))
	{
		SAILOR_LOG_ERROR(
			"Cannot resolve generated materials folder for %s.",
			assetInfo->GetAssetFilepath().c_str());
		return false;
	}

	auto sanitizeLegacyMaterialStem = [](const std::string& materialName,
		size_t materialIndex)
		{
			std::string result = materialName.empty() ?
				("material" + std::to_string(materialIndex)) :
				materialName;
			constexpr const char* InvalidFilenameCharacters =
				"<>:\"/\\|?*";
			for (char& character : result)
			{
				if (static_cast<unsigned char>(character) < 32 ||
					std::strchr(InvalidFilenameCharacters, character) !=
						nullptr)
				{
					character = '_';
				}
			}
			while (!result.empty() &&
				(result.back() == '.' || result.back() == ' '))
			{
				result.back() = '_';
			}
			return result.empty() ?
				("material" + std::to_string(materialIndex)) :
				result;
		};

	auto findOwnedMaterial = [assetInfo, assetRegistry](
		size_t materialIndex,
		const std::filesystem::path& indexedPath,
		const std::filesystem::path& legacyPath,
		MaterialAssetInfoPtr& outMaterialInfo)
		{
			auto tryMatch = [assetRegistry,
				&indexedPath,
				&legacyPath,
				&outMaterialInfo](const FileId& materialId)
			{
				MaterialAssetInfoPtr materialInfo =
					assetRegistry->GetAssetInfoPtr<MaterialAssetInfoPtr>(
						materialId);
				if (materialInfo == nullptr ||
					!materialInfo->IsWritable())
				{
					return false;
				}

				for (const std::filesystem::path& candidate : {
					indexedPath,
					legacyPath })
				{
					std::error_code equivalentError;
					if (std::filesystem::equivalent(
							candidate,
							materialInfo->GetAssetFilepath(),
							equivalentError) &&
						!equivalentError)
					{
						outMaterialInfo = materialInfo;
						return true;
					}
				}
				return false;
			};

			const TVector<FileId>& defaultMaterials =
				assetInfo->GetDefaultMaterials();
			if (assetInfo->ShouldBatchByMaterial())
			{
				// Batched models retain the direct glTF material ordering. Requiring
				// both the position and a known generated path avoids claiming a
				// separately authored replacement material.
				return materialIndex < defaultMaterials.Num() &&
					tryMatch(defaultMaterials[materialIndex]);
			}

			for (const FileId& materialId : defaultMaterials)
			{
				if (tryMatch(materialId))
				{
					return true;
				}
			}

			return false;
		};

	TVector<FileId> registeredTextureIds;
	assetRegistry->GetAssetInfoIdsByTypeAndSource(
		"Sailor::TextureAssetInfo",
		assetInfo->GetAssetFilepath(),
		registeredTextureIds);
	TMap<int32_t, FileId> textureIdsByGltfIndex;
	for (const FileId& registeredTextureId : registeredTextureIds)
	{
		TextureAssetInfoPtr textureInfo =
			assetRegistry->GetAssetInfoPtr<TextureAssetInfoPtr>(
				registeredTextureId);
		if (textureInfo == nullptr ||
			textureInfo->GetGlbTextureIndex() < 0 ||
			textureInfo->GetFormat() !=
				RHI::ETextureFormat::R8G8B8A8_UNORM ||
			textureInfo->GetClamping() !=
				RHI::ETextureClamping::Repeat ||
			textureInfo->GetFiltration() !=
				RHI::ETextureFiltration::Linear ||
			!textureInfo->ShouldGenerateMips())
		{
			continue;
		}

		std::error_code sourceError;
		if (std::filesystem::equivalent(
				textureInfo->GetAssetFilepath(),
				assetInfo->GetAssetFilepath(),
				sourceError) &&
			!sourceError)
		{
			textureIdsByGltfIndex.Insert(
				textureInfo->GetGlbTextureIndex(),
				registeredTextureId);
		}
	}
	TSet<FileId> updatedMaterialIds;
	bool bSucceeded = true;
	for (size_t materialIndex = 0;
		materialIndex < gltfModel.materials.size();
		++materialIndex)
	{
		const std::string generatedStem =
			assetInfo->GetAssetFilename() + "_material_" +
			std::to_string(materialIndex);
		const std::filesystem::path indexedMaterialPath =
			materialsFolder / (generatedStem + ".mat");
		const std::string legacyMaterialStem =
			sanitizeLegacyMaterialStem(
				gltfModel.materials[materialIndex].name,
				materialIndex);
		const std::filesystem::path legacyMaterialPath =
			materialsFolder / (legacyMaterialStem + ".mat");
		MaterialAssetInfoPtr materialInfo = nullptr;
		if (!findOwnedMaterial(
				materialIndex,
				indexedMaterialPath,
				legacyMaterialPath,
				materialInfo))
		{
			// A default material may be replaced with a separately authored asset.
			// Do not infer ownership from its position in the model's material list.
			SAILOR_LOG(
				"Skipped non-generated material while updating %s: %s",
				assetInfo->GetAssetFilepath().c_str(),
				indexedMaterialPath.string().c_str());
			continue;
		}

		const tinygltf::Material& sourceMaterial =
			gltfModel.materials[materialIndex];
		const auto transmission =
			GltfImporterUtils::ResolveMaterialTransmission(
				sourceMaterial,
				gltfModel.textures.size(),
				assetInfo->GetUnitScale());
		const auto alphaMode = GltfImporterUtils::ResolveMaterialAlphaMode(
			sourceMaterial.alphaMode,
			transmission.IsEnabled());
		YAML::Node generatedProperties(YAML::NodeType::Map);
		generatedProperties["renderQueue"] = alphaMode.m_renderQueue;
		generatedProperties["bEnableZWrite"] =
			alphaMode.m_bEnableZWrite;
		generatedProperties["bCustomDepthShader"] =
			alphaMode.m_bAlphaCutout;
		::Serialize(
			generatedProperties,
			"blendMode",
			alphaMode.m_blendMode);

		YAML::Node generatedDefines(YAML::NodeType::Sequence);
		if (transmission.IsEnabled())
		{
			generatedDefines.push_back("TRANSMISSION");
		}
		if (alphaMode.m_bAlphaCutout)
		{
			generatedDefines.push_back("ALPHA_CUTOUT");
		}
		generatedProperties["defines"] = generatedDefines;
		generatedProperties["uniformsFloat"]["material.alphaCutoff"] =
			static_cast<float>(sourceMaterial.alphaCutoff);

		if (transmission.IsEnabled())
		{
			generatedProperties["uniformsFloat"]
				["material.transmissionFactor"] = transmission.m_factor;
			generatedProperties["uniformsFloat"]
				["material.thicknessFactor"] =
					transmission.m_thicknessFactor;
			generatedProperties["uniformsFloat"]
				["material.attenuationDistance"] =
					transmission.m_attenuationDistance;
			generatedProperties["uniformsFloat"]
				["material.indexOfRefraction"] =
					transmission.m_indexOfRefraction;
			generatedProperties["uniformsVec4"]
				["material.attenuationColor"] = glm::vec4(
					transmission.m_attenuationColor,
					1.0f);

			auto addGeneratedSampler = [assetInfo,
				assetRegistry,
				materialIndex,
				&relativeFolder,
				&generatedProperties,
				&textureIdsByGltfIndex,
				&bSucceeded](
					const char* samplerName,
					const char* assetSuffix,
					int32_t textureIndex)
				{
					if (textureIndex < 0)
					{
						return;
					}

					const FileId* registeredTextureId = nullptr;
					FileId textureFileId =
						textureIdsByGltfIndex.Find(
							textureIndex,
							registeredTextureId) &&
						registeredTextureId != nullptr ?
							*registeredTextureId : FileId();

					if (!textureFileId)
					{
						std::filesystem::path generatedTexturePath;
						const std::string generatedTextureVirtualPath =
							relativeFolder + assetInfo->GetAssetFilename() +
							"_material_" + std::to_string(materialIndex) + "_" +
							assetSuffix + ".png.asset";
						if (!assetRegistry->ResolveWorkspaceContentPathForWrite(
								generatedTextureVirtualPath,
								generatedTexturePath))
						{
							SAILOR_LOG_ERROR(
								"Cannot resolve generated glTF %s for %s.",
								samplerName,
								assetInfo->GetAssetFilepath().c_str());
							bSucceeded = false;
							return;
						}

						textureFileId = ModelImporter::CreateTextureAsset(
							generatedTexturePath.string(),
							assetInfo->GetAssetFilename(),
							static_cast<uint32_t>(textureIndex),
							true,
							RHI::ETextureFormat::R8G8B8A8_UNORM,
							RHI::ETextureClamping::Repeat,
							RHI::ETextureFiltration::Linear,
							assetInfo->ShouldKeepCpuBuffers());
						if (!textureFileId)
						{
							SAILOR_LOG_ERROR(
								"Cannot create generated glTF %s for %s.",
								samplerName,
								assetInfo->GetAssetFilepath().c_str());
							bSucceeded = false;
							return;
						}

						textureIdsByGltfIndex.Insert(
							textureIndex,
							textureFileId);
					}

					generatedProperties["samplers"][samplerName] =
						textureFileId;
				};

			addGeneratedSampler(
				"transmissionSampler",
				"transmissionTexture",
				transmission.m_textureIndex);
			addGeneratedSampler(
				"thicknessSampler",
				"thicknessTexture",
				transmission.m_thicknessTextureIndex);
		}

		YAML::Node materialDocument;
		std::string diagnostic;
		if (!TryLoadYamlFile(
				materialInfo->GetAssetFilepath(),
				materialDocument,
				diagnostic))
		{
			SAILOR_LOG_ERROR(
				"Cannot read generated material '%s': %s",
				materialInfo->GetAssetFilepath().c_str(),
				diagnostic.c_str());
			bSucceeded = false;
			continue;
		}

		const YAML::Node previousDocument = YAML::Clone(materialDocument);
		bool bMerged = false;
		const bool bYamlHandled = External::GuardYamlExceptions(
			[&materialDocument, &generatedProperties, &bMerged]()
			{
				bMerged = GltfImporterUtils::MergeGeneratedMaterialProperties(
					materialDocument,
					generatedProperties);
			},
			diagnostic);
		if (!bYamlHandled || !bMerged)
		{
			if (diagnostic.empty())
			{
				diagnostic = "material YAML has an incompatible structure";
			}
			SAILOR_LOG_ERROR(
				"Cannot migrate generated material '%s': %s",
				materialInfo->GetAssetFilepath().c_str(),
				diagnostic.c_str());
			bSucceeded = false;
			continue;
		}

		if (Utils::AreYamlNodesEqual(previousDocument, materialDocument))
		{
			continue;
		}

		std::string serializedMaterial;
		if (!External::TryDumpYaml(
				materialDocument,
				serializedMaterial,
				diagnostic) ||
			!Workspace::AtomicReplaceWorkspaceCacheText(
				materialInfo->GetAssetFilepath(),
				serializedMaterial,
				diagnostic))
		{
			SAILOR_LOG_ERROR(
				"Cannot save migrated material '%s': %s",
				materialInfo->GetAssetFilepath().c_str(),
				diagnostic.c_str());
			bSucceeded = false;
			continue;
		}

		updatedMaterialIds.Insert(materialInfo->GetFileId());
	}

	for (const FileId& materialId : updatedMaterialIds)
	{
		if (!assetRegistry->UpdateAsset(materialId))
		{
			SAILOR_LOG_ERROR(
				"Cannot reload migrated generated material: %s",
				materialId.ToString().c_str());
			bSucceeded = false;
		}
	}

	return bSucceeded;
}

void ModelImporter::PopulateModelSceneHierarchy(
	Model& model,
	TVector<GltfImporterUtils::SceneNode>& sourceNodes)
{
	model.m_nodes.Clear();
	model.m_renderInstances.Clear();
	model.m_bSupportsEditableHierarchy = true;
	model.m_nodes.Reserve(sourceNodes.Num());
	for (auto& sourceNode : sourceNodes)
	{
		Model::Node node{};
		node.m_name = std::move(sourceNode.m_name);
		node.m_sourceNodeIndex = sourceNode.m_sourceNodeIndex;
		node.m_parentIndex = sourceNode.m_parentIndex;
		node.m_meshIndex = sourceNode.m_meshIndex;
		node.m_skinIndex = sourceNode.m_skinIndex;
		node.m_localTransform = sourceNode.m_localTransform;
		node.m_localMatrix = sourceNode.m_localMatrix;
		node.m_worldMatrix = sourceNode.m_worldMatrix;
		node.m_bTransformDecomposable =
			sourceNode.m_bTransformDecomposable;
		model.m_bSupportsEditableHierarchy &=
			node.m_bTransformDecomposable && node.m_skinIndex < 0;
		model.m_nodes.Add(std::move(node));
	}

	for (size_t nodeIndex = 0; nodeIndex < model.m_nodes.Num(); ++nodeIndex)
	{
		const Model::Node& node = model.m_nodes[nodeIndex];
		if (!model.IsSourceMeshIndexValid(node.m_meshIndex))
		{
			continue;
		}

		const Model::SourceMesh& sourceMesh = model.m_sourceMeshes[
			static_cast<size_t>(node.m_meshIndex)];
		for (uint32_t renderMeshIndex : sourceMesh.m_renderMeshIndices)
		{
			Model::RenderInstance instance{};
			instance.m_renderMeshIndex = renderMeshIndex;
			instance.m_nodeIndex = static_cast<int32_t>(nodeIndex);
			instance.m_modelMatrix = node.m_skinIndex >= 0 ?
				glm::mat4(1.0f) : node.m_worldMatrix;
			model.m_renderInstances.Add(std::move(instance));
		}
	}
}

Tasks::TaskPtr<ModelPtr> ModelImporter::LoadModel(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();
	ModelAssetInfoPtr pAssetInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid);

	// Check promises first
	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedModel = m_loadedModels.At_Lock(uid, ModelPtr());

	// Check loaded assets
	if (loadedModel)
	{
		const bool bNeedCpuBuffers = pAssetInfo &&
			pAssetInfo->ShouldKeepCpuBuffers() &&
			!loadedModel->HasCpuMeshes();
		if (bNeedCpuBuffers && !promise)
		{
			loadedModel = nullptr;
		}
		else
		{
			outModel = loadedModel;
			auto res = promise ? promise : Tasks::TaskPtr<ModelPtr>::Make(outModel);

			m_loadedModels.Unlock(uid);
			m_promises.Unlock(uid);

			return res;
		}
	}

	// There is no promise, we need to load model
	if (pAssetInfo)
	{
		SAILOR_PROFILE_TEXT(pAssetInfo->GetAssetFilepath().c_str());

		ModelPtr pModel = ModelPtr::Make(m_allocator, uid);

		// The way to drop qualifiers inside lambda
		auto& boundsSphere = pModel->m_boundsSphere;
		auto& boundsAabb = pModel->m_boundsAabb;

		struct Data
		{
			TVector<MeshContext> m_parsedMeshes;
			TVector<glm::mat4> m_inverseBind;
			TVector<GltfImporterUtils::SceneNode> m_sceneNodes;
			TVector<std::string> m_sourceMeshNames;
			tinygltf::Model m_gltfModel;
			bool m_bIsImported = false;
			bool m_bShouldKeepCpuBuffers = false;
			bool m_bShouldGenerateBLAS = false;
		};

		auto loadDataTask = Tasks::CreateTaskWithResult<TSharedPtr<Data>>("Load model",
			[pAssetInfo, &boundsAabb, &boundsSphere]()
			{
				TSharedPtr<Data> pData = TSharedPtr<Data>::Make();
				pData->m_bShouldKeepCpuBuffers = pAssetInfo->ShouldKeepCpuBuffers();
				pData->m_bShouldGenerateBLAS = pAssetInfo->ShouldGenerateBLAS();
				pData->m_bIsImported = ImportModel(
					pAssetInfo->GetAssetFilepath(),
					pAssetInfo->GetUnitScale(),
					pAssetInfo->ShouldBatchByMaterial(),
					pData->m_parsedMeshes,
					boundsAabb,
					boundsSphere,
					pData->m_inverseBind,
					&pData->m_gltfModel);
				if (pData->m_bIsImported)
				{
					pData->m_bIsImported =
						GltfImporterUtils::CollectSceneNodes(
							pData->m_gltfModel,
							pAssetInfo->GetUnitScale(),
							pData->m_sceneNodes);
				}
				if (pData->m_bIsImported)
				{
					pData->m_sourceMeshNames.Reserve(
						pData->m_gltfModel.meshes.size());
					for (size_t meshIndex = 0;
						meshIndex < pData->m_gltfModel.meshes.size();
						++meshIndex)
					{
						const std::string& sourceName =
							pData->m_gltfModel.meshes[meshIndex].name;
						pData->m_sourceMeshNames.Add(
							sourceName.empty() ?
								"Mesh_" + std::to_string(meshIndex) :
								sourceName);
					}
				}
				return pData;
			});
		auto migrationTask = loadDataTask->Then(
				[this, pAssetInfo, uid](TSharedPtr<Data> pData)
				{
					if (pData->m_bIsImported)
					{
						UpdateGeneratedMaterialPropertiesOnDemand(
							pAssetInfo,
							pData->m_gltfModel);
					}
					pData->m_gltfModel = tinygltf::Model();
					m_generatedMaterialMigrationTasks.Remove(uid);
				},
				"Migrate generated model materials",
				EThreadType::Main);
		m_generatedMaterialMigrationTasks.At_Lock(uid, nullptr) = migrationTask;
		m_generatedMaterialMigrationTasks.Unlock(uid);

		promise = loadDataTask->Then<ModelPtr>([pModel](TSharedPtr<Data> pData) mutable
				{
					if (pData->m_bIsImported)
					{
						pModel->m_meshes.Clear();
						pModel->m_cpuMeshes.Clear();
						pModel->m_nodes.Clear();
						pModel->m_sourceMeshes.Clear();
						pModel->m_renderInstances.Clear();
						pModel->m_bSupportsEditableHierarchy = true;
						pModel->m_meshes.Reserve(pData->m_parsedMeshes.Num());
						pModel->m_sourceMeshes.Resize(
							pData->m_sourceMeshNames.Num());
						for (size_t sourceMeshIndex = 0;
							sourceMeshIndex < pData->m_sourceMeshNames.Num();
							++sourceMeshIndex)
						{
							pModel->m_sourceMeshes[sourceMeshIndex].m_name =
								std::move(pData->m_sourceMeshNames[sourceMeshIndex]);
						}
						if (pData->m_bShouldKeepCpuBuffers || pData->m_bShouldGenerateBLAS)
						{
							pModel->m_cpuMeshes.Reserve(pData->m_parsedMeshes.Num());
						}

						for (size_t meshIndex = 0;
							meshIndex < pData->m_parsedMeshes.Num();
							++meshIndex)
						{
							auto& mesh = pData->m_parsedMeshes[meshIndex];
							if (!mesh.HasGeometry())
							{
								continue;
							}

							RHI::RHIMeshPtr pMesh = RHI::Renderer::GetDriver()->CreateMesh();
							pMesh->m_vertexDescription = RHI::Renderer::GetDriver()->GetOrAddVertexDescription<RHI::VertexP3N3T3B3UV2C4I4W4>();
							pMesh->m_bounds = mesh.bounds;
							pMesh->m_materialIndex = mesh.materialSlot !=
								(std::numeric_limits<uint32_t>::max)() ?
									mesh.materialSlot :
									static_cast<uint32_t>(meshIndex);
							pMesh->m_bakedVolumeScale = mesh.bakedVolumeScale;
							RHI::Renderer::GetDriver()->UpdateMesh(pMesh,
								mesh.outVertices.GetData(), sizeof(RHI::VertexP3N3T3B3UV2C4I4W4) * mesh.outVertices.Num(),
								mesh.outIndices.GetData(), sizeof(uint32_t) * mesh.outIndices.Num());

							const uint32_t renderMeshIndex =
								static_cast<uint32_t>(pModel->m_meshes.Num());
							pModel->m_meshes.Emplace(pMesh);
							if (mesh.sourceMeshIndex >= 0 &&
								static_cast<size_t>(mesh.sourceMeshIndex) <
									pModel->m_sourceMeshes.Num())
							{
								Model::SourceMesh& sourceMesh =
									pModel->m_sourceMeshes[
										static_cast<size_t>(mesh.sourceMeshIndex)];
								sourceMesh.m_renderMeshIndices.Add(
									renderMeshIndex);
								sourceMesh.m_bounds.Extend(mesh.bounds);
							}

							if (pData->m_bShouldKeepCpuBuffers || pData->m_bShouldGenerateBLAS)
							{
								Model::MeshCpuData cpuMesh{};
								cpuMesh.m_vertices = std::move(mesh.outVertices);
								cpuMesh.m_indices = std::move(mesh.outIndices);
								cpuMesh.m_bounds = mesh.bounds;
								cpuMesh.m_materialIndex = mesh.materialIndex;
								pModel->m_cpuMeshes.Add(std::move(cpuMesh));
							}
						}

						ModelImporter::PopulateModelSceneHierarchy(
							*pModel,
							pData->m_sceneNodes);

						pModel->m_inverseBind = std::move(pData->m_inverseBind);
						pModel->ProceedCpuMeshes(pData->m_bShouldGenerateBLAS, pData->m_bShouldKeepCpuBuffers);
						pModel->Flush();
					}
					return pModel;
				}, "Update RHI Meshes", EThreadType::RHI)->ToTaskWithResult();

		outModel = loadedModel = pModel;
		promise->Run();

		m_loadedModels.Unlock(uid);
		m_promises.Unlock(uid);

		return promise;
	}

	outModel = nullptr;
	m_loadedModels.Unlock(uid);
	m_promises.Unlock(uid);

	return Tasks::TaskPtr<ModelPtr>();
}

bool ModelImporter::LoadModel_Immediate(FileId uid, ModelPtr& outModel)
{
	SAILOR_PROFILE_FUNCTION();

	auto task = LoadModel(uid, outModel);
	if (!task)
	{
		outModel = nullptr;
		return false;
	}

	task->Wait();
	return task->GetResult().IsValid();
}

static glm::vec3 CalculateNormal(const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2)
{
	return Math::SafeNormalize(glm::cross(v1 - v0, v2 - v0));
}

static void GenerateTangents(ModelImporter::MeshContext& meshContext,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount)
{
	TVector<glm::vec3> tangents(vertexCount, glm::vec3(0.0f));
	TVector<glm::vec3> bitangents(vertexCount, glm::vec3(0.0f));

	auto processTriangle = [&](uint32_t idx0, uint32_t idx1, uint32_t idx2)
		{
			glm::vec3 verts[3] = {
					meshContext.outVertices[idx0].m_position,
					meshContext.outVertices[idx1].m_position,
					meshContext.outVertices[idx2].m_position };

			glm::vec2 uvs[3] = {
					meshContext.outVertices[idx0].m_texcoord,
					meshContext.outVertices[idx1].m_texcoord,
					meshContext.outVertices[idx2].m_texcoord };

				glm::vec3 t(0.0f);
				glm::vec3 b(0.0f);
				Raytracing::GenerateTangentBitangent(t, b, verts, uvs);

			tangents[idx0 - vertexOffset] += t;
			tangents[idx1 - vertexOffset] += t;
			tangents[idx2 - vertexOffset] += t;

			bitangents[idx0 - vertexOffset] += b;
			bitangents[idx1 - vertexOffset] += b;
			bitangents[idx2 - vertexOffset] += b;
		};

	if (indexCount > 0 && meshContext.outIndices.Num() > 0)
	{
		for (uint32_t i = 0; i + 2 < indexCount; i += 3)
		{
			uint32_t idx0 = meshContext.outIndices[indexOffset + i];
			uint32_t idx1 = meshContext.outIndices[indexOffset + i + 1];
			uint32_t idx2 = meshContext.outIndices[indexOffset + i + 2];
			processTriangle(idx0, idx1, idx2);
		}
	}
	else
	{
		for (uint32_t i = 0; i + 2 < vertexCount; i += 3)
		{
			uint32_t idx0 = vertexOffset + i;
			uint32_t idx1 = vertexOffset + i + 1;
			uint32_t idx2 = vertexOffset + i + 2;
			processTriangle(idx0, idx1, idx2);
		}
	}

	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		meshContext.outVertices[vertexOffset + i].m_tangent =
			Math::SafeNormalize(tangents[i]);
		meshContext.outVertices[vertexOffset + i].m_bitangent =
			Math::SafeNormalize(bitangents[i]);
	}
}

static void GenerateNormals(ModelImporter::MeshContext& meshContext,
	uint32_t vertexOffset,
	uint32_t vertexCount,
	uint32_t indexOffset,
	uint32_t indexCount)
{
	TVector<glm::vec3> normals(vertexCount, glm::vec3(0.0f));

	if (indexCount > 0 && meshContext.outIndices.Num() > 0)
	{
		for (uint32_t i = 0; i + 2 < indexCount; i += 3)
		{
			uint32_t idx0 = meshContext.outIndices[indexOffset + i];
			uint32_t idx1 = meshContext.outIndices[indexOffset + i + 1];
			uint32_t idx2 = meshContext.outIndices[indexOffset + i + 2];

			glm::vec3 normal = CalculateNormal(meshContext.outVertices[idx0].m_position, meshContext.outVertices[idx1].m_position, meshContext.outVertices[idx2].m_position);

			normals[idx0 - vertexOffset] += normal;
			normals[idx1 - vertexOffset] += normal;
			normals[idx2 - vertexOffset] += normal;
		}
	}
	else
	{
		for (uint32_t i = 0; i + 2 < vertexCount; i += 3)
		{
			uint32_t idx0 = vertexOffset + i;
			uint32_t idx1 = vertexOffset + i + 1;
			uint32_t idx2 = vertexOffset + i + 2;

			glm::vec3 normal = CalculateNormal(meshContext.outVertices[idx0].m_position, meshContext.outVertices[idx1].m_position, meshContext.outVertices[idx2].m_position);

			normals[i] += normal;
			normals[i + 1] += normal;
			normals[i + 2] += normal;
		}
	}

	for (uint32_t i = 0; i < vertexCount; ++i)
	{
		meshContext.outVertices[vertexOffset + i].m_normal =
			Math::SafeNormalize(normals[i], glm::vec3(0.0f, 1.0f, 0.0f));
	}
}

bool ModelImporter::ImportModel(ModelAssetInfoPtr assetInfo, TVector<MeshContext>& outParsedMeshes, Math::AABB& outBoundsAabb, Math::Sphere& outBoundsSphere, TVector<glm::mat4>& outInverseBind)
{
	return assetInfo &&
		ImportModel(
			assetInfo->GetAssetFilepath(),
			assetInfo->GetUnitScale(),
			assetInfo->ShouldBatchByMaterial(),
			outParsedMeshes,
			outBoundsAabb,
			outBoundsSphere,
			outInverseBind);
}

bool ModelImporter::ImportModel(
	const std::string& assetFilepath,
	float unitScale,
	bool bShouldBatchByMaterial,
	TVector<MeshContext>& outParsedMeshes,
	Math::AABB& outBoundsAabb,
	Math::Sphere& outBoundsSphere,
	TVector<glm::mat4>& outInverseBind,
	tinygltf::Model* outGltfModel)
{
	outParsedMeshes.Clear();
	outBoundsAabb = Math::AABB();
	outBoundsSphere = Math::Sphere();
	outInverseBind.Clear();
	if (outGltfModel != nullptr)
	{
		*outGltfModel = tinygltf::Model();
	}

	if (!std::isfinite(unitScale))
	{
		return false;
	}

	tinygltf::Model gltfModel;
	std::string err;
	std::string warn;
	const bool bGltfParsed = GltfImporterUtils::LoadModel(
		assetFilepath,
		true,
		gltfModel,
		err,
		warn);

	if (!err.empty())
	{
		SAILOR_LOG_ERROR("Parsing gltf %s error: %s", assetFilepath.c_str(), err.c_str());
	}

	if (!warn.empty())
	{
		SAILOR_LOG("Parsing gltf %s warning: %s", assetFilepath.c_str(), warn.c_str());
	}

	if (!bGltfParsed)
	{
		return false;
	}

	TVector<glm::mat4> parsedInverseBind;
	if (!gltfModel.skins.empty())
	{
		const auto& gltfSkin = gltfModel.skins[0];
		const size_t numBones = gltfSkin.joints.size();
		parsedInverseBind.Resize(numBones);
		for (size_t i = 0; i < numBones; ++i)
		{
			parsedInverseBind[i] = glm::mat4(1.0f);
		}

		GltfAccessorView inverseBindView;
		if (gltfSkin.inverseBindMatrices >= 0)
		{
			if (!TryGetAccessorView(
				gltfModel,
				gltfSkin.inverseBindMatrices,
				TINYGLTF_TYPE_MAT4,
				numBones,
				inverseBindView) ||
				!IsFloatAccessor(*inverseBindView.m_accessor))
			{
				SAILOR_LOG_ERROR(
					"Cannot import invalid inverse-bind accessor: %s",
					assetFilepath.c_str());
				return false;
			}

			for (size_t i = 0; i < numBones; ++i)
			{
				for (size_t component = 0; component < 16; ++component)
				{
					parsedInverseBind[i][static_cast<int32_t>(component / 4)]
						[static_cast<int32_t>(component % 4)] =
						ReadAccessorFloat(inverseBindView, i, component);
				}

				for (int32_t column = 0; column < 4; ++column)
				{
					if (!Math::AllFinite(parsedInverseBind[i][column]))
					{
						SAILOR_LOG_ERROR(
							"Cannot import non-finite inverse-bind matrix: %s",
							assetFilepath.c_str());
						return false;
					}
				}
			}
		}
	}

	if (gltfModel.materials.size() == std::numeric_limits<size_t>::max())
	{
		return false;
	}

	const bool bHasMeshQuantization =
		HasGltfExtension(gltfModel, "KHR_mesh_quantization");
	TVector<GltfImporterUtils::SceneNode> sceneNodes;
	if (!GltfImporterUtils::CollectSceneNodes(
			gltfModel,
			unitScale,
			sceneNodes))
	{
		SAILOR_LOG_ERROR(
			"Cannot resolve glTF scene hierarchy: %s",
			assetFilepath.c_str());
		return false;
	}
	for (const GltfImporterUtils::SceneNode& node : sceneNodes)
	{
		if (node.m_skinIndex > 0)
		{
			SAILOR_LOG_ERROR(
				"Cannot import unsupported active glTF skin index %d; only skin 0 is supported: %s",
				node.m_skinIndex,
				assetFilepath.c_str());
			return false;
		}
	}

	TVector<size_t> meshContextOffsets(gltfModel.meshes.size());
	TVector<size_t> meshContextCounts(gltfModel.meshes.size());
	for (size_t meshIndex = 0;
		meshIndex < gltfModel.meshes.size();
		++meshIndex)
	{
		const tinygltf::Mesh& mesh = gltfModel.meshes[meshIndex];
		meshContextOffsets[meshIndex] = outParsedMeshes.Num();
		for (size_t primitiveIndex = 0;
			primitiveIndex < mesh.primitives.size();
			++primitiveIndex)
		{
			const int32_t materialIndex =
				mesh.primitives[primitiveIndex].material >= 0 &&
				static_cast<size_t>(mesh.primitives[primitiveIndex].material) <
					gltfModel.materials.size() ?
					mesh.primitives[primitiveIndex].material : -1;
			bool bHasContext = false;
			if (bShouldBatchByMaterial)
			{
				for (size_t contextIndex = meshContextOffsets[meshIndex];
					contextIndex < outParsedMeshes.Num();
					++contextIndex)
				{
					if (outParsedMeshes[contextIndex].materialIndex ==
						materialIndex)
					{
						bHasContext = true;
						break;
					}
				}
			}

			if (!bHasContext)
			{
				if (outParsedMeshes.Num() >
					static_cast<size_t>(
						std::numeric_limits<uint32_t>::max()))
				{
					return false;
				}

				MeshContext context{};
				context.materialIndex = materialIndex;
				context.materialSlot = bShouldBatchByMaterial ?
					(materialIndex >= 0 ?
						static_cast<uint32_t>(materialIndex) : 0u) :
					static_cast<uint32_t>(outParsedMeshes.Num());
				context.sourceMeshIndex = static_cast<int32_t>(meshIndex);
				outParsedMeshes.Add(std::move(context));
			}
		}

		meshContextCounts[meshIndex] =
			outParsedMeshes.Num() - meshContextOffsets[meshIndex];
	}

	for (size_t meshIndex = 0;
		meshIndex < gltfModel.meshes.size();
		++meshIndex)
	{
		const tinygltf::Mesh& mesh = gltfModel.meshes[meshIndex];
		const glm::mat4 geometryTransform =
			glm::scale(glm::mat4(1.0f), glm::vec3(unitScale));
		const glm::mat3 directionTransform = glm::mat3(
			glm::scale(
				glm::mat4(1.0f),
				glm::vec3(unitScale < 0.0f ? -1.0f : 1.0f)));
		const float transformDeterminant =
			glm::determinant(directionTransform);
		if (!std::isfinite(transformDeterminant))
		{
			SAILOR_LOG_ERROR(
				"Cannot import glTF mesh instance with invalid transform: %s",
				assetFilepath.c_str());
			return false;
		}

		const bool bReverseWinding = transformDeterminant < 0.0f;
		glm::mat3 normalTransform = directionTransform;
		if (transformDeterminant != 0.0f)
		{
			const glm::mat3 inverseTranspose =
				glm::transpose(glm::inverse(directionTransform));
			if (IsFiniteGltfMatrix(inverseTranspose))
			{
				normalTransform = inverseTranspose;
			}
		}

		for (size_t primitiveIndex = 0;
			primitiveIndex < mesh.primitives.size();
			++primitiveIndex)
		{
			const tinygltf::Primitive& primitive =
				mesh.primitives[primitiveIndex];
			const int32_t materialIndex = primitive.material >= 0 &&
				static_cast<size_t>(primitive.material) < gltfModel.materials.size() ?
				primitive.material : -1;
			if (primitive.mode != TINYGLTF_MODE_TRIANGLES)
			{
				SAILOR_LOG(
					"Skipping non-triangle glTF primitive in %s.",
					assetFilepath.c_str());
				continue;
			}

			const auto positionIt = primitive.attributes.find("POSITION");
			GltfAccessorView positionView;
			if (positionIt == primitive.attributes.end() ||
				!TryGetAccessorView(
					gltfModel,
					positionIt->second,
					TINYGLTF_TYPE_VEC3,
					1,
					positionView) ||
				positionView.m_accessor->count >
					std::numeric_limits<uint32_t>::max() ||
				!IsPositionAccessorSupported(
					*positionView.m_accessor,
					bHasMeshQuantization))
			{
				SAILOR_LOG_ERROR(
					"Skipping glTF primitive with unsupported POSITION accessor: %s",
					assetFilepath.c_str());
				continue;
			}

			const size_t vertexCount = positionView.m_accessor->count;
			auto tryGetAttribute = [&](const char* semantic,
				int32_t expectedType,
				GltfAccessorView& outView)
				{
					const auto it = primitive.attributes.find(semantic);
					return it != primitive.attributes.end() &&
						TryGetAccessorView(
							gltfModel,
							it->second,
							expectedType,
							vertexCount,
							outView) &&
						outView.m_accessor->count == vertexCount;
				};

			GltfAccessorView normalView;
			GltfAccessorView texcoordView;
			GltfAccessorView tangentView;
			GltfAccessorView colorView;
			GltfAccessorView jointsView;
			GltfAccessorView weightsView;

			const bool bNormalsPresent =
				primitive.attributes.find("NORMAL") !=
				primitive.attributes.end();
			const bool bTexcoordsPresent =
				primitive.attributes.find("TEXCOORD_0") !=
				primitive.attributes.end();
			const bool bTangentsPresent =
				primitive.attributes.find("TANGENT") !=
				primitive.attributes.end();
			const bool bJointsPresent =
				primitive.attributes.find("JOINTS_0") !=
				primitive.attributes.end();
			const bool bWeightsPresent =
				primitive.attributes.find("WEIGHTS_0") !=
				primitive.attributes.end();

			const bool bHasNormals = bNormalsPresent &&
				tryGetAttribute("NORMAL", TINYGLTF_TYPE_VEC3, normalView);
			const bool bHasTexcoords = bTexcoordsPresent &&
				tryGetAttribute("TEXCOORD_0", TINYGLTF_TYPE_VEC2, texcoordView);
			const bool bHasTangents = bTangentsPresent &&
				tryGetAttribute("TANGENT", TINYGLTF_TYPE_VEC4, tangentView);
			const bool bHasJoints = bJointsPresent &&
				tryGetAttribute("JOINTS_0", TINYGLTF_TYPE_VEC4, jointsView);
			const bool bHasWeights = bWeightsPresent &&
				tryGetAttribute("WEIGHTS_0", TINYGLTF_TYPE_VEC4, weightsView);

			const auto colorIt = primitive.attributes.find("COLOR_0");
			const bool bColorPresent = colorIt != primitive.attributes.end();
			const bool bHasColor = bColorPresent &&
				TryGetAccessorView(
					gltfModel,
					colorIt->second,
					-1,
					vertexCount,
					colorView) &&
				colorView.m_accessor->count == vertexCount &&
				(colorView.m_accessor->type == TINYGLTF_TYPE_VEC3 ||
					colorView.m_accessor->type == TINYGLTF_TYPE_VEC4);

			if ((bNormalsPresent &&
				 (!bHasNormals ||
				  !IsDirectionAccessorSupported(
					  *normalView.m_accessor,
					  bHasMeshQuantization))) ||
				(bTexcoordsPresent &&
				 (!bHasTexcoords ||
				  !IsTexcoordAccessorSupported(
					  *texcoordView.m_accessor,
					  bHasMeshQuantization))) ||
				(bTangentsPresent &&
				 (!bHasTangents ||
				  !IsDirectionAccessorSupported(
					  *tangentView.m_accessor,
					  bHasMeshQuantization))) ||
				(bColorPresent &&
				 (!bHasColor ||
				  !IsColorAccessorSupported(*colorView.m_accessor))) ||
				bJointsPresent != bWeightsPresent ||
				(bJointsPresent &&
				 (!bHasJoints || !bHasWeights ||
				  !IsJointsAccessorSupported(*jointsView.m_accessor) ||
				  !IsWeightsAccessorSupported(*weightsView.m_accessor))))
			{
				SAILOR_LOG_ERROR(
					"Skipping glTF primitive with invalid vertex attributes: %s",
					assetFilepath.c_str());
				continue;
			}

			TVector<uint32_t> localIndices;
			if (primitive.indices >= 0)
			{
				GltfAccessorView indexView;
				if (!TryGetAccessorView(
						gltfModel,
						primitive.indices,
						TINYGLTF_TYPE_SCALAR,
						1,
						indexView) ||
					indexView.m_accessor->count >
						std::numeric_limits<uint32_t>::max() ||
					indexView.m_accessor->normalized)
				{
					SAILOR_LOG_ERROR(
						"Skipping glTF primitive with unsupported index accessor: %s",
						assetFilepath.c_str());
					continue;
				}

				localIndices.Reserve(indexView.m_accessor->count);
				bool bIndicesValid = true;
				for (size_t i = 0; i < indexView.m_accessor->count; ++i)
				{
					uint32_t index = 0;
					if (!TryReadAccessorIndex(indexView, i, index) ||
						index >= vertexCount)
					{
						bIndicesValid = false;
						break;
					}

					localIndices.Add(index);
				}

				if (!bIndicesValid)
				{
					SAILOR_LOG_ERROR(
						"Skipping glTF primitive with out-of-range indices: %s",
						assetFilepath.c_str());
					continue;
				}
			}
			else
			{
				localIndices.Reserve(vertexCount);
				for (uint32_t i = 0; i < vertexCount; ++i)
				{
					localIndices.Add(i);
				}
			}

			if (localIndices.Num() == 0 || localIndices.Num() % 3 != 0)
			{
				SAILOR_LOG_ERROR(
					"Skipping glTF primitive with invalid triangle indices: %s",
					assetFilepath.c_str());
				continue;
			}
			if (bReverseWinding)
			{
				for (size_t i = 0; i < localIndices.Num(); i += 3)
				{
					std::swap(localIndices[i + 1], localIndices[i + 2]);
				}
			}

			TVector<RHI::VertexP3N3T3B3UV2C4I4W4> localVertices;
			localVertices.Reserve(vertexCount);
			bool bVerticesValid = true;
			for (size_t i = 0; i < vertexCount; ++i)
			{
				RHI::VertexP3N3T3B3UV2C4I4W4 vertex{};
				const glm::vec3 sourcePosition(
					ReadAccessorFloat(positionView, i, 0),
					ReadAccessorFloat(positionView, i, 1),
					ReadAccessorFloat(positionView, i, 2));
				vertex.m_position = glm::vec3(
					geometryTransform * glm::vec4(sourcePosition, 1.0f));
				const glm::vec3 sourceNormal = bHasNormals ?
					glm::vec3(
						ReadAccessorFloat(normalView, i, 0),
						ReadAccessorFloat(normalView, i, 1),
						ReadAccessorFloat(normalView, i, 2)) :
					glm::vec3(0.0f);
				vertex.m_normal = normalTransform * sourceNormal;
				vertex.m_texcoord = bHasTexcoords ?
					glm::vec2(
						ReadAccessorFloat(texcoordView, i, 0),
						ReadAccessorFloat(texcoordView, i, 1)) :
					glm::vec2(0.0f);
				const glm::vec3 sourceTangent = bHasTangents ?
					glm::vec3(
						ReadAccessorFloat(tangentView, i, 0),
						ReadAccessorFloat(tangentView, i, 1),
						ReadAccessorFloat(tangentView, i, 2)) :
					glm::vec3(0.0f);
				vertex.m_tangent = directionTransform * sourceTangent;
				vertex.m_bitangent = bHasTangents && bHasNormals ?
					directionTransform *
						(glm::cross(sourceNormal, sourceTangent) *
							ReadAccessorFloat(tangentView, i, 3)) :
					glm::vec3(0.0f);
				vertex.m_color = glm::vec4(1.0f);
				if (bHasColor)
				{
					vertex.m_color = glm::vec4(
						ReadAccessorFloat(colorView, i, 0),
						ReadAccessorFloat(colorView, i, 1),
						ReadAccessorFloat(colorView, i, 2),
						colorView.m_accessor->type == TINYGLTF_TYPE_VEC4 ?
							ReadAccessorFloat(colorView, i, 3) :
							1.0f);
				}

				vertex.m_boneIds = glm::ivec4(0);
				if (bHasJoints)
				{
					for (size_t component = 0; component < 4; ++component)
					{
						vertex.m_boneIds[static_cast<int32_t>(component)] =
							static_cast<int32_t>(
							ReadAccessorFloat(jointsView, i, component));
					}
				}

				vertex.m_boneWeights = glm::vec4(0.0f);
				if (bHasWeights)
				{
					for (size_t component = 0; component < 4; ++component)
					{
						vertex.m_boneWeights[static_cast<int32_t>(component)] =
							ReadAccessorFloat(weightsView, i, component);
					}
				}

				if (!Math::AllFinite(vertex.m_position) ||
					!Math::AllFinite(vertex.m_normal) ||
					!Math::AllFinite(vertex.m_texcoord) ||
					!Math::AllFinite(vertex.m_tangent) ||
					!Math::AllFinite(vertex.m_bitangent) ||
					!Math::AllFinite(vertex.m_color) ||
					!Math::AllFinite(vertex.m_boneWeights))
				{
					bVerticesValid = false;
					break;
				}

				localVertices.Add(vertex);
			}

			if (!bVerticesValid)
			{
				SAILOR_LOG_ERROR(
					"Skipping glTF primitive with non-finite vertices: %s",
					assetFilepath.c_str());
				continue;
			}

			MeshContext* pMeshContext = nullptr;
			if (bShouldBatchByMaterial)
			{
				const size_t contextEnd = meshContextOffsets[meshIndex] +
					meshContextCounts[meshIndex];
				for (size_t contextIndex = meshContextOffsets[meshIndex];
					contextIndex < contextEnd;
					++contextIndex)
				{
					if (outParsedMeshes[contextIndex].materialIndex ==
						materialIndex)
					{
						pMeshContext = &outParsedMeshes[contextIndex];
						break;
					}
				}
			}
			else
			{
				const size_t contextIndex =
					meshContextOffsets[meshIndex] + primitiveIndex;
				if (contextIndex < outParsedMeshes.Num())
				{
					pMeshContext = &outParsedMeshes[contextIndex];
				}
			}

			if (pMeshContext == nullptr)
			{
				SAILOR_LOG_ERROR(
					"Cannot resolve glTF source mesh context: %s",
					assetFilepath.c_str());
				return false;
			}

			const size_t existingVertices = pMeshContext != nullptr ?
				pMeshContext->outVertices.Num() : 0;
			const size_t existingIndices = pMeshContext != nullptr ?
				pMeshContext->outIndices.Num() : 0;
			const size_t maxMeshElements =
				std::numeric_limits<uint32_t>::max();
			if (vertexCount > maxMeshElements - existingVertices ||
				localIndices.Num() > maxMeshElements - existingIndices)
			{
				SAILOR_LOG_ERROR(
					"Skipping oversized glTF primitive: %s",
					assetFilepath.c_str());
				continue;
			}

			const uint32_t startIndex =
				static_cast<uint32_t>(pMeshContext->outVertices.Num());
			const uint32_t indicesStart =
				static_cast<uint32_t>(pMeshContext->outIndices.Num());
			for (const auto& vertex : localVertices)
			{
				pMeshContext->outVertices.Add(vertex);
				pMeshContext->bounds.Extend(vertex.m_position);
			}

			for (uint32_t index : localIndices)
			{
				pMeshContext->outIndices.Add(startIndex + index);
			}

			const uint32_t indexCount =
				static_cast<uint32_t>(localIndices.Num());
			if (!bHasNormals)
			{
				GenerateNormals(
					*pMeshContext,
					startIndex,
					static_cast<uint32_t>(vertexCount),
					indicesStart,
					indexCount);
			}

			if (!bHasTangents || !bHasNormals)
			{
				GenerateTangents(
					*pMeshContext,
					startIndex,
					static_cast<uint32_t>(vertexCount),
					indicesStart,
					indexCount);
			}

			for (size_t i = 0; i < vertexCount; ++i)
			{
				SanitizeVertexFrame(
					pMeshContext->outVertices[startIndex + i]);
			}
		}
	}

	TVector<Math::AABB> sourceMeshBounds(gltfModel.meshes.size());
	for (const MeshContext& meshContext : outParsedMeshes)
	{
		if (meshContext.HasGeometry() &&
			meshContext.sourceMeshIndex >= 0 &&
			static_cast<size_t>(meshContext.sourceMeshIndex) <
				sourceMeshBounds.Num())
		{
			sourceMeshBounds[
				static_cast<size_t>(meshContext.sourceMeshIndex)]
				.Extend(meshContext.bounds);
		}
	}

	for (const GltfImporterUtils::SceneNode& node : sceneNodes)
	{
		if (node.m_meshIndex < 0 ||
			static_cast<size_t>(node.m_meshIndex) >=
				sourceMeshBounds.Num())
		{
			continue;
		}

		Math::AABB instanceBounds =
			sourceMeshBounds[static_cast<size_t>(node.m_meshIndex)];
		if (!instanceBounds.IsValid())
		{
			continue;
		}
		if (node.m_skinIndex < 0)
		{
			instanceBounds.Apply(node.m_worldMatrix);
		}
		outBoundsAabb.Extend(instanceBounds);
	}

	bool bImported =
		outParsedMeshes.Num() > 0 && outBoundsAabb.IsValid();
	if (bImported)
	{
		outBoundsSphere.m_center =
			0.5f * outBoundsAabb.m_min + 0.5f * outBoundsAabb.m_max;
		outBoundsSphere.m_radius =
			glm::distance(outBoundsAabb.m_max, outBoundsSphere.m_center);
		bImported = Math::AllFinite(outBoundsSphere.m_center) &&
			std::isfinite(outBoundsSphere.m_radius);
	}

	if (!bImported)
	{
		outParsedMeshes.Clear();
		outBoundsAabb = Math::AABB();
		outBoundsSphere = Math::Sphere();
		outInverseBind.Clear();
	}

	if (outGltfModel != nullptr && bImported)
	{
		*outGltfModel = std::move(gltfModel);
	}

	if (bImported)
	{
		outInverseBind = std::move(parsedInverseBind);
	}

	return bImported;
}

Tasks::TaskPtr<bool> ModelImporter::LoadDefaultMaterials(FileId uid, TVector<MaterialPtr>& outMaterials)
{
	outMaterials.Clear();

	if (ModelAssetInfoPtr modelInfo = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<ModelAssetInfoPtr>(uid))
	{
		Tasks::TaskPtr<bool> loadingFinished = Tasks::CreateTaskWithResult<bool>("Load Default Materials", []() { return true; });
		const TVector<FileId>& defaultMaterials = modelInfo->GetDefaultMaterials();
		outMaterials.Resize(defaultMaterials.Num());

		for (size_t materialIndex = 0; materialIndex < defaultMaterials.Num(); ++materialIndex)
		{
			MaterialPtr material;
			Tasks::ITaskPtr loadMaterial;
			const FileId& materialFileId = defaultMaterials[materialIndex];
			if (materialFileId && (loadMaterial = App::GetSubmodule<MaterialImporter>()->LoadMaterial(materialFileId, material)))
			{
				if (material)
				{
					// Preserve the glTF material slot even if an adjacent generated
					// material is temporarily unavailable. RHIMesh::m_materialIndex
					// refers to this original slot and must never address a compacted
					// list.
					outMaterials[materialIndex] = material;
					//TODO: Add hot reloading dependency
					loadingFinished->Join(loadMaterial);
				}
			}
		}

		App::GetSubmodule<Tasks::Scheduler>()->Run(loadingFinished);
		return loadingFinished;
	}

	return Tasks::TaskPtr<bool>::Make(false);
}

bool ModelImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	ModelPtr outModel;
	if (bImmediate)
	{
		bool bRes = LoadModel_Immediate(uid, outModel);
		out = outModel;
		return bRes;
	}

	LoadModel(uid, outModel);
	out = outModel;
	return true;
}

void ModelImporter::CollectGarbage()
{
	TVector<FileId> uidsToRemove;

	m_promises.LockAll();
	auto ids = m_promises.GetKeys();
	m_promises.UnlockAll();

	for (const auto& id : ids)
	{
		auto promise = m_promises.At_Lock(id);

		if (!promise.IsValid() || (promise.IsValid() && promise->IsFinished()))
		{
			FileId uid = id;
			uidsToRemove.Emplace(uid);
		}

		m_promises.Unlock(id);
	}

	for (auto& uid : uidsToRemove)
	{
		m_promises.Remove(uid);
	}
}
