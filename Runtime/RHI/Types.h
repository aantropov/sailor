#pragma once
#include "Core/RefPtr.hpp"
#include <glm/glm/glm.hpp>
#include <glm/glm/gtx/hash.hpp>
#include <vector>

namespace Sailor::RHI
{
	using ShaderByteCode = std::vector<uint32_t>;

	enum class ETextureFiltration : uint8_t
	{
		Nearest = 0,
		Linear,
		Bicubic
	};

	enum class ETextureClamping : uint8_t
	{
		Clamp = 0,
		Repeat
	};

	enum class ETextureType : uint8_t
	{
		Texture1D = 0,
		Texture2D = 1,
		Texture3D = 2,
	};

	enum class EMsaaSamples : uint8_t
	{
		Samples_1 = 1,
		Samples_2 = 2,
		Samples_4 = 4,
		Samples_8 = 8,
		Samples_16 = 16,
		Samples_32 = 32,
		Samples_64 = 64
	};

	enum EBufferUsageBit : uint16_t
	{
		BufferTransferSrc_Bit = 0x00000001,
		BufferTransferDst_Bit = 0x00000002,

		UniformTexelBuffer_Bit = 0x00000004,
		StorageTexelBuffer_Bit = 0x00000008,
		UniformBuffer_Bit = 0x00000010,
		StorageBuffer_Bit = 0x00000020,
		IndexBuffer_Bit = 0x00000040,
		VertexBuffer_Bit = 0x00000080,
		IndirectBuffer_Bit = 0x00000100
	};

	typedef uint16_t EBufferUsageFlags;

