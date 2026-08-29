#pragma once

#include "GlobalIllumination/GIProbesComposition.h"
#include "AssetRegistry/GlobalIllumination/GIProbesImporter.h"
#include "Core/SpinLock.h"
#include "ECS/ECS.h"
#include "GlobalIllumination/GISettings.h"
#include "RHI/GlobalIllumination.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <string>

namespace Sailor
{
	class GlobalIlluminationECSData final : public ECS::TComponent
	{};

	enum class EGlobalIlluminationProbeResidency : uint8_t
	{
		Unloaded = 0,
		Loading,
		Resident,
		Failed
	};

	struct SAILOR_SHARED_API GlobalIlluminationProbeState final
	{
		std::string m_name{};
		FileId m_asset{};
		EGlobalIlluminationProbeMode m_mode =
			EGlobalIlluminationProbeMode::Blend;
		float m_weight = 0.0f;
		EGlobalIlluminationProbeResidency m_residency =
			EGlobalIlluminationProbeResidency::Unloaded;
		uint64_t m_assetRevision = 0u;
		std::string m_diagnostic{};
	};

	using GlobalIlluminationSnapshotPtr =
		RHI::RHIGlobalIlluminationSnapshotPtr;

	class GlobalIlluminationECS final :
		public ECS::TSystem<GlobalIlluminationECS, GlobalIlluminationECSData>
	{
	public:
		static constexpr const char* DisplayName = "Global Illumination ECS";

		SAILOR_API void BeginPlay() override;
		SAILOR_API Tasks::ITaskPtr Tick(float deltaTime) override;
		SAILOR_API void EndPlay() override;
		SAILOR_API uint32_t GetOrder() const override { return 175u; }

		SAILOR_API uint32_t GetMaxProbeStatesPerSnapshot() const noexcept;
		SAILOR_API bool IsEnabled() const noexcept;
		SAILOR_API const GISettings& GetWorldSettings() const noexcept
		{
			return m_worldSettings;
		}
		SAILOR_API bool ApplyWorldSettings(
			const GISettings& settings,
			std::string& outDiagnostic);
		SAILOR_API bool SetProbeWeight(
			const std::string& name,
			float weight,
			std::string& outDiagnostic);
		SAILOR_API bool SetProbeWeights(
			const TMap<std::string, float>& weights,
			std::string& outDiagnostic);
		SAILOR_API bool SetProbeMode(
			const std::string& name,
			EGlobalIlluminationProbeMode mode,
			std::string& outDiagnostic);
		SAILOR_API bool PreloadProbe(
			const std::string& name,
			std::string& outDiagnostic);
		SAILOR_API bool UnloadProbe(
			const std::string& name,
			std::string& outDiagnostic);

		SAILOR_API GlobalIlluminationSnapshotPtr GetActiveSnapshot() const;
		SAILOR_API TVector<GlobalIlluminationProbeState> GetProbeStates() const;
		SAILOR_API std::string GetDiagnostic() const;
		SAILOR_API uint64_t GetCompositionCount() const noexcept
		{
			return m_compositionCount.load(std::memory_order_acquire);
		}
		SAILOR_API uint64_t GetRejectedCompositionCount() const noexcept
		{
			return m_rejectedCompositionCount.load(std::memory_order_acquire);
		}

	private:
		struct RuntimeBinding final
		{
			FileId m_assetId{};
			EGlobalIlluminationProbeMode m_mode =
				EGlobalIlluminationProbeMode::Blend;
			float m_weight = 0.0f;
			bool m_bPreload = false;
			GIProbesAssetPtr m_asset{};
			Tasks::TaskPtr<GIProbesAssetPtr> m_loadTask{};
			bool m_bRuntimeRetained = false;
			EGlobalIlluminationProbeResidency m_residency =
				EGlobalIlluminationProbeResidency::Unloaded;
			uint64_t m_observedRevision = 0u;
			std::string m_diagnostic{};
		};

		void InitializeFromWorld();
		bool StartLoad(
			const std::string& name,
			RuntimeBinding& binding,
			std::string& outDiagnostic);
		void RefreshResidency();
		void RecomposeIfNeeded();
		void DrawDebugVisualization() const;
		uint32_t CountPositiveWeights(
			const TMap<std::string, float>* overrides = nullptr) const;
		void SetDiagnostic(std::string diagnostic);
		void ClearActiveSnapshot();

		TMap<std::string, RuntimeBinding> m_bindings{};
		GISettings m_worldSettings{};
		mutable SpinLock m_snapshotLock;
		GlobalIlluminationSnapshotPtr m_activeSnapshot{};
		std::string m_diagnostic{};
		uint64_t m_generation = 0u;
		std::atomic<uint64_t> m_compositionCount{ 0u };
		std::atomic<uint64_t> m_rejectedCompositionCount{ 0u };
		uint32_t m_observedQualityBudget =
			(std::numeric_limits<uint32_t>::max)();
		bool m_bObservedEnabled = true;
		bool m_bInitialized = false;
		bool m_bCompositionDirty = true;
	};

	template class ECS::TSystem<GlobalIlluminationECS, GlobalIlluminationECSData>;
}
