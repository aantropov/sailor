#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "Engine/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	namespace Experimental
	{
		class ParticlesNode : public TFrameGraphNode<ParticlesNode>
		{
		public:

			class ParticleInfo final : public IYamlSerializable
			{
			public:

				bool m_bIsLoaded = false;

				uint32_t m_screenW;
				uint32_t m_screenH;
				uint32_t m_fps;
				uint32_t m_frames;
				uint32_t m_n;
				float m_traceDecay;
				uint32_t m_traceFrames;

				virtual void Deserialize(const YAML::Node& inData)
				{
					DESERIALIZE_PROPERTY(inData, m_screenW);
					DESERIALIZE_PROPERTY(inData, m_screenH);
					DESERIALIZE_PROPERTY(inData, m_fps);
					DESERIALIZE_PROPERTY(inData, m_frames);
					DESERIALIZE_PROPERTY(inData, m_n);
					DESERIALIZE_PROPERTY(inData, m_traceDecay);
					DESERIALIZE_PROPERTY(inData, m_traceFrames);
				}
			};

			struct ParticleData
			{
				float m_bIsEnabled, m_size1, m_size2, m_nouse_;
				float m_x1, m_y1, m_z1, m_w1_;
				float m_r1, m_g1, m_b1, m_a1;
				float m_x2, m_y2, m_z2, m_w2_;
				float m_r2, m_g2, m_b2, m_a2;
			};

			struct PushConstants
			{
				uint32_t m_numInstances;
				uint32_t m_numFrames;
				uint32_t m_fps;
				uint32_t m_traceFrames = 1;
				float m_traceDecay = 1.0f;
			};

			class PerInstanceData
			{
			public:

				glm::mat4 model;
				vec4 color;
				vec4 colorOld;
				uint32_t materialInstance = 0;
				uint32_t bIsCulled = 0;
				size_t padding;

				bool operator==(const PerInstanceData& rhs) const { return this->materialInstance == rhs.materialInstance && this->model == rhs.model; }

				size_t GetHash() const
				{
					hash<glm::mat4> p;
					return p(model);
				}
			};

			SAILOR_API static const char* GetName() { return m_name; }

			SAILOR_API virtual void Process(RHI::RHIFrameGraphPtr frameGraph, RHI::RHICommandListPtr transferCommandList, RHI::RHICommandListPtr commandList, const RHI::RHISceneViewSnapshot& sceneView) override;
			SAILOR_API virtual void Clear() override;

		protected:

			ParticleInfo m_particlesHeader;
			TVector<ParticleData> m_particlesDataBinary;

			RHI::RHIShaderBindingSetPtr m_perInstanceData;

			RHI::RHIMeshPtr m_mesh;
			RHI::RHIMaterialPtr m_material;
			RHI::RHIMaterialPtr m_shadowMaterial;

			RHI::RHIBufferPtr m_particlesFrames;
			RHI::RHIBufferPtr m_instances;

			RHI::RHITexturePtr m_shadowMap;
			RHI::RHIShaderBindingSetPtr m_shadowMapBinding;

			uint32_t m_numInstances = 0;
			ShaderSetPtr m_pComputeShader{};

			static const char* m_name;
		};
	}

	template class TFrameGraphNode<Experimental::ParticlesNode>;
};