	enum class ETextureFormat : uint32_t
	{
		UNDEFINED = 0,
		R4G4_UNORM_PACK8 = 1,
		R4G4B4A4_UNORM_PACK16 = 2,
		B4G4R4A4_UNORM_PACK16 = 3,
		R5G6B5_UNORM_PACK16 = 4,
		B5G6R5_UNORM_PACK16 = 5,
		R5G5B5A1_UNORM_PACK16 = 6,
		B5G5R5A1_UNORM_PACK16 = 7,
		A1R5G5B5_UNORM_PACK16 = 8,
		R8_UNORM = 9,
		R8_SNORM = 10,
		R8_USCALED = 11,
		R8_SSCALED = 12,
		R8_UINT = 13,
		R8_SINT = 14,
		R8_SRGB = 15,
		R8G8_UNORM = 16,
		R8G8_SNORM = 17,
		R8G8_USCALED = 18,
		R8G8_SSCALED = 19,
		R8G8_UINT = 20,
		R8G8_SINT = 21,
		R8G8_SRGB = 22,
		R8G8B8_UNORM = 23,
		R8G8B8_SNORM = 24,
		R8G8B8_USCALED = 25,
		R8G8B8_SSCALED = 26,
		R8G8B8_UINT = 27,
		R8G8B8_SINT = 28,
		R8G8B8_SRGB = 29,
		B8G8R8_UNORM = 30,
		B8G8R8_SNORM = 31,
		B8G8R8_USCALED = 32,
		B8G8R8_SSCALED = 33,
		B8G8R8_UINT = 34,
		B8G8R8_SINT = 35,
		B8G8R8_SRGB = 36,
		R8G8B8A8_UNORM = 37,
		R8G8B8A8_SNORM = 38,
		R8G8B8A8_USCALED = 39,
		R8G8B8A8_SSCALED = 40,
		R8G8B8A8_UINT = 41,
		R8G8B8A8_SINT = 42,
		R8G8B8A8_SRGB = 43,
		B8G8R8A8_UNORM = 44,
		B8G8R8A8_SNORM = 45,
		B8G8R8A8_USCALED = 46,
		B8G8R8A8_SSCALED = 47,
		B8G8R8A8_UINT = 48,
		B8G8R8A8_SINT = 49,
		B8G8R8A8_SRGB = 50,
		A8B8G8R8_UNORM_PACK32 = 51,
		A8B8G8R8_SNORM_PACK32 = 52,
		A8B8G8R8_USCALED_PACK32 = 53,
		A8B8G8R8_SSCALED_PACK32 = 54,
		A8B8G8R8_UINT_PACK32 = 55,
		A8B8G8R8_SINT_PACK32 = 56,
		A8B8G8R8_SRGB_PACK32 = 57,
		A2R10G10B10_UNORM_PACK32 = 58,
		A2R10G10B10_SNORM_PACK32 = 59,
		A2R10G10B10_USCALED_PACK32 = 60,
		A2R10G10B10_SSCALED_PACK32 = 61,
		A2R10G10B10_UINT_PACK32 = 62,
		A2R10G10B10_SINT_PACK32 = 63,
		A2B10G10R10_UNORM_PACK32 = 64,
		A2B10G10R10_SNORM_PACK32 = 65,
		A2B10G10R10_USCALED_PACK32 = 66,
		A2B10G10R10_SSCALED_PACK32 = 67,
		A2B10G10R10_UINT_PACK32 = 68,
		A2B10G10R10_SINT_PACK32 = 69,
		R16_UNORM = 70,
		R16_SNORM = 71,
		R16_USCALED = 72,
		R16_SSCALED = 73,
		R16_UINT = 74,
		R16_SINT = 75,
		R16_SFLOAT = 76,
		R16G16_UNORM = 77,
		R16G16_SNORM = 78,
		R16G16_USCALED = 79,
		R16G16_SSCALED = 80,
		R16G16_UINT = 81,
		R16G16_SINT = 82,
		R16G16_SFLOAT = 83,
		R16G16B16_UNORM = 84,
		R16G16B16_SNORM = 85,
		R16G16B16_USCALED = 86,
		R16G16B16_SSCALED = 87,
		R16G16B16_UINT = 88,
		R16G16B16_SINT = 89,
		R16G16B16_SFLOAT = 90,
		R16G16B16A16_UNORM = 91,
		R16G16B16A16_SNORM = 92,
		R16G16B16A16_USCALED = 93,
		R16G16B16A16_SSCALED = 94,
		R16G16B16A16_UINT = 95,
		R16G16B16A16_SINT = 96,
		R16G16B16A16_SFLOAT = 97,
		R32_UINT = 98,
		R32_SINT = 99,
		R32_SFLOAT = 100,
		R32G32_UINT = 101,
		R32G32_SINT = 102,
		R32G32_SFLOAT = 103,
		R32G32B32_UINT = 104,
		R32G32B32_SINT = 105,
		R32G32B32_SFLOAT = 106,
		R32G32B32A32_UINT = 107,
		R32G32B32A32_SINT = 108,
		R32G32B32A32_SFLOAT = 109,
		R64_UINT = 110,
		R64_SINT = 111,
		R64_SFLOAT = 112,
		R64G64_UINT = 113,
		R64G64_SINT = 114,
		R64G64_SFLOAT = 115,
		R64G64B64_UINT = 116,
		R64G64B64_SINT = 117,
		R64G64B64_SFLOAT = 118,
		R64G64B64A64_UINT = 119,
		R64G64B64A64_SINT = 120,
		R64G64B64A64_SFLOAT = 121,
		B10G11R11_UFLOAT_PACK32 = 122,
		E5B9G9R9_UFLOAT_PACK32 = 123,
		D16_UNORM = 124,
		X8_D24_UNORM_PACK32 = 125,
		D32_SFLOAT = 126,
		S8_UINT = 127,
		D16_UNORM_S8_UINT = 128,
		D24_UNORM_S8_UINT = 129,
		D32_SFLOAT_S8_UINT = 130,
		BC1_RGB_UNORM_BLOCK = 131,
		BC1_RGB_SRGB_BLOCK = 132,
		BC1_RGBA_UNORM_BLOCK = 133,
		BC1_RGBA_SRGB_BLOCK = 134,
		BC2_UNORM_BLOCK = 135,
		BC2_SRGB_BLOCK = 136,
		BC3_UNORM_BLOCK = 137,
		BC3_SRGB_BLOCK = 138,
		BC4_UNORM_BLOCK = 139,
		BC4_SNORM_BLOCK = 140,
		BC5_UNORM_BLOCK = 141,
		BC5_SNORM_BLOCK = 142,
		BC6H_UFLOAT_BLOCK = 143,
		BC6H_SFLOAT_BLOCK = 144,
		BC7_UNORM_BLOCK = 145,
		BC7_SRGB_BLOCK = 146,
		ETC2_R8G8B8_UNORM_BLOCK = 147,
		ETC2_R8G8B8_SRGB_BLOCK = 148,
		ETC2_R8G8B8A1_UNORM_BLOCK = 149,
		ETC2_R8G8B8A1_SRGB_BLOCK = 150,
		ETC2_R8G8B8A8_UNORM_BLOCK = 151,
		ETC2_R8G8B8A8_SRGB_BLOCK = 152,
		EAC_R11_UNORM_BLOCK = 153,
		EAC_R11_SNORM_BLOCK = 154,
		EAC_R11G11_UNORM_BLOCK = 155,
		EAC_R11G11_SNORM_BLOCK = 156,
		ASTC_4x4_UNORM_BLOCK = 157,
		ASTC_4x4_SRGB_BLOCK = 158,
		ASTC_5x4_UNORM_BLOCK = 159,
		ASTC_5x4_SRGB_BLOCK = 160,
		ASTC_5x5_UNORM_BLOCK = 161,
		ASTC_5x5_SRGB_BLOCK = 162,
		ASTC_6x5_UNORM_BLOCK = 163,
		ASTC_6x5_SRGB_BLOCK = 164,
		ASTC_6x6_UNORM_BLOCK = 165,
		ASTC_6x6_SRGB_BLOCK = 166,
		ASTC_8x5_UNORM_BLOCK = 167,
		ASTC_8x5_SRGB_BLOCK = 168,
		ASTC_8x6_UNORM_BLOCK = 169,
		ASTC_8x6_SRGB_BLOCK = 170,
		ASTC_8x8_UNORM_BLOCK = 171,
		ASTC_8x8_SRGB_BLOCK = 172,
		ASTC_10x5_UNORM_BLOCK = 173,
		ASTC_10x5_SRGB_BLOCK = 174,
		ASTC_10x6_UNORM_BLOCK = 175,
		ASTC_10x6_SRGB_BLOCK = 176,
		ASTC_10x8_UNORM_BLOCK = 177,
		ASTC_10x8_SRGB_BLOCK = 178,
		ASTC_10x10_UNORM_BLOCK = 179,
		ASTC_10x10_SRGB_BLOCK = 180,
		ASTC_12x10_UNORM_BLOCK = 181,
		ASTC_12x10_SRGB_BLOCK = 182,
		ASTC_12x12_UNORM_BLOCK = 183,
		ASTC_12x12_SRGB_BLOCK = 184,
		G8B8G8R8_422_UNORM = 1000156000,
		B8G8R8G8_422_UNORM = 1000156001,
		G8_B8_R8_3PLANE_420_UNORM = 1000156002,
		G8_B8R8_2PLANE_420_UNORM = 1000156003,
		G8_B8_R8_3PLANE_422_UNORM = 1000156004,
		G8_B8R8_2PLANE_422_UNORM = 1000156005,
		G8_B8_R8_3PLANE_444_UNORM = 1000156006,
		R10X6_UNORM_PACK16 = 1000156007,
		R10X6G10X6_UNORM_2PACK16 = 1000156008,
		R10X6G10X6B10X6A10X6_UNORM_4PACK16 = 1000156009,
		G10X6B10X6G10X6R10X6_422_UNORM_4PACK16 = 1000156010,
		B10X6G10X6R10X6G10X6_422_UNORM_4PACK16 = 1000156011,
		G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16 = 1000156012,
		G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 = 1000156013,
		G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16 = 1000156014,
		G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16 = 1000156015,
		G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16 = 1000156016,
		R12X4_UNORM_PACK16 = 1000156017,
		R12X4G12X4_UNORM_2PACK16 = 1000156018,
		R12X4G12X4B12X4A12X4_UNORM_4PACK16 = 1000156019,
		G12X4B12X4G12X4R12X4_422_UNORM_4PACK16 = 1000156020,
		B12X4G12X4R12X4G12X4_422_UNORM_4PACK16 = 1000156021,
		G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16 = 1000156022,
		G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16 = 1000156023,
		G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16 = 1000156024,
		G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16 = 1000156025,
		G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16 = 1000156026,
		G16B16G16R16_422_UNORM = 1000156027,
		B16G16R16G16_422_UNORM = 1000156028,
		G16_B16_R16_3PLANE_420_UNORM = 1000156029,
		G16_B16R16_2PLANE_420_UNORM = 1000156030,
		G16_B16_R16_3PLANE_422_UNORM = 1000156031,
		G16_B16R16_2PLANE_422_UNORM = 1000156032,
		G16_B16_R16_3PLANE_444_UNORM = 1000156033,
		PVRTC1_2BPP_UNORM_BLOCK_IMG = 1000054000,
		PVRTC1_4BPP_UNORM_BLOCK_IMG = 1000054001,
		PVRTC2_2BPP_UNORM_BLOCK_IMG = 1000054002,
		PVRTC2_4BPP_UNORM_BLOCK_IMG = 1000054003,
		PVRTC1_2BPP_SRGB_BLOCK_IMG = 1000054004,
		PVRTC1_4BPP_SRGB_BLOCK_IMG = 1000054005,
		PVRTC2_2BPP_SRGB_BLOCK_IMG = 1000054006,
		PVRTC2_4BPP_SRGB_BLOCK_IMG = 1000054007,
		ASTC_4x4_SFLOAT_BLOCK_EXT = 1000066000,
		ASTC_5x4_SFLOAT_BLOCK_EXT = 1000066001,
		ASTC_5x5_SFLOAT_BLOCK_EXT = 1000066002,
		ASTC_6x5_SFLOAT_BLOCK_EXT = 1000066003,
		ASTC_6x6_SFLOAT_BLOCK_EXT = 1000066004,
		ASTC_8x5_SFLOAT_BLOCK_EXT = 1000066005,
		ASTC_8x6_SFLOAT_BLOCK_EXT = 1000066006,
		ASTC_8x8_SFLOAT_BLOCK_EXT = 1000066007,
		ASTC_10x5_SFLOAT_BLOCK_EXT = 1000066008,
		ASTC_10x6_SFLOAT_BLOCK_EXT = 1000066009,
		ASTC_10x8_SFLOAT_BLOCK_EXT = 1000066010,
		ASTC_10x10_SFLOAT_BLOCK_EXT = 1000066011,
		ASTC_12x10_SFLOAT_BLOCK_EXT = 1000066012,
		ASTC_12x12_SFLOAT_BLOCK_EXT = 1000066013,
		A4R4G4B4_UNORM_PACK16_EXT = 1000340000,
		A4B4G4R4_UNORM_PACK16_EXT = 1000340001
	};

