#include "AnimationImporter.h"
#include "AnimationClipSampler.h"
#include "AssetRegistry/AssetRegistry.h"
#include "AssetRegistry/Model/GltfImporterUtils.h"
#include <tiny_gltf.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include "Math/Transform.h"
#include "Math/Math.h"

using namespace Sailor;

namespace
{
	struct GltfFloatAccessorView
	{
		const tinygltf::Accessor* m_accessor = nullptr;
		const uint8_t* m_data = nullptr;
		size_t m_stride = 0;
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

		return count == 1 ||
			(stride >= elementSize &&
			 count - 1 <= (bufferSize - offset - elementSize) / stride);
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
		const GltfFloatAccessorView& view,
		size_t sparseElement,
		uint32_t& outIndex)
	{
		if (sparseElement >= view.m_sparseCount)
		{
			return false;
		}

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

	bool TryGetFloatAccessorView(
		const tinygltf::Model& model,
		int32_t accessorIndex,
		int32_t expectedType,
		size_t requiredCount,
		GltfFloatAccessorView& outView)
	{
		outView = {};
		if (accessorIndex < 0 ||
			static_cast<size_t>(accessorIndex) >= model.accessors.size())
		{
			return false;
		}

		const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
		if (accessor.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT ||
			accessor.normalized ||
			accessor.type != expectedType ||
			accessor.count == 0 ||
			accessor.count < requiredCount ||
			(!accessor.sparse.isSparse && accessor.bufferView < 0) ||
			(accessor.bufferView < 0 && accessor.byteOffset != 0))
		{
			return false;
		}

		const int32_t componentCount = tinygltf::GetNumComponentsInType(
			static_cast<uint32_t>(accessor.type));
		if (componentCount <= 0 ||
			static_cast<size_t>(componentCount) >
				std::numeric_limits<size_t>::max() / sizeof(float))
		{
			return false;
		}

		const size_t elementSize =
			static_cast<size_t>(componentCount) * sizeof(float);
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
			stride = bufferView.byteStride > 0 ?
				static_cast<size_t>(bufferView.byteStride) : elementSize;
			if (stride < elementSize || stride % sizeof(float) != 0 ||
				!TryGetBufferViewData(
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

		const int32_t sparseIndexComponentType =
			accessor.sparse.indices.componentType;
		const int32_t sparseIndexSize = tinygltf::GetComponentSizeInBytes(
			static_cast<uint32_t>(sparseIndexComponentType));
		if (sparseIndexSize <= 0 ||
			(sparseIndexComponentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE &&
			 sparseIndexComponentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT &&
			 sparseIndexComponentType != TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) ||
			accessor.sparse.indices.bufferView < 0 ||
			accessor.sparse.values.bufferView < 0 ||
			static_cast<size_t>(accessor.sparse.indices.bufferView) >=
				model.bufferViews.size() ||
			static_cast<size_t>(accessor.sparse.values.bufferView) >=
				model.bufferViews.size())
		{
			return false;
		}

		const size_t sparseCount =
			static_cast<size_t>(accessor.sparse.count);
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
		outView.m_sparseIndexComponentType = sparseIndexComponentType;

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
		const GltfFloatAccessorView& view,
		size_t elementIndex)
	{
		if (view.m_accessor == nullptr ||
			elementIndex >= view.m_accessor->count)
		{
			return nullptr;
		}

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

	bool TryReadAccessorFloat(
		const GltfFloatAccessorView& view,
		size_t elementIndex,
		size_t componentIndex,
		float& outValue)
	{
		if (view.m_accessor == nullptr ||
			elementIndex >= view.m_accessor->count ||
			componentIndex >= view.m_componentCount)
		{
			return false;
		}

		const uint8_t* data = GetAccessorElementData(view, elementIndex);
		outValue = data != nullptr ?
			ReadUnaligned<float>(data + componentIndex * sizeof(float)) : 0.0f;
		return std::isfinite(outValue);
	}

	enum class EAnimationTarget : uint8_t
	{
		Translation,
		Rotation,
		Scale
	};

	struct PreparedAnimationChannel
	{
		TVector<float> m_timestamps;
		TVector<glm::vec4> m_values;
		size_t m_targetNode = 0;
		EAnimationTarget m_target = EAnimationTarget::Translation;
		EAnimationInterpolation m_interpolation = EAnimationInterpolation::Linear;
	};

	bool IsTransformFinite(const Math::Transform& transform)
	{
		return Math::AllFinite(transform.m_position) &&
			Math::AllFinite(transform.m_scale) &&
			std::isfinite(transform.m_rotation.x) &&
			std::isfinite(transform.m_rotation.y) &&
			std::isfinite(transform.m_rotation.z) &&
			std::isfinite(transform.m_rotation.w);
	}
}

AnimationImporter::AnimationImporter(AnimationAssetInfoHandler* infoHandler)
{
	SAILOR_PROFILE_FUNCTION();
	m_allocator = ObjectAllocatorPtr::Make(EAllocationPolicy::SharedMemory_MultiThreaded);
	infoHandler->Subscribe(this);
}

AnimationImporter::~AnimationImporter()
{
	for (auto& anim : m_loadedAnimations)
	{
		anim.m_second.DestroyObject(m_allocator);
	}
}

bool AnimationImporter::LoadAsset(FileId uid, TObjectPtr<Object>& out, bool bImmediate)
{
	AnimationPtr outAnim;
	if (bImmediate)
	{
		bool res = LoadAnimation_Immediate(uid, outAnim);
		out = outAnim;
		return res;
	}

	LoadAnimation(uid, outAnim);
	out = outAnim;
	return true;
}

Tasks::TaskPtr<AnimationPtr> AnimationImporter::LoadAnimation(FileId uid, AnimationPtr& outAnimation)
{
	SAILOR_PROFILE_FUNCTION();

	auto& promise = m_promises.At_Lock(uid, nullptr);
	auto& loadedAnimation = m_loadedAnimations.At_Lock(uid, AnimationPtr());

	if (loadedAnimation)
	{
		outAnimation = loadedAnimation;
		auto res = promise ? promise : Tasks::TaskPtr<AnimationPtr>::Make(outAnimation);

		m_loadedAnimations.Unlock(uid);
		m_promises.Unlock(uid);

		return res;
	}

	if (!promise)
	{
		AnimationPtr anim = AnimationPtr::Make(m_allocator, uid);

		promise = Tasks::CreateTaskWithResult<AnimationPtr>("Load Animation",
			[this, uid, anim]() mutable
			{
				ImportAnimation(uid, anim);
				return anim;
			}, EThreadType::Worker);

		outAnimation = loadedAnimation = anim;

		promise->Run();
	}
	else
	{
		outAnimation = loadedAnimation;
	}

	m_loadedAnimations.Unlock(uid);
	m_promises.Unlock(uid);

	return promise;
}

bool AnimationImporter::LoadAnimation_Immediate(FileId uid, AnimationPtr& outAnimation)
{
	auto task = LoadAnimation(uid, outAnimation);
	task->Wait();
	return task->GetResult().IsValid();
}

bool AnimationImporter::ImportAnimation(FileId uid, AnimationPtr& outAnimation)
{
	AnimationPtr anim = outAnimation;
	if (!anim)
	{
		anim = AnimationPtr::Make(m_allocator, uid);
	}

	if (auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AnimationAssetInfoPtr>(uid))
	{
		tinygltf::Model gltfModel;
		std::string err, warn;

		const bool parsed = GltfImporterUtils::LoadModel(
			info->GetAssetFilepath(),
			true,
			gltfModel,
			err,
			warn);

		if (!parsed)
		{
			return false;
		}

		if (!gltfModel.animations.empty() && !gltfModel.skins.empty())
		{
			const int32_t sourceAnimationIndex = info->GetAnimationIndex();
			const int32_t sourceSkinIndex = info->GetSkinIndex();
			if (sourceAnimationIndex < 0 || sourceSkinIndex < 0 ||
				static_cast<size_t>(sourceAnimationIndex) >=
					gltfModel.animations.size() ||
				static_cast<size_t>(sourceSkinIndex) >= gltfModel.skins.size())
			{
				return false;
			}

			const size_t animIndex =
				static_cast<size_t>(sourceAnimationIndex);
			const size_t skinIndex = static_cast<size_t>(sourceSkinIndex);
			const auto& gltfAnim = gltfModel.animations[animIndex];
			const auto& gltfSkin = gltfModel.skins[skinIndex];
			const size_t numBones = gltfSkin.joints.size();
			const size_t numNodes = gltfModel.nodes.size();
			constexpr size_t MaxVectorElements =
				(static_cast<size_t>(std::numeric_limits<uint32_t>::max()) + 1) / 2;
			if (numBones == 0 ||
				numBones > MaxVectorElements ||
				numNodes > MaxVectorElements ||
				numNodes > static_cast<size_t>(std::numeric_limits<int32_t>::max()) ||
				gltfAnim.samplers.size() > MaxVectorElements ||
				gltfAnim.channels.size() > MaxVectorElements)
			{
				return false;
			}

			for (int32_t joint : gltfSkin.joints)
			{
				if (joint < 0 || static_cast<size_t>(joint) >= numNodes)
				{
					return false;
				}
			}

			TVector<int32_t> parents;
			parents.Resize(numNodes);
			for (size_t i = 0; i < numNodes; ++i)
			{
				parents[i] = -1;
			}

			for (size_t i = 0; i < numNodes; ++i)
			{
				const auto& node = gltfModel.nodes[i];
				for (int32_t child : node.children)
				{
					if (child >= 0 &&
						static_cast<size_t>(child) < numNodes &&
						static_cast<size_t>(child) != i &&
						parents[child] < 0)
					{
						parents[child] = static_cast<int32_t>(i);
					}
				}
			}

			TVector<Math::Transform> base(numNodes);
			for (size_t i = 0; i < numNodes; ++i)
			{
				const auto& n = gltfModel.nodes[i];
				if (n.translation.size() == 3)
				{
					const glm::vec4 translation(
						static_cast<float>(n.translation[0]),
						static_cast<float>(n.translation[1]),
						static_cast<float>(n.translation[2]),
						1.0f);
					if (Math::AllFinite(translation))
					{
						base[i].m_position = translation;
					}
				}

				if (n.rotation.size() == 4)
				{
					glm::quat rotation(
						static_cast<float>(n.rotation[3]),
						static_cast<float>(n.rotation[0]),
						static_cast<float>(n.rotation[1]),
						static_cast<float>(n.rotation[2]));
					const float lengthSquared = glm::dot(rotation, rotation);
					if (std::isfinite(lengthSquared) &&
						lengthSquared > std::numeric_limits<float>::epsilon())
					{
						base[i].m_rotation = glm::normalize(rotation);
					}
				}

				if (n.scale.size() == 3)
				{
					const glm::vec4 scale(
						static_cast<float>(n.scale[0]),
						static_cast<float>(n.scale[1]),
						static_cast<float>(n.scale[2]),
						1.0f);
					if (Math::AllFinite(scale))
					{
						base[i].m_scale = scale;
					}
				}
			}

			TVector<PreparedAnimationChannel> preparedChannels;
			preparedChannels.Reserve(gltfAnim.channels.size());
			float animationDuration = 0.0f;
			size_t maxSourceSampleCount = 0;
			for (const auto& channel : gltfAnim.channels)
			{
				if (channel.sampler < 0 ||
					static_cast<size_t>(channel.sampler) >= gltfAnim.samplers.size() ||
					channel.target_node < 0 ||
					static_cast<size_t>(channel.target_node) >= numNodes)
				{
					continue;
				}

				const auto& sampler = gltfAnim.samplers[channel.sampler];
				GltfFloatAccessorView inputView;
				if (!TryGetFloatAccessorView(
					gltfModel,
					sampler.input,
					TINYGLTF_TYPE_SCALAR,
					1,
					inputView) ||
					inputView.m_accessor->count > MaxVectorElements)
				{
					continue;
				}
				const size_t sampleCount = inputView.m_accessor->count;

				PreparedAnimationChannel prepared;
				prepared.m_timestamps.Resize(sampleCount);
				bool bValid = true;
				for (size_t sample = 0; sample < sampleCount; ++sample)
				{
					bValid &= TryReadAccessorFloat(
						inputView,
						sample,
						0,
						prepared.m_timestamps[sample]);
				}
				if (!bValid || !AnimationClipSampler::ValidateTimestamps(prepared.m_timestamps))
				{
					continue;
				}

				int32_t outputType = TINYGLTF_TYPE_VEC3;
				if (channel.target_path == "translation")
				{
					prepared.m_target = EAnimationTarget::Translation;
				}
				else if (channel.target_path == "rotation")
				{
					prepared.m_target = EAnimationTarget::Rotation;
					outputType = TINYGLTF_TYPE_VEC4;
				}
				else if (channel.target_path == "scale")
				{
					prepared.m_target = EAnimationTarget::Scale;
				}
				else
				{
					continue;
				}

				if (sampler.interpolation.empty() || sampler.interpolation == "LINEAR")
				{
					prepared.m_interpolation = EAnimationInterpolation::Linear;
				}
				else if (sampler.interpolation == "STEP")
				{
					prepared.m_interpolation = EAnimationInterpolation::Step;
				}
				else if (sampler.interpolation == "CUBICSPLINE")
				{
					prepared.m_interpolation = EAnimationInterpolation::CubicSpline;
				}
				else
				{
					continue;
				}

				const bool bCubicSpline =
					prepared.m_interpolation == EAnimationInterpolation::CubicSpline;
				if (bCubicSpline &&
					sampleCount > std::numeric_limits<size_t>::max() / 3)
				{
					continue;
				}

				const size_t outputCount = bCubicSpline ?
					sampleCount * 3 : sampleCount;
				GltfFloatAccessorView outputView;
				if (!TryGetFloatAccessorView(
					gltfModel,
					sampler.output,
					outputType,
					outputCount,
					outputView) ||
					outputView.m_accessor->count != outputCount)
				{
					continue;
				}

				prepared.m_values.Resize(outputCount);
				const size_t componentCount =
					prepared.m_target == EAnimationTarget::Rotation ? 4 : 3;
				bValid = true;
				for (size_t element = 0; element < outputCount; ++element)
				{
					glm::vec4 value(0.0f);
					for (size_t component = 0; component < componentCount; ++component)
					{
						bValid &= TryReadAccessorFloat(
							outputView,
							element,
							component,
							value[static_cast<glm::length_t>(component)]);
					}
					prepared.m_values[element] = value;
				}
				if (!bValid)
				{
					continue;
				}

				prepared.m_targetNode =
					static_cast<size_t>(channel.target_node);
				animationDuration = (std::max)(
					animationDuration,
					prepared.m_timestamps[prepared.m_timestamps.Num() - 1]);
				maxSourceSampleCount = (std::max)(maxSourceSampleCount, sampleCount);
				preparedChannels.Add(prepared);
			}

			if (preparedChannels.IsEmpty() || !std::isfinite(animationDuration))
			{
				return false;
			}

			constexpr float TargetSamplingFps = 30.0f;
			const double targetIntervals = std::ceil(
				static_cast<double>(animationDuration) * TargetSamplingFps);
			if (!std::isfinite(targetIntervals) ||
				targetIntervals > static_cast<double>(MaxVectorElements - 1))
			{
				return false;
			}

			const size_t numIntervals = (std::max)(
				static_cast<size_t>(targetIntervals),
				maxSourceSampleCount - 1);
			const size_t numFrames = numIntervals + 1;
			if (numFrames > MaxVectorElements / numBones)
			{
				return false;
			}

			TVector<Math::Transform> framesData;
			framesData.Resize(numFrames * numBones);
			const float samplingFps = animationDuration > 0.0f ?
				static_cast<float>(numIntervals) / animationDuration : TargetSamplingFps;

			for (size_t f = 0; f < numFrames; ++f)
			{
				const float sampleTime = (std::min)(
					static_cast<float>(f) / samplingFps,
					animationDuration);
				TVector<Math::Transform> local(base);
				for (const auto& channel : preparedChannels)
				{
					Math::Transform& target = local[channel.m_targetNode];
					if (channel.m_target == EAnimationTarget::Rotation)
					{
						AnimationClipSampler::SampleRotation(
							channel.m_timestamps,
							channel.m_values,
							channel.m_interpolation,
							sampleTime,
							target.m_rotation);
					}
					else
					{
						glm::vec4 value;
						if (AnimationClipSampler::SampleVector(
							channel.m_timestamps,
							channel.m_values,
							channel.m_interpolation,
							sampleTime,
							value))
						{
							value.w = 1.0f;
							if (channel.m_target == EAnimationTarget::Translation)
							{
								target.m_position = value;
							}
							else
							{
								target.m_scale = value;
							}
						}
					}
				}

				TVector<Math::Transform> global(numNodes);
				TVector<uint8_t> composeState(numNodes);
				auto compose = [&](auto&& self, size_t nodeIndex) -> bool
				{
					if (composeState[nodeIndex] == 2)
					{
						return IsTransformFinite(global[nodeIndex]);
					}
					if (composeState[nodeIndex] == 1)
					{
						global[nodeIndex] = local[nodeIndex];
						composeState[nodeIndex] = 2;
						return false;
					}

					composeState[nodeIndex] = 1;
					Math::Transform composed = local[nodeIndex];
					const int32_t parentIndex = parents[nodeIndex];
					if (parentIndex >= 0 &&
						self(self, static_cast<size_t>(parentIndex)))
					{
						const Math::Transform& parent =
							global[static_cast<size_t>(parentIndex)];
						composed.m_position =
							parent.TransformPosition(composed.m_position);
						composed.m_rotation =
							parent.m_rotation * composed.m_rotation;
						composed.m_scale.x *= parent.m_scale.x;
						composed.m_scale.y *= parent.m_scale.y;
						composed.m_scale.z *= parent.m_scale.z;
					}

					global[nodeIndex] = IsTransformFinite(composed) ?
						composed : local[nodeIndex];
					composeState[nodeIndex] = 2;
					return IsTransformFinite(global[nodeIndex]);
				};

				for (size_t i = 0; i < numNodes; ++i)
				{
					compose(compose, i);
				}

				for (size_t j = 0; j < numBones; ++j)
				{
					const size_t nodeIndex =
						static_cast<size_t>(gltfSkin.joints[j]);
					framesData[f * numBones + j] = global[nodeIndex];
				}
			}

			anim->m_numBones = static_cast<uint32_t>(numBones);
			anim->m_numFrames = static_cast<uint32_t>(numFrames);
			anim->m_fps = samplingFps;
			anim->m_duration = animationDuration;
			anim->m_frames = std::move(framesData);
		}
	}

	outAnimation = anim;
	return true;
}

void AnimationImporter::CollectGarbage()
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
