#include "AssetRegistry/Model/GltfImporterUtils.h"
#include "AssetRegistry/Model/GltfModelImporterInternal.h"

#include "Core/Utils.h"
#include "Memory/UniquePtr.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

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
using namespace Sailor::GltfImporterInternal;

namespace Sailor::GltfImporterInternal
{
	constexpr uint64_t MaxMeshoptPlaceholderBytes = 256ull * 1024ull * 1024ull;
	static bool LoadAsciiWithMeshoptPlaceholders(tinygltf::TinyGLTF& loader,
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

		const std::string source{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
		if (source.find("meshopt_compression") == std::string::npos)
		{
			return false;
		}

		nlohmann::json document = nlohmann::json::parse(source, nullptr, false);
		if (document.is_discarded())
		{
			error = "Cannot parse glTF JSON for meshopt fallback.";
			return false;
		}

		if (!document.contains("buffers") || !document["buffers"].is_array())
		{
			return false;
		}

		TVector<size_t> placeholderBuffers;
		auto& buffers = document["buffers"];
		for (size_t i = 0; i < buffers.size(); ++i)
		{
			auto& buffer = buffers[i];
			if (!buffer.is_object() || buffer.contains("uri") || !buffer.contains("byteLength") ||
				!buffer["byteLength"].is_number_integer() || !buffer.contains("extensions") ||
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

			if (meshoptExtension == nullptr || !meshoptExtension->is_object() ||
				!meshoptExtension->contains("fallback") || !(*meshoptExtension)["fallback"].is_boolean() ||
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
				const int64_t signedByteLength = buffer["byteLength"].get<int64_t>();
				if (signedByteLength > 0)
				{
					byteLength = static_cast<uint64_t>(signedByteLength);
				}
			}

			if (byteLength == 0 || byteLength > MaxMeshoptPlaceholderBytes ||
				byteLength > std::numeric_limits<unsigned int>::max())
			{
				error = "Invalid meshopt fallback buffer size.";
				outHandled = true;
				return false;
			}

			TVector<uint8_t> placeholder;
			placeholder.Resize(static_cast<size_t>(byteLength));
			buffer["uri"] = "data:application/octet-stream;base64," + tinygltf::base64_encode(placeholder.GetData(),
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

		const bool loaded = loader.LoadASCIIFromString(&model,
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

#if defined(SAILOR_HAS_DRACO)
	static bool IsDracoAttributeLayoutValid(const draco::PointAttribute& attribute)
	{
		const int32_t dataTypeLength = draco::DataTypeLength(attribute.data_type());
		const draco::DataBuffer* buffer = attribute.buffer();
		const int64_t byteOffset = attribute.byte_offset();
		const int64_t byteStride = attribute.byte_stride();
		if (dataTypeLength <= 0 || buffer == nullptr || attribute.num_components() == 0 || attribute.size() == 0 ||
			byteOffset < 0 || byteStride < 0)
		{
			return false;
		}

		const uint64_t elementSize =
			static_cast<uint64_t>(dataTypeLength) * static_cast<uint64_t>(attribute.num_components());
		const uint64_t offset = static_cast<uint64_t>(byteOffset);
		const uint64_t stride = static_cast<uint64_t>(byteStride);
		const uint64_t lastIndex = static_cast<uint64_t>(attribute.size() - 1);
		if (stride < elementSize || lastIndex > (std::numeric_limits<uint64_t>::max() - offset) / stride)
		{
			return false;
		}

		const uint64_t lastElementOffset = offset + lastIndex * stride;
		if (elementSize > std::numeric_limits<uint64_t>::max() - lastElementOffset ||
			lastElementOffset + elementSize > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
			lastElementOffset + elementSize > static_cast<uint64_t>(buffer->data_size()))
		{
			return false;
		}

		return true;
	}

	template <typename T> bool DecodeDracoIndices(const draco::Mesh& mesh, size_t pointCount, TVector<uint8_t>& outData)
	{
		const size_t indexCount = static_cast<size_t>(mesh.num_faces()) * 3;
		outData.Resize(indexCount * sizeof(T));
		size_t outputIndex = 0;
		for (draco::FaceIndex faceIndex(0); faceIndex < mesh.num_faces(); ++faceIndex)
		{
			const draco::Mesh::Face& face = mesh.face(faceIndex);
			for (size_t corner = 0; corner < 3; ++corner)
			{
				const uint64_t index = static_cast<uint64_t>(face[corner].value());
				if (index >= pointCount || index > std::numeric_limits<T>::max())
				{
					return false;
				}

				const T value = static_cast<T>(index);
				std::memcpy(outData.GetData() + outputIndex * sizeof(T), &value, sizeof(value));
				++outputIndex;
			}
		}

		return true;
	}

	template <typename T>
	bool DecodeDracoAttribute(const draco::Mesh& mesh,
		const draco::PointAttribute& attribute,
		size_t componentCount,
		TVector<uint8_t>& outData)
	{
		const size_t pointCount = static_cast<size_t>(mesh.num_points());
		if (!attribute.is_mapping_identity() && attribute.indices_map_size() < pointCount)
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
				attribute.mapped_index(draco::PointIndex(static_cast<draco::PointIndex::ValueType>(point)));
			const uint64_t rawValueIndex = static_cast<uint64_t>(valueIndex.value());
			if (rawValueIndex >= attribute.size() ||
				!attribute.ConvertValue<T>(valueIndex, static_cast<int8_t>(componentCount), values.data()))
			{
				return false;
			}

			std::memcpy(outData.GetData() + point * elementSize, values.data(), elementSize);
		}

		return true;
	}

	static bool DecodeDracoAttribute(int32_t componentType,
		const draco::Mesh& mesh,
		const draco::PointAttribute& attribute,
		size_t componentCount,
		TVector<uint8_t>& outData)
	{
		switch (componentType)
		{
		case TINYGLTF_COMPONENT_TYPE_BYTE:
			return DecodeDracoAttribute<int8_t>(mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
			return DecodeDracoAttribute<uint8_t>(mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_SHORT:
			return DecodeDracoAttribute<int16_t>(mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
			return DecodeDracoAttribute<uint16_t>(mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
			return DecodeDracoAttribute<uint32_t>(mesh, attribute, componentCount, outData);
		case TINYGLTF_COMPONENT_TYPE_FLOAT:
			return DecodeDracoAttribute<float>(mesh, attribute, componentCount, outData);
		default:
			return false;
		}
	}
#endif

	static bool DecodeDracoPrimitives(tinygltf::Model& model, std::string& outError)
	{
		outError.clear();
#if defined(SAILOR_HAS_DRACO)
		TMap<size_t, TUniquePtr<draco::Mesh>> decodedMeshes;
#endif
		for (tinygltf::Mesh& gltfMesh : model.meshes)
		{
			for (tinygltf::Primitive& primitive : gltfMesh.primitives)
			{
				auto extensionIt = primitive.extensions.find("KHR_draco_mesh_compression");
				if (extensionIt == primitive.extensions.end())
				{
					continue;
				}

#if defined(SAILOR_HAS_DRACO)
				const tinygltf::Value& extension = extensionIt->second;
				if (primitive.mode != TINYGLTF_MODE_TRIANGLES || primitive.indices < 0 || !extension.IsObject() ||
					!extension.Has("bufferView") || !extension.Get("bufferView").IsInt() ||
					extension.Get("bufferView").Get<int>() < 0 || !extension.Has("attributes") ||
					!extension.Get("attributes").IsObject())
				{
					outError = "Invalid Draco primitive metadata.";
					return false;
				}

				const size_t compressedViewIndex = static_cast<size_t>(extension.Get("bufferView").Get<int>());
				if (compressedViewIndex >= model.bufferViews.size())
				{
					outError = "Draco buffer view is out of range.";
					return false;
				}

				const tinygltf::BufferView& compressedView = model.bufferViews[compressedViewIndex];
				if (compressedView.buffer < 0 || static_cast<size_t>(compressedView.buffer) >= model.buffers.size())
				{
					outError = "Draco source buffer is out of range.";
					return false;
				}

				const tinygltf::Buffer& compressedBuffer = model.buffers[compressedView.buffer];
				if (compressedView.byteLength == 0 ||
					compressedView.byteLength > static_cast<size_t>(std::numeric_limits<int64_t>::max()) ||
					compressedView.byteOffset > compressedBuffer.data.size() ||
					compressedView.byteLength > compressedBuffer.data.size() - compressedView.byteOffset)
				{
					outError = "Draco source data is out of range.";
					return false;
				}

				TUniquePtr<draco::Mesh>* decodedMeshPtr = nullptr;
				if (!decodedMeshes.Find(compressedViewIndex, decodedMeshPtr))
				{
					draco::DecoderBuffer decoderBuffer;
					decoderBuffer.Init(
						reinterpret_cast<const char*>(compressedBuffer.data.data() + compressedView.byteOffset),
						compressedView.byteLength);
					draco::Decoder decoder;
					auto decodedMesh = TUniquePtr<draco::Mesh>::Make();
					const draco::Status decodeStatus =
						decoder.DecodeBufferToGeometry(&decoderBuffer, decodedMesh.GetRawPtr());
					if (!decodeStatus.ok())
					{
						outError = "Cannot decode Draco mesh: " + decodeStatus.error_msg_string();
						return false;
					}

					if (!decodedMesh)
					{
						outError = "Cannot decode Draco mesh.";
						return false;
					}

					decodedMeshes[compressedViewIndex] = std::move(decodedMesh);
					if (!decodedMeshes.Find(compressedViewIndex, decodedMeshPtr))
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
				const draco::Mesh& decodedMesh = *decodedMeshPtr->GetRawPtr();
				const size_t pointCount = static_cast<size_t>(decodedMesh.num_points());
				const size_t faceCount = static_cast<size_t>(decodedMesh.num_faces());
				if (pointCount == 0 || faceCount == 0 || pointCount > std::numeric_limits<uint32_t>::max() ||
					faceCount > std::numeric_limits<size_t>::max() / 3)
				{
					outError = "Invalid decoded Draco mesh size.";
					return false;
				}

				if (primitive.indices >= 0)
				{
					if (static_cast<size_t>(primitive.indices) >= model.accessors.size())
					{
						outError = "Draco index accessor is out of range.";
						return false;
					}

					tinygltf::Accessor& accessor = model.accessors[primitive.indices];
					if (accessor.type != TINYGLTF_TYPE_SCALAR || accessor.normalized || accessor.sparse.isSparse ||
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
						bIndicesDecoded = DecodeDracoIndices<uint8_t>(decodedMesh, pointCount, decodedIndices);
						break;
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
						bIndicesDecoded = DecodeDracoIndices<uint16_t>(decodedMesh, pointCount, decodedIndices);
						break;
					case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
						bIndicesDecoded = DecodeDracoIndices<uint32_t>(decodedMesh, pointCount, decodedIndices);
						break;
					default:
						break;
					}

					if (!bIndicesDecoded || decodedIndices.IsEmpty() ||
						decodedIndices.Num() > std::numeric_limits<int>::max())
					{
						outError = "Cannot decode Draco indices.";
						return false;
					}

					tinygltf::Buffer decodedBuffer;
					decodedBuffer.data.assign(
						decodedIndices.GetData(), decodedIndices.GetData() + decodedIndices.Num());
					model.buffers.emplace_back(std::move(decodedBuffer));
					tinygltf::BufferView decodedView;
					decodedView.buffer = static_cast<int>(model.buffers.size() - 1);
					decodedView.byteLength = model.buffers.back().data.size();
					decodedView.target = TINYGLTF_TARGET_ELEMENT_ARRAY_BUFFER;
					model.bufferViews.emplace_back(std::move(decodedView));
					accessor.bufferView = static_cast<int>(model.bufferViews.size() - 1);
					accessor.byteOffset = 0;
					accessor.count = faceCount * 3;
				}

				const tinygltf::Value::Object attributes = extension.Get("attributes").Get<tinygltf::Value::Object>();
				for (const auto& [semantic, attributeValue] : attributes)
				{
					if (!attributeValue.IsInt() || attributeValue.Get<int>() < 0)
					{
						outError = "Invalid Draco attribute identifier.";
						return false;
					}

					const auto primitiveAttribute = primitive.attributes.find(semantic);
					if (primitiveAttribute == primitive.attributes.end() || primitiveAttribute->second < 0 ||
						static_cast<size_t>(primitiveAttribute->second) >= model.accessors.size())
					{
						outError = "Draco attribute accessor is out of range.";
						return false;
					}

					tinygltf::Accessor& accessor = model.accessors[primitiveAttribute->second];
					const int32_t componentSize =
						tinygltf::GetComponentSizeInBytes(static_cast<uint32_t>(accessor.componentType));
					const int32_t componentCount =
						tinygltf::GetNumComponentsInType(static_cast<uint32_t>(accessor.type));
					if (componentSize <= 0 || componentCount <= 0 || componentCount > 4 || accessor.sparse.isSparse ||
						accessor.count != pointCount ||
						pointCount > std::numeric_limits<size_t>::max() /
										 (static_cast<size_t>(componentSize) * static_cast<size_t>(componentCount)))
					{
						outError = "Invalid Draco attribute accessor.";
						return false;
					}

					const draco::PointAttribute* attribute =
						decodedMesh.GetAttributeByUniqueId(static_cast<uint32_t>(attributeValue.Get<int>()));
					if (attribute == nullptr || attribute->num_components() != componentCount)
					{
						outError = "Draco attribute is missing or incompatible.";
						return false;
					}

					TVector<uint8_t> decodedAttribute;
					if (!DecodeDracoAttribute(accessor.componentType,
							decodedMesh,
							*attribute,
							static_cast<size_t>(componentCount),
							decodedAttribute) ||
						decodedAttribute.IsEmpty() || decodedAttribute.Num() > std::numeric_limits<int>::max())
					{
						outError = "Cannot decode Draco attribute.";
						return false;
					}

					tinygltf::Buffer decodedBuffer;
					decodedBuffer.data.assign(
						decodedAttribute.GetData(), decodedAttribute.GetData() + decodedAttribute.Num());
					model.buffers.emplace_back(std::move(decodedBuffer));
					tinygltf::BufferView decodedView;
					decodedView.buffer = static_cast<int>(model.buffers.size() - 1);
					decodedView.byteLength = model.buffers.back().data.size();
					decodedView.byteStride = 0;
					decodedView.target = TINYGLTF_TARGET_ARRAY_BUFFER;
					model.bufferViews.emplace_back(std::move(decodedView));
					accessor.bufferView = static_cast<int>(model.bufferViews.size() - 1);
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

	bool HasGltfExtension(const tinygltf::Model& model, const char* extension)
	{
		return std::find(model.extensionsUsed.begin(), model.extensionsUsed.end(), extension) !=
				   model.extensionsUsed.end() ||
			   std::find(model.extensionsRequired.begin(), model.extensionsRequired.end(), extension) !=
				   model.extensionsRequired.end();
	}

	static bool HasBufferViewFallback(const tinygltf::Model& model, const tinygltf::BufferView& bufferView)
	{
		if (bufferView.buffer < 0 || static_cast<size_t>(bufferView.buffer) >= model.buffers.size())
		{
			return false;
		}

		const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
		return bufferView.byteOffset <= buffer.data.size() &&
			   bufferView.byteLength <= buffer.data.size() - bufferView.byteOffset;
	}

	static bool TryGetMeshoptSize(const tinygltf::Value& extension,
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

	template <typename T> int32_t SignExtendMeshoptColor(T value)
	{
		constexpr uint32_t numBits = sizeof(T) * 8;
		constexpr uint32_t signBit = 1u << (numBits - 1);
		constexpr uint32_t range = 1u << numBits;
		const uint32_t raw = static_cast<uint32_t>(value);
		return (raw & signBit) != 0 ? static_cast<int32_t>(raw - range) : static_cast<int32_t>(raw);
	}

	template <typename T> bool DecodeMeshoptColorFilter(T* data, size_t count)
	{
		const double maxValue = static_cast<double>(std::numeric_limits<T>::max());
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
			const int32_t co = SignExtendMeshoptColor(data[offset + 1]);
			const int32_t cg = SignExtendMeshoptColor(data[offset + 2]);
			const int32_t red = y + co - cg;
			const int32_t green = y + cg;
			const int32_t blue = y - co - cg;
			const int32_t encodedAlpha = static_cast<int32_t>(data[offset + 3]);
			const int32_t alpha = ((encodedAlpha << 1) & alphaScale) | (encodedAlpha & 1);
			const double scale = maxValue / alphaScale;
			auto decodeComponent = [&](int32_t component)
			{
				const double decoded = static_cast<double>(component) * scale + 0.5;
				return static_cast<T>(std::clamp(decoded, 0.0, maxValue));
			};

			data[offset] = decodeComponent(red);
			data[offset + 1] = decodeComponent(green);
			data[offset + 2] = decodeComponent(blue);
			data[offset + 3] = decodeComponent(alpha);
		}

		return true;
	}

	static bool DecodeMeshoptBufferViews(tinygltf::Model& model, std::string& outError)
	{
		outError.clear();
		for (tinygltf::BufferView& bufferView : model.bufferViews)
		{
			auto extensionIt = bufferView.extensions.find("EXT_meshopt_compression");
			if (extensionIt == bufferView.extensions.end())
			{
				extensionIt = bufferView.extensions.find("KHR_meshopt_compression");
			}

			if (extensionIt == bufferView.extensions.end())
			{
				continue;
			}

			const std::string extensionName = extensionIt->first;
			const tinygltf::Value extension = extensionIt->second;
			const bool bHasFallback = HasBufferViewFallback(model, bufferView);
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
			if (!TryGetMeshoptSize(extension, "buffer", sourceBufferIndex, true) ||
				!TryGetMeshoptSize(extension, "byteOffset", sourceOffset, false) ||
				!TryGetMeshoptSize(extension, "byteLength", sourceLength, true) ||
				!TryGetMeshoptSize(extension, "byteStride", stride, true) ||
				!TryGetMeshoptSize(extension, "count", count, true) || sourceLength == 0 || stride == 0 || count == 0 ||
				count > std::numeric_limits<size_t>::max() / stride || count * stride != bufferView.byteLength ||
				sourceBufferIndex >= model.buffers.size())
			{
				if (useFallbackOrFail("Invalid meshopt buffer-view metadata."))
				{
					continue;
				}
				return false;
			}

			const tinygltf::Buffer& sourceBuffer = model.buffers[sourceBufferIndex];
			if (sourceOffset > sourceBuffer.data.size() || sourceLength > sourceBuffer.data.size() - sourceOffset)
			{
				if (useFallbackOrFail("Meshopt source data is out of range."))
				{
					continue;
				}
				return false;
			}

			if (!extension.Has("mode") || !extension.Get("mode").IsString())
			{
				if (useFallbackOrFail("Meshopt compression mode is missing."))
				{
					continue;
				}
				return false;
			}

			const std::string& mode = extension.Get("mode").Get<std::string>();
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

			const bool bAttributesLayoutValid = mode == "ATTRIBUTES" && stride >= 4 && stride <= 256 && stride % 4 == 0;
			const bool bTrianglesLayoutValid = mode == "TRIANGLES" && count % 3 == 0 && (stride == 2 || stride == 4);
			const bool bIndicesLayoutValid = mode == "INDICES" && (stride == 2 || stride == 4);
			const bool bFilterLayoutValid =
				(filter == "NONE" && (bAttributesLayoutValid || bTrianglesLayoutValid || bIndicesLayoutValid)) ||
				(mode == "ATTRIBUTES" &&
					((filter == "OCTAHEDRAL" && (stride == 4 || stride == 8)) ||
						(filter == "QUATERNION" && stride == 8) || (filter == "EXPONENTIAL" && stride % 4 == 0) ||
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
			const uint8_t* source = sourceBuffer.data.data() + sourceOffset;
			int decodeResult = -1;
			if (mode == "ATTRIBUTES")
			{
				decodeResult = meshopt_decodeVertexBuffer(decoded.GetData(), count, stride, source, sourceLength);
			}
			else if (mode == "TRIANGLES")
			{
				decodeResult = meshopt_decodeIndexBuffer(decoded.GetData(), count, stride, source, sourceLength);
			}
			else if (mode == "INDICES")
			{
				decodeResult = meshopt_decodeIndexSequence(decoded.GetData(), count, stride, source, sourceLength);
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
				const bool bColorDecoded =
					stride == 4 ? DecodeMeshoptColorFilter(reinterpret_cast<uint8_t*>(decoded.GetData()), count)
								: DecodeMeshoptColorFilter(reinterpret_cast<uint16_t*>(decoded.GetData()), count);
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

			if (bufferView.buffer < 0 || static_cast<size_t>(bufferView.buffer) >= model.buffers.size() ||
				bufferView.byteOffset > std::numeric_limits<size_t>::max() - bufferView.byteLength)
			{
				if (useFallbackOrFail("Invalid meshopt destination buffer."))
				{
					continue;
				}
				return false;
			}

			tinygltf::Buffer& destinationBuffer = model.buffers[bufferView.buffer];
			const size_t destinationEnd = bufferView.byteOffset + bufferView.byteLength;
			if (destinationBuffer.data.size() < destinationEnd)
			{
				destinationBuffer.data.resize(destinationEnd);
			}
			std::memcpy(destinationBuffer.data.data() + bufferView.byteOffset, decoded.GetData(), decoded.Num());
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

}

bool GltfImporterUtils::LoadModel(const std::string& assetFilepath,
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

	const bool bIsGlb = Utils::GetFileExtension(assetFilepath.c_str()) == "glb";
	const bool bLoadedNormally =
		bIsGlb ? loader.LoadBinaryFromFile(&outModel, &outError, &outWarning, assetFilepath.c_str())
			   : loader.LoadASCIIFromFile(&outModel, &outError, &outWarning, assetFilepath.c_str());

	bool bParsed = bLoadedNormally;
	if (!bParsed && !bIsGlb)
	{
		tinygltf::Model patchedModel;
		std::string patchedError;
		std::string patchedWarning;
		bool bHandled = false;
		bParsed = LoadAsciiWithMeshoptPlaceholders(
			loader, patchedModel, patchedError, patchedWarning, assetFilepath, bHandled);
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
