#pragma once
#include "RHI/SceneView.h"

namespace Sailor::RHI
{
	// Flight-local CPU draw lists may survive submission reset. Retain the immutable
	// roots owning their visible records; a new frame number alone is not a change.
	class RHIPreparedDrawCache
	{
	public:
		bool Matches(const RHISceneViewSnapshot& view, size_t queueTag, uint32_t options,
			const RHIShaderBindingSetPtr& textures, uint64_t textureRevision) const
		{
			if (!m_bValid || !view.m_camera || !view.m_submissionContext ||
				m_queueTag != queueTag || m_options != options ||
				m_materialRevision != view.m_submissionContext->GetMaterialRevision() ||
				m_textures != textures || m_textureRevision != textureRevision ||
				!SameVersions(m_sceneVersions, view.m_sceneVersions) ||
				!SameVersions(m_motionVersions, MotionVersions(view)) ||
				m_renderMode != view.m_renderMode ||
				m_viewMatrix != view.m_camera->GetViewMatrix() ||
				m_projectionMatrix != view.m_camera->GetProjectionMatrix() ||
				m_cameraPosition != view.m_cameraTransform.m_position ||
				m_proxies.Num() != view.m_proxies.Num() ||
				!(m_lodMeshes == view.m_lodMeshes) ||
				!(m_instancedLodOffsets == view.m_instancedLodOffsets))
			{
				return false;
			}
			// Visibility and selected LODs can change independently of the scene root
			// (for example, streaming). Compare the actual ordered selection as well.
			for (size_t i = 0u; i < m_proxies.Num(); ++i)
			{
				const auto& previous = m_proxies[i];
				const auto& current = view.m_proxies[i];
				if (previous.m_handle != current.m_handle || previous.m_record != current.m_record ||
					previous.m_resource != current.m_resource ||
					previous.m_meshLodOffset != current.m_meshLodOffset ||
					previous.m_instancedLodOffset != current.m_instancedLodOffset)
				{
					return false;
				}
			}
			return true;
		}

		void Capture(const RHISceneViewSnapshot& view, size_t queueTag, uint32_t options,
			const RHIShaderBindingSetPtr& textures, uint64_t textureRevision, bool bComplete)
		{
			if (!bComplete || !view.m_sceneVersions || !view.m_camera || !view.m_submissionContext)
			{
				Invalidate();
				return;
			}
			m_sceneVersions = view.m_sceneVersions;
			m_motionVersions = MotionVersions(view);
			m_materialRevision = view.m_submissionContext->GetMaterialRevision();
			m_textures = textures;
			m_textureRevision = textureRevision;
			m_queueTag = queueTag;
			m_options = options;
			m_viewMatrix = view.m_camera->GetViewMatrix();
			m_projectionMatrix = view.m_camera->GetProjectionMatrix();
			m_cameraPosition = view.m_cameraTransform.m_position;
			m_renderMode = view.m_renderMode;
			m_proxies = view.m_proxies;
			m_lodMeshes = view.m_lodMeshes;
			m_instancedLodOffsets = view.m_instancedLodOffsets;
			m_bValid = true;
		}

		void Invalidate()
		{
			m_bValid = false;
			m_proxies.Clear(false);
			m_lodMeshes.Clear(false);
			m_instancedLodOffsets.Clear(false);
			m_sceneVersions.Clear();
			m_motionVersions.Clear();
			m_textures.Clear();
		}

	private:
		using SceneVersions = TSharedPtr<const TVector<RHISceneVersionPtr>>;

		static SceneVersions MotionVersions(const RHISceneViewSnapshot& view)
		{
			return view.m_previousMotionFrame ? view.m_previousMotionFrame->m_sceneVersions : SceneVersions{};
		}

		static bool SameVersions(const SceneVersions& lhs, const SceneVersions& rhs)
		{
			return lhs == rhs || (lhs && rhs && *lhs == *rhs);
		}

		SceneVersions m_sceneVersions;
		SceneVersions m_motionVersions;
		TVector<RHIVisibleSceneProxy> m_proxies;
		TVector<RHIMeshPtr> m_lodMeshes;
		TVector<uint32_t> m_instancedLodOffsets;
		RHIShaderBindingSetPtr m_textures;
		glm::mat4 m_viewMatrix{};
		glm::mat4 m_projectionMatrix{};
		glm::vec4 m_cameraPosition{};
		ESceneViewRenderMode m_renderMode = ESceneViewRenderMode::Lit;
		uint64_t m_materialRevision = 0ull;
		uint64_t m_textureRevision = 0ull;
		size_t m_queueTag = 0u;
		uint32_t m_options = 0u;
		bool m_bValid = false;
	};
}
