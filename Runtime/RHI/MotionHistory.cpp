#include "MotionHistory.h"
#include "RHI/SceneView.h"
#include "RHI/Renderer.h"
#include "RHI/Shader.h"
#include "RHI/ViewSubmissionResources.h"

#include <cmath>

using namespace Sailor;
using namespace Sailor::RHI;

namespace
{
	class MotionSubmissionResources final : public RHIFrameGraphSubmissionResource
	{
	public:
		void ResetForSubmission() override { m_bIsUploaded = false; }
		size_t m_boneCapacity = 0;
		bool m_bIsUploaded = false;
	};
}

void Sailor::RHI::UploadMotionData(RHICommandListPtr commandList,
	const RHISceneViewSnapshot& snapshot, const glm::ivec2& extent)
{
	auto bindings = snapshot.m_frameBindings;
	auto resources = snapshot.m_submissionContext->GetOrAddFrameGraphResources<MotionSubmissionResources>(
		bindings.GetRawPtr(), snapshot.m_cameraIndex, 0u);
	if (resources->m_bIsUploaded) return;

	auto& driver = Renderer::GetDriver();
	auto commands = Renderer::GetDriverCommands();
	if (!bindings->HasBinding("previousFrameData"_h))
	{
		driver->AddBufferToShaderBindings(bindings, "previousFrameData"_h,
			sizeof(UboFrameData), 1u, EShaderBindingType::UniformBuffer);
	}
	const auto previous = snapshot.m_previousMotionFrame;
	const auto frame = previous ? previous->m_frameData : snapshot.GetFrameData(extent);
	commands->UpdateShaderBinding(commandList,
		bindings->GetOrAddShaderBinding("previousFrameData"_h), &frame, sizeof(frame));

	const auto bones = previous ? previous->m_bones : snapshot.m_cpuBoneMatrices;
	const size_t count = bones ? bones->Num() : 0u;
	auto& capacity = resources->m_boneCapacity;
	if (capacity < (std::max)(size_t{ 1u }, count))
	{
		capacity = GrowSubmissionCapacity(capacity, count);
		driver->AddSsboToShaderBindings(bindings, "previousBones"_h, sizeof(glm::mat4), capacity, 2u, true);
	}
	const glm::mat4 identity(1.0f);
	commands->UpdateShaderBinding(commandList, bindings->GetOrAddShaderBinding("previousBones"_h),
		count ? bones->GetData() : &identity, (std::max)(size_t{ 1u }, count) * sizeof(glm::mat4));
	commands->MemoryBarrier(commandList, static_cast<EAccessFlags>(EAccessBit::TransferWrite_Bit),
		static_cast<EAccessFlags>(EAccessBit::UniformRead_Bit) | static_cast<EAccessFlags>(EAccessBit::ShaderRead_Bit));
	resources->m_bIsUploaded = true;
}

RHIMotionHistoryFrame Sailor::RHI::CaptureMotionHistory(
	const RHISceneViewSnapshot& snapshot, const glm::ivec2& extent)
{
	RHIMotionHistoryFrame result;
	result.m_world = snapshot.m_world;
	result.m_cameraOwner = snapshot.m_camera->GetOwner();
	result.m_cameraRevision = snapshot.m_camera->GetMotionHistoryRevision();
	result.m_renderMode = snapshot.m_renderMode;
	result.m_sceneVersions = snapshot.m_sceneVersions;
	result.m_bones = snapshot.m_cpuBoneMatrices;
	for (const auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
		result.m_mobilityRevisions[static_cast<size_t>(mobility)] = snapshot.GetMobilityRevision(mobility);
	result.m_frameData = snapshot.GetFrameData(extent);
	return result;
}

RHIObjectMotionData Sailor::RHI::MakeObjectMotionData(
	const glm::mat4& current, const glm::mat4& previous,
	uint32_t previousSkeletonOffset, bool valid)
{
	RHIObjectMotionData result;
	valid = valid && glm::distance(glm::vec3(current[3]), glm::vec3(previous[3])) < 100.0f;
	result.m_previousModel = valid ? previous : current;
	result.m_state = glm::uvec4(previousSkeletonOffset, valid ? 1u : 0u, 0u, 0u);
	return result;
}

bool Sailor::RHI::IsMotionHistoryContinuous(
	const RHIMotionHistoryFrame& previous, const RHIMotionHistoryFrame& current)
{
	const auto& a = previous.m_frameData;
	const auto& b = current.m_frameData;
	const float elapsed = b.m_currentTime - a.m_currentTime;
	if (previous.m_world != current.m_world ||
		previous.m_cameraOwner != current.m_cameraOwner ||
		previous.m_cameraRevision != current.m_cameraRevision ||
		previous.m_renderMode != current.m_renderMode ||
		a.m_viewportSize != b.m_viewportSize ||
		a.m_cameraZNearZFar != b.m_cameraZNearZFar ||
		!std::isfinite(elapsed) || elapsed <= 0.0f || elapsed > 0.25f)
	{
		return false;
	}

	// Explicit cuts are authoritative. Also reject large editor teleports and
	// discontinuous rotations, so missing a cut notification is bounded safely.
	const glm::vec3 oldForward = glm::vec3(glm::inverse(a.m_view)[2]);
	const glm::vec3 newForward = glm::vec3(glm::inverse(b.m_view)[2]);
	return glm::distance(glm::vec3(a.m_cameraPosition), glm::vec3(b.m_cameraPosition)) < 50.0f &&
		glm::dot(oldForward, newForward) > 0.5f;
}

bool Sailor::RHI::ResolvePreviousMotionProxy(
	const RHISceneViewSnapshot& snapshot, const RHIVisibleSceneProxy& current,
	RHIVisibleSceneProxy& previous)
{
	previous = current;
	if (!snapshot.m_previousMotionFrame || !snapshot.m_sceneVersions ||
		!snapshot.m_previousMotionFrame->m_sceneVersions)
	{
		return false;
	}
	for (const auto& currentVersion : *snapshot.m_sceneVersions)
	{
		const RHISceneInstanceRecord* currentRecord = nullptr;
		if (!currentVersion || !currentVersion->Resolve(current.m_handle, currentRecord) ||
			currentRecord != current.m_record)
		{
			continue;
		}
		for (const auto& previousVersion : *snapshot.m_previousMotionFrame->m_sceneVersions)
		{
			const RHISceneInstanceRecord* record = nullptr;
			if (!previousVersion || previousVersion->m_sceneIdentity != currentVersion->m_sceneIdentity ||
				!previousVersion->Resolve(current.m_handle, record) || !record ||
				record->m_producerKey != currentRecord->m_producerKey ||
				record->m_topology != currentRecord->m_topology)
			{
				continue;
			}
			previous = current;
			previous.m_record = record;
			return true;
		}
		break;
	}
	return false;
}