	enum ETextureUsageBit : uint8_t
	{
		TextureTransferSrc_Bit = 0x00000001,
		TextureTransferDst_Bit = 0x00000002,

		Sampled_Bit = 0x00000004,
		Storage_Bit = 0x00000008,
		ColorAttachment_Bit = 0x00000010,
		DepthStencilAttachment__Bit = 0x00000020,
		TransientAttachment_Bit = 0x00000040,
		InputAttachment_Bit = 0x00000080
	};

	typedef uint8_t ETextureUsageFlags;

	enum class EShaderStage : uint8_t
	{
		Vertex = 0x00000001,
		Geometry = 0x00000008,
		Fragment = 0x00000010,
		Compute = 0x00000020
	};

	enum class EFillMode : uint8_t
	{
		Fill = 0,
		Line = 1,
		Point = 2
	};

	enum class ECullMode : uint8_t
	{
		Front = 0x00000001,
		Back = 0x00000002,
		FrontAndBack = 0x00000003
	};

	enum class EBlendMode : uint8_t
	{
		None = 0,
		Additive = 0x00000001,
		AlphaBlending = 0x00000002,
		Multiply = 0x00000003
	};

	enum class EShaderBindingType : uint8_t
	{
		Sampler = 0,
		CombinedImageSampler = 1,
		SampledImage = 2,
		StorageImage = 3,
		UniformTexelBuffer = 4,
		StorageTexelBuffer = 5,
		UniformBuffer = 6,
		StorageBuffer = 7,
		UniformBufferDynamic = 8,
		StorageBufferDynamic = 9,
		InputAttachment = 10
	};

