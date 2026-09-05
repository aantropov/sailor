#include "MotionHistory.h"
#include "RHI/SceneView.h"

#include <cmath>

using namespace Sailor;
using namespace Sailor::RHI;

RHIMotionHistoryFrame Sailor::RHI::CaptureMotionHistory(
	const RHISceneViewSnapshot& snapshot, WorldPtr world,
	float worldTime, const glm::ivec2& extent)
{
	RHIMotionHistoryFrame result;
	result.m_world = world;
	result.m_cameraOwner = snapshot.m_camera->GetOwner();
	result.m_cameraRevision = snapshot.m_camera->GetMotionHistoryRevision();
	result.m_renderMode = snapshot.m_renderMode;
	result.m_sceneVersions = snapshot.m_sceneVersions;
	result.m_bones = snapshot.m_cpuBoneMatrices;
	for (const auto mobility : { EMobilityType::Static, EMobilityType::Stationary, EMobilityType::Dynamic })
		result.m_mobilityRevisions[static_cast<size_t>(mobility)] = snapshot.GetMobilityRevision(mobility);
	auto& frame = result.m_frameData;
	frame.m_cameraPosition = snapshot.m_cameraTransform.m_position;
	frame.m_projection = snapshot.m_camera->GetProjectionMatrix();
	frame.m_invProjection = snapshot.m_camera->GetInvProjection();
	frame.m_cameraZNearZFar = glm::vec2(snapshot.m_camera->GetZNear(), snapshot.m_camera->GetZFar());
	frame.m_currentTime = worldTime;
	frame.m_deltaTime = snapshot.m_deltaTime;
	frame.m_view = snapshot.m_camera->GetViewMatrix();
	frame.m_viewportSize = extent;
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
	previous = {};
	if (!snapshot.m_previousMotionFrame || !snapshot.m_sceneVersions ||
		!snapshot.m_previousMotionFrame->m_sceneVersions || !current.m_record)
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
