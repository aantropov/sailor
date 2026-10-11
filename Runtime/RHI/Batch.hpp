#pragma once

#include "RHI/Buffer.h"
#include "RHI/Material.h"
#include "RHI/Mesh.h"
#include <limits>

namespace Sailor::RHI
{
	class RHIBatch
	{
	public:
		// Bound both an instanced indirect command and a submitted draw run.
		// GPU compaction processes each command independently; a single large
		// vegetation group must not leave all of that work on one invocation.
#if defined(_WIN32)
		static constexpr uint32_t MaxInstancesPerBatch = 2048u;
#else
		static constexpr uint32_t MaxInstancesPerBatch = (std::numeric_limits<uint32_t>::max)();
#endif

		RHIMaterialPtr m_material;
		RHIMaterialVersionPtr m_materialVersion;

		// Here we store the vertex and index bindings that could be shared during rendering (not meshes)
		RHIMeshPtr m_mesh;
		RHIShaderBindingSetPtr m_textureBindings;
		uint32_t m_supportedMeshesPerBatch = std::numeric_limits<uint32_t>::max();

		RHIBatch() = default;
		RHIBatch(const RHIMaterialPtr& material, const RHIMeshPtr& mesh) :
			m_material(material),
			m_materialVersion(material ? material->GetVersion() : RHIMaterialVersionPtr{}),
			m_mesh(mesh)
		{}
		RHIBatch(
			const RHIMaterialPtr& material,
			const RHIMeshPtr& mesh,
			uint64_t submissionId) :
			m_material(material),
			m_materialVersion(material ?
				material->GetVersionForSubmission(submissionId) : RHIMaterialVersionPtr{}),
			m_mesh(mesh)
		{}

		RHIShaderBindingSetPtr GetMaterialBindings() const
		{
			return m_materialVersion ? m_materialVersion->GetBindings() : RHIShaderBindingSetPtr{};
		}

		const RHIShaderBindingSet* GetMaterialBindingsRaw() const
		{
			return m_materialVersion ? m_materialVersion->GetBindingsRaw() : nullptr;
		}

		bool operator==(const RHIBatch& rhs) const
		{
			// Shared meshes in a traffic/vegetation run already have identical
			// resource identities. Avoid retaining bindings and shaders per instance.
			if (m_material == rhs.m_material &&
				m_materialVersion == rhs.m_materialVersion &&
				m_mesh == rhs.m_mesh && m_textureBindings == rhs.m_textureBindings)
			{
				return true;
			}
			const auto* bindings = GetMaterialBindingsRaw();
			const auto* rhsBindings = rhs.GetMaterialBindingsRaw();
			if (!m_material || !rhs.m_material || !m_mesh || !rhs.m_mesh ||
				!m_mesh->m_vertexBuffer || !rhs.m_mesh->m_vertexBuffer ||
				!m_mesh->m_indexBuffer || !rhs.m_mesh->m_indexBuffer ||
				!bindings || !rhsBindings)
			{
				return m_material == rhs.m_material &&
					m_materialVersion == rhs.m_materialVersion &&
					m_mesh == rhs.m_mesh &&
					m_textureBindings == rhs.m_textureBindings;
			}

			const bool bSameBatch =
				m_materialVersion == rhs.m_materialVersion &&
				bindings->GetCompatibilityHashCode() == rhsBindings->GetCompatibilityHashCode() &&
				m_material->GetVertexShader() == rhs.m_material->GetVertexShader() &&
				m_material->GetFragmentShader() == rhs.m_material->GetFragmentShader() &&
				m_material->GetRenderState() == rhs.m_material->GetRenderState() &&
				m_mesh->m_vertexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_vertexBuffer->GetCompatibilityHashCode() &&
				m_mesh->m_indexBuffer->GetCompatibilityHashCode() == rhs.m_mesh->m_indexBuffer->GetCompatibilityHashCode() &&
				m_textureBindings == rhs.m_textureBindings;

			return bSameBatch;
		}

		size_t GetHash() const
		{
			const auto* bindings = GetMaterialBindingsRaw();
			size_t hash = bindings ? bindings->GetCompatibilityHashCode() : 0u;

			HashCombine(hash, m_materialVersion);
			if (m_material)
			{
				HashCombine(hash, Sailor::GetHash(m_material->GetRenderState()));
			}
			if (m_mesh && m_mesh->m_vertexBuffer && m_mesh->m_indexBuffer)
			{
				HashCombine(
					hash,
					m_mesh->m_vertexBuffer->GetCompatibilityHashCode(),
					m_mesh->m_indexBuffer->GetCompatibilityHashCode());
			}
			else
			{
				HashCombine(hash, m_mesh);
			}
			HashCombine(hash, m_textureBindings);
			return hash;
		}
	};
}
