#include "AnimationImporter.h"
#include "AssetRegistry/AssetRegistry.h"
#include <tiny_gltf.h>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/quaternion.hpp>
	
	using namespace Sailor;
	
	AnimationImporter::AnimationImporter(AnimationAssetInfoHandler* infoHandler)
	{
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
	outAnimation = *loaded;
	m_loadedAnimations.Unlock(uid);
	return Tasks::TaskPtr<AnimationPtr>::Make(outAnimation);
	}
	
	auto promise = m_promises.Find(uid);
	if (promise != m_promises.end())
	{
	return *(*promise).m_second;
	}
	
	auto task = Tasks::CreateTask<AnimationPtr>("Load Animation", [this, uid]()
	{
	AnimationPtr anim;
	ImportAnimation(uid, anim);
	return anim;
	}, EThreadType::Worker);
	
	m_promises.Emplace(uid, task);
	task->Run();
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
        AnimationPtr anim = TObjectPtr<Animation>::Make(uid);

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

                        TVector<glm::mat4> framesData;
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

                        TVector<glm::mat4> inverseBind(numBones);
                        if (gltfSkin.inverseBindMatrices >= 0)
                        {
                                const auto& accessor = gltfModel.accessors[gltfSkin.inverseBindMatrices];
                                const auto& view = gltfModel.bufferViews[accessor.bufferView];
                                const float* data = reinterpret_cast<const float*>(&gltfModel.buffers[view.buffer].data[view.byteOffset + accessor.byteOffset]);
                                for (size_t i = 0; i < numBones; ++i)
                                {
                                        inverseBind[i] = glm::make_mat4(data + i * 16);
                                }
                        }
                        else
                        {
                                for (size_t i = 0; i < numBones; ++i) inverseBind[i] = glm::mat4(1.0f);
                        }

                        struct TRS { glm::vec3 t{0}; glm::quat r{1,0,0,0}; glm::vec3 s{1}; };
                        TVector<TRS> base(gltfModel.nodes.size());
                        for (size_t i = 0; i < gltfModel.nodes.size(); ++i)
                        {
                                const auto& n = gltfModel.nodes[i];
                                if (n.translation.size() == 3) base[i].t = glm::vec3(n.translation[0], n.translation[1], n.translation[2]);
                                if (n.rotation.size() == 4) base[i].r = glm::quat((float)n.rotation[3], (float)n.rotation[0], (float)n.rotation[1], (float)n.rotation[2]);
                                if (n.scale.size() == 3) base[i].s = glm::vec3(n.scale[0], n.scale[1], n.scale[2]);
                        }

                        TVector<TVector<TRS>> transforms(numFrames, base);

                        for (const auto& channel : gltfAnim.channels)
                        {
                                const auto& sampler = gltfAnim.samplers[channel.sampler];
                                const auto& inAcc = gltfModel.accessors[sampler.input];
                                const auto& inView = gltfModel.bufferViews[inAcc.bufferView];
                                const float* inData = reinterpret_cast<const float*>(&gltfModel.buffers[inView.buffer].data[inView.byteOffset + inAcc.byteOffset]);

                                const auto& outAcc = gltfModel.accessors[sampler.output];
                                const auto& outView = gltfModel.bufferViews[outAcc.bufferView];
                                const float* outData = reinterpret_cast<const float*>(&gltfModel.buffers[outView.buffer].data[outView.byteOffset + outAcc.byteOffset]);

                                for (size_t f = 0; f < numFrames; ++f)
                                {
                                        size_t offset = f * ((channel.target_path == "rotation") ? 4 : 3);
                                        auto& nodeTrs = transforms[f][channel.target_node];

                                        if (channel.target_path == "translation")
                                        {
                                                nodeTrs.t = glm::make_vec3(outData + offset);
                                        }
                                        else if (channel.target_path == "rotation")
                                        {
                                                nodeTrs.r = glm::quat(outData[offset + 3], outData[offset], outData[offset + 1], outData[offset + 2]);
                                        }
                                        else if (channel.target_path == "scale")
                                        {
                                                nodeTrs.s = glm::make_vec3(outData + offset);
                                        }
                                }
                        }

                        auto compose = [&](uint32_t nodeIndex, const TVector<TRS>& local, TVector<glm::mat4>& global)
                        {
                                glm::mat4 localMat = glm::translate(glm::mat4(1.0f), local[nodeIndex].t) * glm::mat4_cast(local[nodeIndex].r) * glm::scale(glm::mat4(1.0f), local[nodeIndex].s);
                                if (parents[nodeIndex] >= 0)
                                {
                                        global[nodeIndex] = global[parents[nodeIndex]] * localMat;
                                }
                                else
                                {
                                        global[nodeIndex] = localMat;
                                }
                        };

                        for (size_t f = 0; f < numFrames; ++f)
                        {
                                TVector<glm::mat4> global(gltfModel.nodes.size());
                                for (size_t i = 0; i < gltfModel.nodes.size(); ++i)
                                {
                                        compose((uint32_t)i, transforms[f], global);
                                }

                                for (size_t j = 0; j < numBones; ++j)
                                {
                                        uint32_t nodeIndex = gltfSkin.joints[j];
                                        framesData[f * numBones + j] = global[nodeIndex] * inverseBind[j];
                                }
                        }
                }
        }

        anim->m_frames = std::move(framesData);

        outAnimation = anim;
        m_loadedAnimations.Emplace(uid, anim);
        return true;
}
