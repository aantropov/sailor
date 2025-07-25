#include "ECS/AnimationECS.h"
#include "Engine/GameObject.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "AssetRegistry/Animation/AnimationImporter.h"
#include "Components/MeshRendererComponent.h"
#include <cmath>
#include <limits>
#include "Math/Transform.h"

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
		if (!data.GetAnimation() || data.GetAnimation()->m_numFrames ==0)
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
			const Math::Transform& a = anim.m_frames[data.m_frameIndex * anim.m_numBones + i];
			const Math::Transform& b = anim.m_frames[nextFrame * anim.m_numBones + i];
			data.m_currentSkeleton[i] = Math::Lerp(a, b, data.m_lerp);
		}

		ModelPtr model;
		if (auto owner = data.m_owner.StaticCast<GameObject>())
		{
			if (auto mesh = owner->GetComponent<MeshRendererComponent>())
			{
				model = mesh->GetModel();
			}
		}

		TVector<glm::mat4> matrices(data.m_currentSkeleton.Num());
		for (uint32_t i = 0; i < data.m_currentSkeleton.Num(); ++i)
		{
			const glm::mat4 bind = model && model->GetInverseBind().Num() > i ? model->GetInverseBind()[i] : glm::mat4(1.0f);
			matrices[i] = data.m_currentSkeleton[i].Matrix() * bind;
		}

		commands->UpdateShaderBinding(cmdList, m_bonesBuffer,
			matrices.GetData(),
			sizeof(glm::mat4) * matrices.Num(),
			m_bonesBuffer->GetBufferOffset() + sizeof(glm::mat4) * data.m_gpuOffset);
	}

	commands->EndDebugRegion(cmdList);
	return nullptr;
}

void AnimationECS::FillAnimationData(RHI::RHISceneViewPtr& sceneView)
{
	sceneView->m_boneMatrices = m_bonesBinding;
}
