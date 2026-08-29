#include "GlobalIllumination/GIProbesComposition.h"

#include <bit>
#include <cmath>

using namespace Sailor;

namespace
{
	GIProbesCompositionPlan FailPlan(
		EGIProbesCompositionStatus status,
		std::string diagnostic)
	{
		GIProbesCompositionPlan result;
		result.m_status = status;
		result.m_diagnostic = std::move(diagnostic);
		return result;
	}

	bool ValidateTrustedAssetMetadata(
		const GIProbesData& data,
		std::string& outDiagnostic) noexcept
	{
		outDiagnostic.clear();
		if (data.m_formatVersion != GIProbesFormatVersion ||
			data.m_shOrder != GIProbeSphericalHarmonicsOrder ||
			data.m_compression != EGIProbesCompression::Float32)
		{
			outDiagnostic = "probe representation metadata is unsupported";
			return false;
		}
		if (data.m_bricks.IsEmpty() || data.m_probes.IsEmpty())
		{
			outDiagnostic = "probe layout is empty";
			return false;
		}
		if (data.m_layoutHash == 0u ||
			data.m_representationHash == 0u ||
			data.m_transportHash == 0u)
		{
			outDiagnostic = "validated probe asset metadata is missing compatibility hashes";
			return false;
		}
		return true;
	}

	void HashU32(uint64_t& hash, uint32_t value) noexcept
	{
		for (uint32_t shift = 0u; shift < 32u; shift += 8u)
		{
			hash ^= static_cast<uint8_t>((value >> shift) & 0xffu);
			hash *= 1099511628211ull;
		}
	}

	void HashU64(uint64_t& hash, uint64_t value) noexcept
	{
		HashU32(hash, static_cast<uint32_t>(value));
		HashU32(hash, static_cast<uint32_t>(value >> 32u));
	}

	void HashString(uint64_t& hash, const std::string& value) noexcept
	{
		for (const char character : value)
		{
			hash ^= static_cast<uint8_t>(character);
			hash *= 1099511628211ull;
		}
	}
}

GIProbesCompositionPlan GIProbesComposer::BuildPlan(
	const TVector<GIProbesCompositionInput>& inputs,
	uint32_t maxStatesPerSnapshot,
	EGIProbesCompositionValidation validation) noexcept
{
	try
	{
		TVector<const GIProbesCompositionInput*> active;
		active.Reserve(inputs.Num());
		for (const GIProbesCompositionInput& input : inputs)
		{
			if (!std::isfinite(input.m_weight) || input.m_weight < 0.0f)
			{
				return FailPlan(
					EGIProbesCompositionStatus::InvalidInput,
					"probe state '" + input.m_name + "' has an invalid weight");
			}
			if (input.m_weight <= 0.0f)
			{
				continue;
			}
			active.Add(&input);
		}

		if (active.IsEmpty())
		{
			return FailPlan(
				EGIProbesCompositionStatus::Disabled,
				"no probe states have a positive weight");
		}
		if (maxStatesPerSnapshot == 0u || active.Num() > maxStatesPerSnapshot)
		{
			return FailPlan(
				EGIProbesCompositionStatus::BudgetExceeded,
				"the requested probe-state mixture exceeds the active quality budget");
		}

		const GIProbesCompositionInput* reference = nullptr;
		for (const GIProbesCompositionInput* input : active)
		{
			if (!input->m_data)
			{
				return FailPlan(
					EGIProbesCompositionStatus::MissingData,
					"probe state '" + input->m_name + "' is not resident");
			}
			if (!reference ||
				(reference->m_mode == EGlobalIlluminationProbeMode::Additive &&
					input->m_mode == EGlobalIlluminationProbeMode::Blend))
			{
				reference = input;
			}
		}

		std::string validationDiagnostic;
		const auto validate = [&](const GIProbesData& data)
		{
			return validation == EGIProbesCompositionValidation::Full
				? data.Validate(validationDiagnostic)
				: ValidateTrustedAssetMetadata(data, validationDiagnostic);
		};
		if (!validate(*reference->m_data))
		{
			return FailPlan(
				EGIProbesCompositionStatus::InvalidInput,
				"probe state '" + reference->m_name + "' is invalid: " +
				validationDiagnostic);
		}
		for (const GIProbesCompositionInput* input : active)
		{
			if (input != reference && !validate(*input->m_data))
			{
				return FailPlan(
					EGIProbesCompositionStatus::InvalidInput,
					"probe state '" + input->m_name + "' is invalid: " +
					validationDiagnostic);
			}
			if (input != reference &&
				!reference->m_data->IsCompositionCompatibleWith(
					*input->m_data,
					validationDiagnostic))
			{
				return FailPlan(
					EGIProbesCompositionStatus::Incompatible,
					"probe state '" + input->m_name +
					"' is incompatible with '" + reference->m_name +
					"': " + validationDiagnostic);
			}
		}

		float blendWeightSum = 0.0f;
		for (const GIProbesCompositionInput* input : active)
		{
			if (input->m_mode == EGlobalIlluminationProbeMode::Blend)
			{
				blendWeightSum += input->m_weight;
			}
		}
		if (!std::isfinite(blendWeightSum))
		{
			return FailPlan(
				EGIProbesCompositionStatus::InvalidInput,
				"the sum of Blend probe-state weights is not finite");
		}

		GIProbesCompositionPlan result;
		result.m_layout = reference->m_data;
		result.m_states.Reserve(active.Num());
		result.m_names.Reserve(active.Num());
		result.m_assets.Reserve(active.Num());
		result.m_effectiveWeights.Reserve(active.Num());
		result.m_modes.Reserve(active.Num());
		uint64_t lightingHash = 1469598103934665603ull;
		for (const GIProbesCompositionInput* input : active)
		{
			const float effectiveWeight =
				input->m_mode == EGlobalIlluminationProbeMode::Blend
					? (blendWeightSum > 0.0f
						? input->m_weight / blendWeightSum
						: 0.0f)
					: input->m_weight;
			result.m_states.Add(input->m_data);
			result.m_names.Add(input->m_name);
			result.m_assets.Add(input->m_asset);
			result.m_effectiveWeights.Add(effectiveWeight);
			result.m_modes.Add(input->m_mode);
			HashString(lightingHash, input->m_name);
			HashU64(lightingHash, input->m_data->m_lightingHash);
			HashU32(lightingHash, static_cast<uint32_t>(input->m_mode));
			HashU32(lightingHash, std::bit_cast<uint32_t>(effectiveWeight));
		}

		result.m_lightingHash = lightingHash;
		result.m_diagnostic =
			"Prepared Global Illumination ECS snapshot from " +
			std::to_string(active.Num()) + " baked state(s).";
		result.m_status = EGIProbesCompositionStatus::Success;
		return result;
	}
	catch (const std::exception& exception)
	{
		return FailPlan(
			EGIProbesCompositionStatus::InvalidInput,
			std::string("cannot prepare probe states: ") + exception.what());
	}
	catch (...)
	{
		return FailPlan(
			EGIProbesCompositionStatus::InvalidInput,
			"cannot prepare probe states: unknown failure");
	}
}

