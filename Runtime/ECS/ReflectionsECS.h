#pragma once

#include "ECS/ECS.h"
#include "ECS/ReflectionProbeSpatial.h"
#include "ECS/ReflectionProbeRendering.h"
#include "FrameGraph/LocalReflection.h"
#include "Engine/Types.h"

#include <array>

namespace Sailor
{
	enum class EReflectionProbeUpdateMode : uint8_t
	{
		Static,
		OnLoad,
		EveryFrame,
		Interval
	};

	struct ReflectionProbeConfiguration
	{
		TexturePtr m_reflectionMap;
		EReflectionProbeUpdateMode m_updateMode = EReflectionProbeUpdateMode::OnLoad;
		float m_updateInterval = 20.0f;
		uint32_t m_resolution = 128u;
		float m_radius = 10.0f;
	};

	class ReflectionProbeData final : public ECS::TComponent
	{
	public:
		ReflectionProbeConfiguration m_configuration;
		uint32_t GetProbeIndex() const { return m_probeIndex; }
		uint64_t GetSlotGeneration() const { return m_slotGeneration; }

	private:
		uint32_t m_probeIndex = RHI::InvalidReflectionProbeIndex;
		uint64_t m_slotGeneration = 0u;
		Framegraph::LocalReflectionCaptureRequest m_lastRequest;
		RHI::RHITexturePtr m_lastSource;
		double m_nextUpdate = 0.0;
		bool m_bRegistered = false;
		bool m_bCapacityReported = false;
		bool m_bInvalidReported = false;
		friend class ReflectionsECS;
	};

	class ReflectionsECS final : public ECS::TSystem<ReflectionsECS, ReflectionProbeData>
	{
	public:
		SAILOR_API ReflectionsECS();
		SAILOR_API void Tick(float deltaTime) override;
		SAILOR_API void EndPlay() override;
		uint32_t GetOrder() const override { return 200u; }
		TSharedPtr<const ReflectionProbeSpatialSnapshot> GetSpatialSnapshot() const { return m_spatialSnapshot; }
		TSharedPtr<ReflectionProbeRenderState> GetRenderState() const { return m_renderState; }

	protected:
		SAILOR_API void OnComponentUnregistered(size_t index, ReflectionProbeData& component) override;

	private:
		uint32_t AllocateSlot(ReflectionProbeData& component);
		TSharedPtr<ReflectionProbeRenderState> m_renderState;
		TSharedPtr<const ReflectionProbeSpatialSnapshot> m_spatialSnapshot;
		std::array<bool, RHI::MaxReflectionProbes> m_occupied{};
		std::array<uint64_t, RHI::MaxReflectionProbes> m_slotGenerations{};
		uint64_t m_spatialRevision = 0u;
		double m_time = 0.0;
	};

	template class ECS::TSystem<ReflectionsECS, ReflectionProbeData>;
}
