#pragma once
#include <mutex>
#include "AssetRegistry/UID.h"
#include "Types.h"

namespace Sailor
{
	class FrameState;
}

namespace Sailor::Win32
{
	class Window;
}

namespace Sailor::RHI
{
	typedef TRefPtr<class Buffer> BufferPtr;
	typedef TRefPtr<class CommandList> CommandListPtr;
	typedef TRefPtr<class Fence> FencePtr;
	typedef TRefPtr<class Mesh> MeshPtr;
	typedef TRefPtr<class Texture> TexturePtr;
	typedef TRefPtr<class Shader> ShaderPtr;
	typedef TRefPtr<class Material> MaterialPtr;
	typedef TRefPtr<class ShaderBinding> ShaderBindingPtr;
	typedef TRefPtr<class ShaderBindingSet> ShaderBindingSetPtr;

	class IGfxDevice
	{
	public:

		virtual SAILOR_API void Initialize(const Sailor::Win32::Window* pViewport, RHI::EMsaaSamples msaaSamples, bool bIsDebug) = 0;
		virtual SAILOR_API ~IGfxDevice() = default;

		virtual SAILOR_API bool ShouldFixLostDevice(const Sailor::Win32::Window* pViewport) = 0;
		virtual SAILOR_API void FixLostDevice(const Sailor::Win32::Window* pViewport) = 0;

		virtual SAILOR_API bool PresentFrame(const Sailor::FrameState& state,
			const std::vector<CommandListPtr>* primaryCommandBuffers = nullptr,
			const std::vector<CommandListPtr>* secondaryCommandBuffers = nullptr) const = 0;

		virtual void SAILOR_API WaitIdle() = 0;

		virtual SAILOR_API CommandListPtr CreateCommandList(bool bIsSecondary = false, bool bOnlyTransferQueue = false) = 0;
		virtual SAILOR_API BufferPtr CreateBuffer(size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API CommandListPtr CreateBuffer(BufferPtr& outbuffer, const void* pData, size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API MeshPtr CreateMesh(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices);
		virtual SAILOR_API ShaderPtr CreateShader(EShaderStage shaderStage, const ShaderByteCode& shaderSpirv) = 0;
		virtual SAILOR_API TexturePtr CreateImage(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			ETextureType type = ETextureType::Texture2D,
			ETextureFormat format = ETextureFormat::R8G8B8A8_SRGB,
			ETextureFiltration filtration = ETextureFiltration::Linear,
			ETextureClamping clamping = ETextureClamping::Clamp,
			ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit) = 0;
		virtual SAILOR_API MaterialPtr CreateMaterial(const RHI::RenderState& renderState, const UID& shader, const std::vector<std::string>& defines = {}) = 0;

		virtual SAILOR_API void SubmitCommandList(CommandListPtr commandList, FencePtr fence) = 0;

		// Shader binding set
		virtual SAILOR_API ShaderBindingSetPtr CreateShaderBindings() = 0;
		virtual SAILOR_API void AddUniformBufferToShaderBindings(ShaderBindingSetPtr& pShaderBindings, const std::string& name, size_t size, uint32_t shaderBinding) = 0;
		virtual SAILOR_API void AddSamplerToShaderBindings(ShaderBindingSetPtr& pShaderBindings, const std::string& name, RHI::TexturePtr texture, uint32_t shaderBinding) = 0;
		
		// Used for full binding update
		virtual SAILOR_API void UpdateShaderBinding(RHI::ShaderBindingSetPtr bindings, const std::string& binding, TexturePtr value) = 0;
		virtual SAILOR_API void UpdateShaderBinding_Immediate(RHI::ShaderBindingSetPtr bindings, const std::string& binding, const void* value, size_t size) = 0;

		//Immediate context
		virtual SAILOR_API BufferPtr CreateBuffer_Immediate(const void* pData, size_t size, EBufferUsageFlags usage) = 0;
		virtual SAILOR_API void CopyBuffer_Immediate(BufferPtr src, BufferPtr dst, size_t size) = 0;
		virtual SAILOR_API void SubmitCommandList_Immediate(CommandListPtr commandList);

		virtual SAILOR_API TexturePtr CreateImage_Immediate(
			const void* pData,
			size_t size,
			glm::ivec3 extent,
			uint32_t mipLevels = 1,
			ETextureType type = ETextureType::Texture2D,
			ETextureFormat format = ETextureFormat::R8G8B8A8_SRGB,
			ETextureFiltration filtration = ETextureFiltration::Linear,
			ETextureClamping clamping = ETextureClamping::Clamp,
			ETextureUsageFlags usage = ETextureUsageBit::TextureTransferSrc_Bit | ETextureUsageBit::TextureTransferDst_Bit | ETextureUsageBit::Sampled_Bit) = 0;
		//Immediate context

		SAILOR_API void TrackResources();

	protected:

		SAILOR_API void TrackDelayedInitialization(IDelayedInitialization* pResource, FencePtr handle);
		SAILOR_API void TrackPendingCommandList(FencePtr handle);

		std::mutex m_mutexTrackedFences;
		std::vector<FencePtr> m_trackedFences;
	};

	class IGfxDeviceCommands
	{
	public:

		virtual SAILOR_API void BeginCommandList(CommandListPtr cmd) = 0;
		virtual SAILOR_API void EndCommandList(CommandListPtr cmd) = 0;
		
		virtual SAILOR_API void UpdateShaderBinding(CommandListPtr cmd, RHI::ShaderBindingPtr binding, const void* data, size_t size, size_t variableOffset = 0) = 0;
		virtual SAILOR_API void SetMaterialParameter(CommandListPtr cmd, RHI::MaterialPtr material, const std::string& binding, const std::string& variable, const void* value, size_t size) = 0;

		// Used for variables inside uniform buffer 
		// 'customData.color' would be parsed as 'customData' buffer with 'color' variable
		template<typename TDataType>
		void SetMaterialParameter(CommandListPtr cmd, RHI::MaterialPtr material, const std::string& parameter, const TDataType& value)
		{
			std::vector<std::string> splittedString = Utils::SplitString(parameter, ".");
			SetMaterialParameter(cmd, material, splittedString[0], splittedString[1], &value, sizeof(value));
		}
	};
};
