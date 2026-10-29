#include "ECS/ReflectionsECS.h"
#include "ECS/ReflectionProbeRendering.h"
#include "ECS/TransformECS.h"
#include "Engine/GameObject.h"
#include "AssetRegistry/Texture/TextureImporter.h"
#include "RHI/Texture.h"

#include <cmath>

using namespace Sailor;

ReflectionsECS::ReflectionsECS() :
	m_renderState(TSharedPtr<ReflectionProbeRenderState>::Make()),
	m_spatialSnapshot(TSharedPtr<ReflectionProbeSpatialSnapshot>::Make(
		0u, TVector<ReflectionProbeSpatialEntry>{}))
{
}

uint32_t ReflectionsECS::AllocateSlot(ReflectionProbeData& component)
{
	for (uint32_t slot = 0u; slot < RHI::MaxReflectionProbes; ++slot)
	{
		if (!m_occupied[slot])
		{
			m_occupied[slot] = true;
			component.m_probeIndex = slot;
			component.m_slotGeneration = ++m_slotGenerations[slot];
			component.m_bCapacityReported = false;
			return slot;
		}
	}
	if (!component.m_bCapacityReported)
	{
		SAILOR_LOG("ReflectionProbe ignored: the scene capacity of %u probes is exhausted.", RHI::MaxReflectionProbes);
		component.m_bCapacityReported = true;
	}
	return RHI::InvalidReflectionProbeIndex;
}

void ReflectionsECS::OnComponentUnregistered(size_t, ReflectionProbeData& component)
{
	if (component.m_probeIndex != RHI::InvalidReflectionProbeIndex)
	{
		m_renderState->Remove(component.m_probeIndex, component.m_slotGeneration);
		m_occupied[component.m_probeIndex] = false;
		component.m_probeIndex = RHI::InvalidReflectionProbeIndex;
	}
}

void ReflectionsECS::Tick(float deltaTime)
{
	if (std::isfinite(deltaTime) && deltaTime > 0.0f)
	{
		m_time += deltaTime;
	}
	TVector<ReflectionProbeSpatialEntry> entries;
	entries.Reserve((std::min)(m_components.Num(), static_cast<size_t>(RHI::MaxReflectionProbes)));
	for (auto& component : m_components)
	{
		if (!component.m_bIsActive || !component.m_owner)
		{
			continue;
		}
		auto* owner = static_cast<GameObject*>(component.m_owner.GetRawPtr());
		const auto& matrix = owner->GetTransformComponent().GetCachedWorldMatrix();
		const glm::vec3 center(matrix[3]);
		const auto& configuration = component.m_configuration;
		const float radius = CalculateReflectionProbeWorldRadius(configuration.m_radius, matrix);
		const bool valid = Math::AllFinite(center) && std::isfinite(radius) &&
			glm::all(glm::lessThan(glm::abs(center) + glm::vec3(radius + 4.0f), glm::vec3(1u << 28u)));
		if (!valid)
		{
			if (!component.m_bInvalidReported)
			{
				SAILOR_LOG("ReflectionProbe ignored: its world sphere is non-finite or outside the supported spatial range.");
				component.m_bInvalidReported = true;
			}
			if (component.m_bRegistered)
			{
				m_renderState->Remove(component.m_probeIndex, component.m_slotGeneration);
				component.m_bRegistered = false;
			}
			continue;
		}
		component.m_bInvalidReported = false;
		if (component.m_probeIndex == RHI::InvalidReflectionProbeIndex &&
			AllocateSlot(component) == RHI::InvalidReflectionProbeIndex)
		{
			continue;
		}
		entries.Add({ center, radius, component.m_probeIndex, owner->GetInstanceId().ToString() });

		Framegraph::LocalReflectionCaptureRequest request;
		request.m_parameters.m_positionBlend = glm::vec4(center, radius * 0.1f);
		request.m_parameters.m_minEnabled = glm::vec4(center - glm::vec3(radius), 1.0f);
		request.m_parameters.m_max = glm::vec4(center + glm::vec3(radius), 0.0f);
		request.m_faceSize = configuration.m_resolution;
		request.m_nearPlane = 0.01f;
		request.m_farPlane = (std::max)(1000.0f, radius * 2.0f);

		RHI::RHITexturePtr source;
		if (configuration.m_updateMode == EReflectionProbeUpdateMode::Static)
		{
			if (configuration.m_reflectionMap && configuration.m_reflectionMap->IsReady())
			{
				source = configuration.m_reflectionMap->GetRHI();
			}
			if (!source)
			{
				if (component.m_bRegistered)
				{
					m_renderState->Remove(component.m_probeIndex, component.m_slotGeneration);
					component.m_bRegistered = false;
				}
				continue;
			}
		}
		const bool captureConfigurationChanged = !component.m_bRegistered || component.m_bIsDirty ||
			component.m_lastSource != source ||
			component.m_lastRequest.m_faceSize != request.m_faceSize;
		const bool changed = captureConfigurationChanged ||
			component.m_lastRequest.m_parameters.m_positionBlend != request.m_parameters.m_positionBlend ||
			component.m_lastRequest.m_parameters.m_minEnabled != request.m_parameters.m_minEnabled ||
			component.m_lastRequest.m_parameters.m_max != request.m_parameters.m_max;
		if (changed)
		{
			m_renderState->Upsert(component.m_probeIndex, component.m_slotGeneration, request, source);
			component.m_bRegistered = true;
			component.m_lastRequest = request;
			component.m_lastSource = source;
		}
		const bool dynamic = configuration.m_updateMode != EReflectionProbeUpdateMode::Static;
		if (dynamic && (captureConfigurationChanged || configuration.m_updateMode == EReflectionProbeUpdateMode::EveryFrame ||
			(configuration.m_updateMode == EReflectionProbeUpdateMode::Interval && m_time >= component.m_nextUpdate)))
		{
			m_renderState->Request(component.m_probeIndex, component.m_slotGeneration);
			component.m_nextUpdate = m_time + configuration.m_updateInterval;
		}
		component.m_bIsDirty = false;
	}

	if (entries != m_spatialSnapshot->GetEntries())
	{
		m_spatialSnapshot = TSharedPtr<ReflectionProbeSpatialSnapshot>::Make(++m_spatialRevision, std::move(entries));
	}
}

void ReflectionsECS::EndPlay()
{
	m_renderState->Reset();
	m_occupied.fill(false);
	m_spatialSnapshot = TSharedPtr<ReflectionProbeSpatialSnapshot>::Make(
		++m_spatialRevision, TVector<ReflectionProbeSpatialEntry>{});
	m_time = 0.0;
	ECS::TSystem<ReflectionsECS, ReflectionProbeData>::EndPlay();
}