	enum class EShaderBindingMemberType : uint8_t
	{
		Bool = 20,
		Int = 21,
		Float = 22,
		Vector = 23,
		Matrix = 24,
		Image = 25,
		Sampler = 26,
		SampledImage = 27,
		Array = 28,
		RuntimeArray = 29,
		Struct = 30
	};

	struct RenderState
	{
		RenderState(bool bEnableDepthTest = true,
			bool bEnableZWrite = true,
			float depthBias = 0.0f,
			ECullMode cullMode = ECullMode::Back,
			EBlendMode blendMode = EBlendMode::None,
			EFillMode fillMode = EFillMode::Fill) :
			m_bEnableDepthTest(bEnableDepthTest),
			m_bEnableZWrite(bEnableZWrite),
			m_depthBias(depthBias),
			m_cullMode(cullMode),
			m_blendMode(blendMode),
			m_fillMode(fillMode)
		{}

		bool IsDepthTestEnabled() const { return m_bEnableDepthTest; }
		bool IsEnabledZWrite() const { return m_bEnableZWrite; }
		ECullMode GetCullMode() const { return m_cullMode; }
		EBlendMode GetBlendMode() const { return m_blendMode; }
		EFillMode GetFillMode() const { return m_fillMode; }
		float GetDepthBias() const { return m_depthBias; }

