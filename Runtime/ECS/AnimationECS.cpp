	#include "ECS/AnimationECS.h"
#include "Engine/GameObject.h"
#include "RHI/Renderer.h"
#include <cmath>
#include <limits>
	
	using namespace Sailor;
	using namespace Sailor::Tasks;
	
void AnimationECS::BeginPlay()
{
auto& driver = RHI::Renderer::GetDriver();
m_bonesBinding = driver->CreateShaderBindings();
m_bonesBuffer = driver->AddSsboToShaderBindings(m_bonesBinding, "bones", sizeof(glm::mat4), BonesMaxNum, 0);
}

void AnimationECS::EndPlay()
{
m_bonesBinding.Clear();
m_bonesBuffer.Clear();
m_nextBoneOffset = 0;
}

Tasks::ITaskPtr AnimationECS::Tick(float deltaTime)
{
auto renderer = App::GetSubmodule<RHI::Renderer>();
auto commands = renderer->GetDriverCommands();
auto cmdList = GetWorld()->GetCommandList();
       commands->BeginDebugRegion(cmdList, "AnimationECS:Update", RHI::DebugContext::Color_CmdTransfer);

       for (auto& data : m_components)
       {
               if (!data.GetAnimation())
               {
                       continue;
               }

               const auto& anim = *data.GetAnimation();
               data.SetBonesCount(anim.m_numBones);

               if (data.m_bIsPlaying)
               {
                       float frameAdvance = deltaTime * anim.m_fps * data.m_playSpeed * (data.m_bForward ? 1.0f : -1.0f);
                       data.m_currentFrame += frameAdvance;

                       if (data.m_playMode == EAnimationPlayMode::Repeat)
                       {
                               data.m_currentFrame = std::fmod(std::max(data.m_currentFrame, 0.0f), (float)anim.m_numFrames);
                       }
                       else if (data.m_playMode == EAnimationPlayMode::Once)
                       {
                               if (data.m_currentFrame >= anim.m_numFrames - 1)
                               {
                                       data.m_currentFrame = (float)(anim.m_numFrames - 1);
                                       data.m_bIsPlaying = false;
                               }
                       }
                       else if (data.m_playMode == EAnimationPlayMode::PingPong)
                       {
                               if (data.m_bForward && data.m_currentFrame >= anim.m_numFrames - 1)
                               {
                                       data.m_currentFrame = (float)(anim.m_numFrames - 1);
                                       data.m_bForward = false;
                               }
                               else if (!data.m_bForward && data.m_currentFrame <= 0.0f)
                               {
                                       data.m_currentFrame = 0.0f;
                                       data.m_bForward = true;
                               }
                       }
               }

               data.m_frameIndex = (uint32_t)floor(data.m_currentFrame) % anim.m_numFrames;
               data.m_lerp = data.m_currentFrame - floor(data.m_currentFrame);

               if (data.m_currentSkeleton.Num() != anim.m_numBones)
               {
                       data.m_currentSkeleton.Resize(anim.m_numBones);
               }

if (data.m_gpuOffset == std::numeric_limits<uint32_t>::max())
{
data.m_gpuOffset = m_nextBoneOffset;
m_nextBoneOffset += anim.m_numBones;
m_nextBoneOffset = (std::min)(m_nextBoneOffset, BonesMaxNum);
}

               uint32_t nextFrame = (data.m_frameIndex + 1) % anim.m_numFrames;
               for (uint32_t i = 0; i < anim.m_numBones; i++)
               {
                       const glm::mat4& a = anim.m_frames[data.m_frameIndex * anim.m_numBones + i];
                       const glm::mat4& b = anim.m_frames[nextFrame * anim.m_numBones + i];
                       data.m_currentSkeleton[i] = glm::mix(a, b, data.m_lerp);
               }

auto binding = m_bonesBinding->GetOrAddShaderBinding("bones");
commands->UpdateShaderBinding(cmdList, binding,
data.m_currentSkeleton.GetData(),
sizeof(glm::mat4) * data.m_currentSkeleton.Num(),
binding->GetBufferOffset() + sizeof(glm::mat4) * data.m_gpuOffset);
       }

       commands->EndDebugRegion(cmdList);
       return nullptr;
}

void AnimationECS::FillAnimationData(RHI::RHISceneViewPtr& sceneView)
{
sceneView->m_boneMatrices = m_bonesBinding;
}
