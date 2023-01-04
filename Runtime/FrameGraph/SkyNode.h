#pragma once
#include "Core/Defines.h"
#include "Memory/RefPtr.hpp"
#include "Engine/Object.h"
#include "RHI/Types.h"
#include "FrameGraph/BaseFrameGraphNode.h"
#include "FrameGraph/FrameGraphNode.h"

namespace Sailor::Framegraph
{
	class SkyNode : public TFrameGraphNode<SkyNode>
	{
		const uint32_t SkyResolution = 128u;
		const uint32_t SunResolution = 8u;

		struct BrighStarCatalogue_Header
		{
			int32_t	m_baseSequenceIndex;
			int32_t m_firstStarIndex;
			int32_t m_starCount;
			int32_t m_starIndexType;

			uint32_t m_properMotionFlag;

			int32_t m_magnitudeType;
			int32_t m_starEntrySize;
		};

#pragma pack(1)
		struct BrighStarCatalogue_Entry
		{
			float m_catalogueNumber{}; // Catalog number of star
			double m_SRA0{}; // B1950 Right Ascension(radians)
			double m_SDEC0{}; // B1950 Declination(radians)
			char m_IS[2]{}; // Spectral type(2 characters)
			int16_t m_mag{}; // V Magnitude * 100
			float m_XRPM{}; // R.A.proper motion(radians per year)
			float m_XDPM{}; // Dec.proper motion(radians per year)
		};
#pragma pack()

	public:
		SAILOR_API static const char* GetName() { return m_name; }

		SAILOR_API virtual void Process(RHI::RHIFrameGraph* frameGraph,
			RHI::RHICommandListPtr transferCommandList,
			RHI::RHICommandListPtr commandList,
			const RHI::RHISceneViewSnapshot& sceneView) override;

		SAILOR_API virtual void Clear() override;

		SAILOR_API RHI::RHIShaderBindingSetPtr GetShaderBindings() { return m_pShaderBindings; }
		SAILOR_API void SetLocation(float latitudeDegrees, float longitudeDegrees);

	protected:

		static const char* m_name;

		struct PushConstants
		{
			mat4 m_starsModelView{};
		};

		mat4 m_starsModelView{};

		ShaderSetPtr m_pSunShader{};
		ShaderSetPtr m_pSkyShader{};
		ShaderSetPtr m_pStarsShader{};
		ShaderSetPtr m_pComposeShader{};

		RHI::RHIMaterialPtr m_pStarsMaterial{};
		RHI::RHIMaterialPtr m_pSkyMaterial{};
		RHI::RHIMaterialPtr m_pSunMaterial{};
		RHI::RHIMaterialPtr m_pComposeMaterial{};

		RHI::RHIShaderBindingSetPtr m_pShaderBindings{};

		RHI::RHITexturePtr m_pSkyTexture{};
		RHI::RHITexturePtr m_pSunTexture{};

		RHI::RHIMeshPtr m_starsMesh{};

		Tasks::TaskPtr<RHI::RHIMeshPtr, TPair<TVector<RHI::VertexP3C4>, TVector<uint32_t>>> m_loadMeshTask{};
		Tasks::TaskPtr<RHI::RHIMeshPtr, TPair<TVector<RHI::VertexP3C4>, TVector<uint32_t>>> CreateStarsMesh();

		static uint32_t MorganKeenanToTemperature(char spectral_type, char sub_type);

		// Goes from 1000 to 40000 in increases of 100 Kelvin.
		static constexpr uint32_t s_maxRgbTemperatures = (40000 / 100) - (1000 / 100);
		static glm::vec3 s_rgbTemperatures[s_maxRgbTemperatures];

		static const glm::vec3& TemperatureToColor(uint32_t temperature);
		static const glm::vec3& MorganKeenanToColor(char spectralType, char subType);
	};

	template class TFrameGraphNode<SkyNode>;
};