		bool operator==(const RenderState& rhs) const
		{
			return memcmp(this, &rhs, sizeof(RenderState)) == 0;
		}

	private:

		bool m_bEnableDepthTest = true;
		bool m_bEnableZWrite = true;
		float m_depthBias = 0.0f;
		ECullMode m_cullMode = ECullMode::Back;
		EBlendMode m_blendMode = EBlendMode::None;
		EFillMode m_fillMode = EFillMode::Fill;
	};

	struct ShaderLayoutBindingMember
	{
		EShaderBindingMemberType m_type = EShaderBindingMemberType::Float;
		std::string m_name = "";
		uint32_t m_absoluteOffset = 0u;
		uint32_t m_size = 0u;
	};

	struct ShaderLayoutBinding
	{
		EShaderBindingType m_type = EShaderBindingType::CombinedImageSampler;
		std::string m_name = "";
		std::vector<ShaderLayoutBindingMember> m_members{};

		uint8_t m_binding = 0u;
		uint32_t m_size = 0u;
		uint32_t m_set = 0u;
	};

	class Vertex
	{
	public:
		glm::vec3 m_position;
		glm::vec2 m_texcoord;
		glm::vec4 m_color;

		SAILOR_API bool operator==(const Vertex& other) const
		{
			return m_position == other.m_position &&
				m_color == other.m_color &&
				m_texcoord == other.m_texcoord;
		}
	};

	struct UboFrameData
	{
		alignas(16) glm::mat4 m_view;
		alignas(16) glm::mat4 m_projection;
		alignas(16) float m_currentTime;
		alignas(16) float m_deltaTime;
	};

	struct UboTransform
	{
		alignas(16) glm::mat4 m_model;
	};

	class Resource : public TRefBase
	{
	public:

	protected:

		SAILOR_API Resource() = default;
		SAILOR_API virtual ~Resource() = default;

	private:

		SAILOR_API Resource(Resource& copy) = delete;
		SAILOR_API Resource& operator =(Resource& rhs) = delete;

		SAILOR_API Resource(Resource&& copy) = default;
		SAILOR_API Resource& operator =(Resource&& rhs) = default;
	};

	// Used to hold/track RHI resources
	class IDependent
	{
	public:

