#include "AnimationImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include <tiny_gltf.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
#include "Math/Transform.h"

using namespace Sailor;

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
	if (auto loaded = m_loadedAnimations.At_Lock(uid))
	{
		outAnimation = loaded;
		m_loadedAnimations.Unlock(uid);
		return Tasks::TaskPtr<AnimationPtr>::Make(outAnimation);
	}

	auto& promise = m_promises.At_Lock(uid, nullptr);

	if (promise.IsValid())
	{
		m_promises.Unlock(uid);
		return promise;
	}

	auto task = Tasks::CreateTask<AnimationPtr>("Load Animation", [this, uid]()
		{
			AnimationPtr anim;
			ImportAnimation(uid, anim);
			return anim;
		}, EThreadType::Worker);

	task->Run();
	promise = task;

	m_promises.Unlock(uid);

	return task;
}

bool AnimationImporter::LoadAnimation_Immediate(FileId uid, AnimationPtr& outAnimation)
{
	auto task = LoadAnimation(uid, outAnimation);
	task->Wait();
	return task->GetResult().IsValid();
}

bool AnimationImporter::ImportAnimation(FileId uid, AnimationPtr& outAnimation)
{
	AnimationPtr anim = AnimationPtr::Make(m_allocator, uid);

	if (auto info = App::GetSubmodule<AssetRegistry>()->GetAssetInfoPtr<AnimationAssetInfoPtr>(uid))
	{
		tinygltf::Model gltfModel;
		tinygltf::TinyGLTF loader;
		std::string err, warn;

		bool parsed = (Utils::GetFileExtension(info->GetAssetFilepath()) == "glb") ?
			loader.LoadBinaryFromFile(&gltfModel, &err, &warn, info->GetAssetFilepath().c_str()) :
			loader.LoadASCIIFromFile(&gltfModel, &err, &warn, info->GetAssetFilepath().c_str());
			
		if (!parsed)
		{
			return false;
		}

		if (!gltfModel.animations.empty() && !gltfModel.skins.empty())
		{
			uint32_t animIndex = (uint32_t)info->GetAnimationIndex();
			uint32_t skinIndex = (uint32_t)info->GetSkinIndex();

			if (animIndex >= gltfModel.animations.size() || skinIndex >= gltfModel.skins.size())
			{
				return false;
			}

			const auto& gltfAnim = gltfModel.animations[animIndex];
			const auto& gltfSkin = gltfModel.skins[skinIndex];

			size_t numBones = gltfSkin.joints.size();
			const tinygltf::Accessor& timeAccessor = gltfModel.accessors[gltfAnim.samplers[0].input];
			size_t numFrames = timeAccessor.count;

			anim->m_numBones = (uint32_t)numBones;
			anim->m_numFrames = (uint32_t)numFrames;
			anim->m_fps = 30.0f;

			TVector<Math::Transform> framesData;
			framesData.Resize(numFrames * numBones);

			TVector<int> parents(gltfModel.nodes.size(), -1);
			for (size_t i = 0; i < gltfModel.nodes.size(); ++i)
			{
				const auto& node = gltfModel.nodes[i];
				for (int child : node.children)
				{
					parents[child] = (int)i;
				}
			}


			TVector<Math::Transform> base(gltfModel.nodes.size());
			for (size_t i = 0; i < gltfModel.nodes.size(); ++i)
			{
				const auto& n = gltfModel.nodes[i];
				if (n.translation.size() == 3) base[i].m_position = glm::vec4(n.translation[0], n.translation[1], n.translation[2], 1.0f);
				if (n.rotation.size() == 4) base[i].m_rotation = glm::quat((float)n.rotation[3], (float)n.rotation[0], (float)n.rotation[1], (float)n.rotation[2]);
				if (n.scale.size() == 3) base[i].m_scale = glm::vec4(n.scale[0], n.scale[1], n.scale[2], 1.0f);
			}

			TVector<TVector<Math::Transform>> transforms(numFrames, base);

			for (const auto& channel : gltfAnim.channels)
			{
				const auto& sampler = gltfAnim.samplers[channel.sampler];
				const auto& inAcc = gltfModel.accessors[sampler.input];
				const auto& inView = gltfModel.bufferViews[inAcc.bufferView];
				//const float* inData = reinterpret_cast<const float*>(&gltfModel.buffers[inView.buffer].data[inView.byteOffset + inAcc.byteOffset]);

				const auto& outAcc = gltfModel.accessors[sampler.output];
				const auto& outView = gltfModel.bufferViews[outAcc.bufferView];
				const float* outData = reinterpret_cast<const float*>(&gltfModel.buffers[outView.buffer].data[outView.byteOffset + outAcc.byteOffset]);

				for (size_t f = 0; f < numFrames; ++f)
				{
					size_t offset = f * ((channel.target_path == "rotation") ? 4 : 3);
					auto& nodeTrs = transforms[f][channel.target_node];

					if (channel.target_path == "translation")
					{
						nodeTrs.m_position = glm::vec4(glm::make_vec3(outData + offset), 1.0f);
					}
					else if (channel.target_path == "rotation")
					{
						nodeTrs.m_rotation = glm::quat(outData[offset + 3], outData[offset], outData[offset + 1], outData[offset + 2]);
					}
					else if (channel.target_path == "scale")
					{
						nodeTrs.m_scale = glm::vec4(glm::make_vec3(outData + offset), 1.0f);
					}
				}
			}

			auto compose = [&](uint32_t nodeIndex, const TVector<Math::Transform>& local, TVector<Math::Transform>& global)
				{
					global[nodeIndex] = local[nodeIndex];
					if (parents[nodeIndex] >= 0)
					{
						const Math::Transform& parent = global[parents[nodeIndex]];
						global[nodeIndex].m_position = parent.TransformPosition(global[nodeIndex].m_position);
						global[nodeIndex].m_rotation = parent.m_rotation * global[nodeIndex].m_rotation;
						global[nodeIndex].m_scale.x *= parent.m_scale.x;
						global[nodeIndex].m_scale.y *= parent.m_scale.y;
						global[nodeIndex].m_scale.z *= parent.m_scale.z;
					}
				};

			for (size_t f = 0; f < numFrames; ++f)
			{
				TVector<Math::Transform> global(gltfModel.nodes.size());
				for (size_t i = 0; i < gltfModel.nodes.size(); ++i)
				{
					compose((uint32_t)i, transforms[f], global);
				}

				for (size_t j = 0; j < numBones; ++j)
				{
					uint32_t nodeIndex = gltfSkin.joints[j];
					framesData[f * numBones + j] = global[nodeIndex];
				}
			}

			anim->m_frames = std::move(framesData);
		}
	}

	outAnimation = anim;
	auto& newAnim = m_loadedAnimations.At_Lock(uid);
	newAnim = anim;
	m_loadedAnimations.Unlock(uid);

	return true;
}
