#pragma once

#include "RHI/Batch.hpp"

namespace Sailor::RHI
{
	// One preparation task owns this cache. Every lookup uses the same submission
	// cutoff; later world publications cannot change already captured draw metadata.
	class RHIMaterialPreparationCache
	{
	public:
		struct Entry
		{
			RHIMaterialPtr m_material{};
			RHIMaterialVersionPtr m_version{};
			uint32_t m_materialInstance = 0u;
			bool m_bHasGraphicsShaders = false;
		};

		explicit RHIMaterialPreparationCache(uint64_t submissionId) :
			m_submissionId(submissionId)
		{}

		const Entry& Get(const RHIMaterialPtr& material)
		{
			const Entry* existing = nullptr;
			if (m_entries.Find(material.GetRawPtr(), existing))
			{
				return *existing;
			}
			SAILOR_PROFILE_SCOPE("Capture material draw metadata");
			auto& entry = m_entries[material.GetRawPtr()];
			entry.m_material = material;
			if (material)
			{
				entry.m_bHasGraphicsShaders = material->GetVertexShader() && material->GetFragmentShader();
				entry.m_version = material->GetVersionForSubmission(m_submissionId);
				const auto* bindings = entry.m_version ? entry.m_version->GetBindingsRaw() : nullptr;
				if (bindings)
				{
					const RHIShaderBindingPtr* binding = nullptr;
					if (bindings->GetShaderBindings().Find("material", binding) && *binding)
					{
						entry.m_materialInstance = (*binding)->GetStorageInstanceIndex();
					}
				}
			}
			return entry;
		}

		RHIBatch MakeBatch(const RHIMaterialPtr& material, const RHIMeshPtr& mesh)
		{
			const auto& entry = Get(material);
			RHIBatch result;
			result.m_material = entry.m_material;
			result.m_materialVersion = entry.m_version;
			result.m_mesh = mesh;
			return result;
		}

	private:
		uint64_t m_submissionId = 0ull;
		TMap<const RHIMaterial*, Entry> m_entries{};
	};
}