GIProbesCompositionResult GIProbesComposer::Compose(
	const TVector<GIProbesCompositionInput>& inputs,
	uint32_t maxStatesPerSnapshot) noexcept
{
	GIProbesCompositionPlan plan = BuildPlan(
		inputs,
		maxStatesPerSnapshot,
		EGIProbesCompositionValidation::Full);
	GIProbesCompositionResult result;
	result.m_status = plan.m_status;
	result.m_diagnostic = plan.m_diagnostic;
	if (!plan.IsSuccess())
	{
		return result;
	}

	try
	{
		GIProbesDataPtr composed = GIProbesDataPtr::Make();
		*composed = *plan.m_layout;
		for (GIProbe& probe : composed->m_probes)
		{
			for (glm::vec3& coefficient : probe.m_irradiance)
			{
				coefficient = glm::vec3(0.0f);
			}
		}

		for (size_t stateIndex = 0u;
			stateIndex < plan.m_states.Num();
			++stateIndex)
		{
			const GIProbesData& state = *plan.m_states[stateIndex];
			const float effectiveWeight = plan.m_effectiveWeights[stateIndex];
			for (size_t probeIndex = 0u;
				probeIndex < composed->m_probes.Num();
				++probeIndex)
			{
				for (uint32_t coefficientIndex = 0u;
					coefficientIndex < GIProbeSphericalHarmonicsCoefficientCount;
					++coefficientIndex)
				{
					composed->m_probes[probeIndex].m_irradiance[coefficientIndex] +=
						state.m_probes[probeIndex].m_irradiance[coefficientIndex] *
						effectiveWeight;
				}
			}
		}

		composed->m_stateName = "Global Illumination ECS snapshot";
		composed->m_lightingHash = plan.m_lightingHash;
		composed->m_diagnostics.m_message =
			"Composed by Global Illumination ECS from " +
			std::to_string(plan.m_states.Num()) + " baked state(s).";
		result.m_status = EGIProbesCompositionStatus::Success;
		result.m_diagnostic = composed->m_diagnostics.m_message;
		result.m_data = std::move(composed);
		result.m_names = std::move(plan.m_names);
		result.m_effectiveWeights = std::move(plan.m_effectiveWeights);
		result.m_modes = std::move(plan.m_modes);
		return result;
	}
	catch (const std::exception& exception)
	{
		result.m_status = EGIProbesCompositionStatus::InvalidInput;
		result.m_diagnostic =
			std::string("cannot compose probe states: ") + exception.what();
		return result;
	}
	catch (...)
	{
		result.m_status = EGIProbesCompositionStatus::InvalidInput;
		result.m_diagnostic =
			"cannot compose probe states: unknown failure";
		return result;
	}
}