		void AddDependency(TRefPtr<Resource> dependency)
		{
			m_dependencies.push_back(std::move(dependency));
		}

		void ClearDependencies()
		{
			m_dependencies.clear();
		}

		virtual SAILOR_API ~IDependent() = default;

	protected:
		std::vector<TRefPtr<Resource>> m_dependencies;
	};

	class IObservable
	{
	public:
		SAILOR_API IObservable() = default;
		SAILOR_API IObservable(IObservable&) = default;
		SAILOR_API IObservable(IObservable&&) = default;
		SAILOR_API IObservable& operator=(IObservable&) = default;
		SAILOR_API IObservable& operator=(IObservable&&) = default;
		virtual SAILOR_API ~IObservable() = default;

		virtual void SAILOR_API TraceVisit(TRefPtr<Resource> visitor, bool& bShouldRemoveFromList) = 0;
	};

	class IVisitor
	{
	public:

		virtual SAILOR_API ~IVisitor() = default;

		void SAILOR_API AddObservable(TRefPtr<RHI::Resource> resource)
		{
			m_elements.push_back(std::move(resource));
		}

		virtual SAILOR_API void TraceObservables()
		{
			for (int32_t i = 0; i < m_elements.size(); i++)
			{
				auto& element = m_elements[i];

				auto visit = dynamic_cast<IObservable*>(element.GetRawPtr());
				assert(visit);

				bool bShouldRemove = false;
				visit->TraceVisit((TRefPtr<Resource>(dynamic_cast<Resource*>(this))), bShouldRemove);

				if (bShouldRemove)
				{
					std::iter_swap(m_elements.begin() + i, m_elements.end() - 1);
					m_elements.pop_back();
					i--;
				}
			}
		}

		SAILOR_API void ClearObservables()
		{
			m_elements.clear();
		}

	protected:

		std::vector<TRefPtr<RHI::Resource>> m_elements;
	};

	// Used to create resource after create the instance of class
	class IExplicitInitialization
	{
	public:

		SAILOR_API virtual void Compile() = 0;
		SAILOR_API virtual void Release() = 0;
	};

	// Used to track resource create/update with command lists
	class IDelayedInitialization : public IObservable, public IDependent
	{
	public:

		virtual void TraceVisit(class TRefPtr<Resource> visitor, bool& bShouldRemoveFromList) override;
		virtual bool IsReady() const;
	};

	// Used as composing approach to build the object by functionality
	template<typename TState>
	class IStateModifier
	{
	public:
		SAILOR_API virtual void Apply(TState& State) const = 0;
		SAILOR_API virtual ~IStateModifier() = default;
	};
};

namespace std
{
	inline void hash_combine(std::size_t& seed) { }

	template <typename T, typename... Rest>
	inline void hash_combine(std::size_t& seed, const T& v, Rest... rest)
	{
		std::hash<T> hasher;
		seed ^= hasher(v) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
		hash_combine(seed, rest...);
	}

	template<> struct hash<Sailor::RHI::Vertex>
	{
		SAILOR_API size_t operator()(Sailor::RHI::Vertex const& vertex) const
		{
			return ((hash<glm::vec3>()(vertex.m_position) ^
				(hash<glm::vec3>()(vertex.m_color) << 1)) >> 1) ^
				(hash<glm::vec2>()(vertex.m_texcoord) << 1);
		}
	};

	template<> struct hash<Sailor::RHI::RenderState>
	{
		SAILOR_API size_t operator()(Sailor::RHI::RenderState const& state) const
		{
			auto hash1 = hash<uint8_t>()((uint8_t)state.GetBlendMode() ^
				((uint8_t)state.GetCullMode() << 2) ^
				((uint8_t)state.GetFillMode() << 4) ^
				((uint8_t)state.IsDepthTestEnabled() << 5) ^
				((uint8_t)state.IsEnabledZWrite() << 6));

			auto hash2 = hash<float>()((float)state.GetDepthBias());

			std::size_t hash = 0;
			hash_combine(hash, hash1, hash2);

			return hash;
		}
	};
}