#include "Sailor.h"
#include "Engine/Frame.h"
#include "Engine/EngineLoop.h"
#include "AssetRegistry/FrameGraph/FrameGraphImporter.h"
#include "Support/TempDirectory.h"
#include "EditorEngineProtocolInternal.h"
#include "EditorEngineWebSocketServer.h"
#include "Support/EditorProtocolWire.h"
#include <ixwebsocket/IXGetFreePort.h>
#include <ixwebsocket/IXNetSystem.h>
#if defined(__APPLE__)
#include "Support/MacViewportPresentation.h"
#endif
#include "FrameGraph/ParticlesNode.h"
#include "FrameGraph/EditorReadbackNode.h"
#include "Editor/EditorRuntimeBridge.h"
#include "Submodules/EditorRemote/RemoteViewportMacTransport.h"
#include "GraphicsDriver/Vulkan/VulkanDevice.h"
#include "GraphicsDriver/Vulkan/VulkanGraphicsDriver.h"
#include "GraphicsDriver/Vulkan/VulkanImage.h"
#include "GraphicsDriver/Vulkan/VulkanImageView.h"
#include "GraphicsDriver/Vulkan/VulkanQueue.h"
#include "GraphicsDriver/Vulkan/VulkanFence.h"
#include "GraphicsDriver/Vulkan/VulkanSemaphore.h"
#include "GraphicsDriver/Vulkan/VulkanSwapchain.h"
#include "RHI/Buffer.h"
#include "RHI/CommandList.h"
#include "RHI/Fence.h"
#include "RHI/Material.h"
#include "RHI/Renderer.h"
#include "RHI/RenderTarget.h"
#include "RHI/Surface.h"
#include "RHI/Texture.h"
#include "Tasks/Tasks.h"
#if defined(_WIN32)
#include <Windows.h>
#include "Submodules/EditorRemote/RemoteViewportWindowsNative.h"
#endif

#include <iostream>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <dlfcn.h>

namespace { std::atomic<uint32_t> deviceIdleCalls{ 0 }; }

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
	static auto nativeWait = reinterpret_cast<PFN_vkDeviceWaitIdle>(dlsym(RTLD_NEXT, "vkDeviceWaitIdle"));
	++deviceIdleCalls;
	return nativeWait ? nativeWait(device) : VK_ERROR_INITIALIZATION_FAILED;
}
#endif

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

namespace Sailor::Tests { int RunPathTracerCommandTests(int argc, const char** argv); }

extern "C" SAILOR_SHARED_API int32_t SailorProtocolStopLocalHost(bool bShutdownEngine) noexcept;
extern "C" SAILOR_SHARED_API int32_t SailorProtocolStartLocalHost(const uint8_t* requestData, uint32_t requestSize,
	uint16_t port, const char* token, uint32_t tokenSize) noexcept;
extern "C" SAILOR_SHARED_API void SailorProtocolRequestLocalHostStop() noexcept;
extern "C" SAILOR_SHARED_API void SailorProtocolFreeBuffer(uint8_t* buffer) noexcept;
extern "C" SAILOR_SHARED_API int32_t SailorProtocolInvoke(const uint8_t* requestData, uint32_t requestSize,
	uint8_t** responseData, uint32_t* responseSize) noexcept;

namespace Sailor::GraphicsDriver::Vulkan
{
	class VulkanSubmissionTestAccess
	{
	public:
		static void ExchangeFenceDispatch(VulkanDevice& device, PFN_vkGetFenceStatus& status, PFN_vkWaitForFences& wait)
		{
			std::swap(device.m_getFenceStatus, status);
			std::swap(device.m_waitForFences, wait);
		}

		static PFN_vkQueueSubmit ExchangeSubmit(VulkanQueue& queue, PFN_vkQueueSubmit submit,
			PFN_vkQueueSubmit* forward = nullptr)
		{
			queue.m_lock.Lock();
			auto previous = std::exchange(queue.m_queueSubmit, submit);
			if (forward) *forward = previous;
			queue.m_lock.Unlock();
			return previous;
		}

		static size_t Flight(const VulkanDevice& device) { return device.m_currentFrame; }
		static PFN_vkQueueWaitIdle ExchangeQueueWait(VulkanQueue& queue, PFN_vkQueueWaitIdle wait)
		{
			queue.m_lock.Lock();
			const auto previous = std::exchange(queue.m_queueWaitIdle, wait);
			queue.m_lock.Unlock();
			return previous;
		}
		static VulkanFencePtr FlightFence(const VulkanDevice& device) { return device.m_syncFences[device.m_currentFrame]; }
		static VulkanSemaphorePtr AcquireSemaphore(const VulkanDevice& device) { return device.m_imageAvailableSemaphores[device.m_currentFrame]; }
		static VulkanSemaphorePtr PresentSemaphore(const VulkanDevice& device) { return device.m_renderFinishedSemaphores[device.m_currentSwapchainImageIndex]; }
		static uint32_t ImageIndex(const VulkanDevice& device) { return device.m_currentSwapchainImageIndex; }
		static VulkanQueuePtr UploadQueue(VulkanDevice& device, bool transfer = false)
		{
			const auto family = transfer ? device.m_queueFamilies.m_transferFamily : device.m_queueFamilies.m_graphicsFamily;
			if (family == device.m_queueFamilies.m_computeFamily) return device.m_computeQueue;
			if (family == device.m_queueFamilies.m_transferFamily) return device.m_transferQueue;
			return device.m_graphicsQueue;
		}
		static void CheckSyncCounts(const VulkanDevice& device)
		{
			const size_t images = device.m_swapchain->GetImageViews().Num();
			if (device.m_renderFinishedSemaphores.Num() != images || device.m_syncImages.Num() != images ||
				device.m_swapchainImagesInitialized.Num() != images || device.m_syncFences.Num() != VulkanApi::MaxFramesInFlight ||
				device.m_imageAvailableSemaphores.Num() != VulkanApi::MaxFramesInFlight || device.m_frameDeps.Num() != VulkanApi::MaxFramesInFlight)
				throw std::runtime_error("synchronization storage must follow image/flight ownership");
		}
	};
}

namespace
{
	void Require(bool condition, const char* message)
	{
		if (!condition) throw std::runtime_error(message);
	}

	thread_local VkResult nextResult = VK_SUCCESS;
	thread_local uint32_t submitCalls = 0u;
	thread_local uint32_t lastSubmitCount = 0u;
	thread_local uint32_t lastCommandCount = 0u;
	thread_local const void* lastSubmitNext = nullptr;
	thread_local bool rejectNativeSubmit = false;
	thread_local bool submitBeforeFailure = false;
	thread_local uint32_t submitsBeforeFailure = 0u;
	thread_local VkSemaphore lastWait = VK_NULL_HANDLE;
	thread_local VkSemaphore lastSignal = VK_NULL_HANDLE;
	PFN_vkQueueSubmit forwardNativeSubmit = nullptr;
	PFN_vkQueueWaitIdle forwardQueueWait = nullptr;
	thread_local VkResult queueWaitResult = VK_SUCCESS;
	thread_local uint32_t queueWaitCalls = 0u;
	PFN_vkGetFenceStatus forwardFenceStatus = nullptr;
	PFN_vkWaitForFences forwardFenceWait = nullptr;
	thread_local std::array<VkFence, 2> observedFences{};
	thread_local std::array<VkResult, 2> fenceResults{};
	thread_local uint32_t fenceStatusCalls = 0u;
	thread_local uint32_t fenceWaitCalls = 0u;
	thread_local uint32_t allFenceWaitCalls = 0u;
	thread_local VkResult fenceWaitResult = VK_TIMEOUT;
	thread_local bool captureNextFenceWait = false;
	thread_local bool capturedFenceCompleted = false;
	thread_local uint32_t waitsBeforeCapture = 0u;

	VKAPI_ATTR VkResult VKAPI_CALL NativeQueueWait(VkQueue queue)
	{
		// Finish actual work before reporting a synthetic shutdown failure.
		const auto result = forwardQueueWait(queue);
		++queueWaitCalls;
		return result == VK_SUCCESS ? queueWaitResult : result;
	}

	class QueueWaitOverride
	{
	public:
		QueueWaitOverride(VulkanQueuePtr queue, VkResult result) : m_queue(std::move(queue))
		{
			queueWaitResult = result;
			m_original = VulkanSubmissionTestAccess::ExchangeQueueWait(*m_queue, NativeQueueWait);
			forwardQueueWait = m_original;
		}
		~QueueWaitOverride() { VulkanSubmissionTestAccess::ExchangeQueueWait(*m_queue, m_original); }
	private:
		VulkanQueuePtr m_queue;
		PFN_vkQueueWaitIdle m_original;
	};

	VKAPI_ATTR VkResult VKAPI_CALL NativeFenceStatus(VkDevice device, VkFence fence)
	{
		for (size_t i = 0; i < observedFences.size(); ++i)
		{
			if (fence == observedFences[i])
			{
				++fenceStatusCalls;
				return fenceResults[i];
			}
		}
		return forwardFenceStatus(device, fence);
	}

	VKAPI_ATTR VkResult VKAPI_CALL NativeFenceWait(VkDevice device, uint32_t count, const VkFence* fences,
		VkBool32 all, uint64_t timeout)
	{
		++allFenceWaitCalls;
		if (count == 1u && captureNextFenceWait)
		{
			if (waitsBeforeCapture > 0u)
			{
				--waitsBeforeCapture;
				return forwardFenceWait(device, count, fences, all, timeout);
			}
			captureNextFenceWait = false;
			observedFences[0] = fences[0];
			const auto actual = forwardFenceWait(device, count, fences, all, 5000000000ull);
			capturedFenceCompleted = actual == VK_SUCCESS;
			if (!capturedFenceCompleted) return actual;
		}
		if (count == 1u && fences[0] == observedFences[0])
		{
			++fenceWaitCalls;
			return fenceWaitResult;
		}
		return forwardFenceWait(device, count, fences, all, timeout);
	}

	class FenceDispatchOverride
	{
	public:
		explicit FenceDispatchOverride(VulkanDevice& device) : m_device(device)
		{
			// App::Start is never called; all scheduler queues are drained before this scope.
			VulkanSubmissionTestAccess::ExchangeFenceDispatch(device, m_status, m_wait);
			forwardFenceStatus = m_status;
			forwardFenceWait = m_wait;
		}

		~FenceDispatchOverride()
		{
			VulkanSubmissionTestAccess::ExchangeFenceDispatch(m_device, m_status, m_wait);
			observedFences = {};
			captureNextFenceWait = false;
			waitsBeforeCapture = 0u;
		}

	private:
		VulkanDevice& m_device;
		PFN_vkGetFenceStatus m_status = NativeFenceStatus;
		PFN_vkWaitForFences m_wait = NativeFenceWait;
	};

	VKAPI_ATTR VkResult VKAPI_CALL StubSubmit(VkQueue, uint32_t count, const VkSubmitInfo*, VkFence)
	{
		++submitCalls;
		lastSubmitCount = count;
		return nextResult;
	}

	VKAPI_ATTR VkResult VKAPI_CALL NativeSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* info, VkFence fence)
	{
		if (rejectNativeSubmit && submitsBeforeFailure > 0u)
		{
			--submitsBeforeFailure;
			return forwardNativeSubmit(queue, count, info, fence);
		}
		if (rejectNativeSubmit)
		{
			StubSubmit(queue, count, info, fence);
			lastCommandCount = count ? info[0].commandBufferCount : 0u;
			lastSubmitNext = count ? info[0].pNext : nullptr;
			lastWait = count && info[0].waitSemaphoreCount ? info[0].pWaitSemaphores[info[0].waitSemaphoreCount - 1u] : VK_NULL_HANDLE;
			lastSignal = count && info[0].signalSemaphoreCount ? info[0].pSignalSemaphores[0] : VK_NULL_HANDLE;
			if (nextResult != VK_SUCCESS && !submitBeforeFailure) return nextResult;
		}
		const VkResult actual = forwardNativeSubmit(queue, count, info, fence);
		return rejectNativeSubmit && actual == VK_SUCCESS ? nextResult : actual;
	}

	void TestQueueDispatch()
	{
		auto native = VulkanQueuePtr::Make(VK_NULL_HANDLE, 5u, 2u);
		Require(native->QueueFamilyIndex() == 5u && native->QueueIndex() == 2u,
			"ordinary queue construction must preserve family and index without exposing loader symbols to clients");
		auto queue = VulkanQueuePtr::Make(VK_NULL_HANDLE, 3u, 1u, StubSubmit);
		VkSubmitInfo info{ VK_STRUCTURE_TYPE_SUBMIT_INFO };
		for (VkResult result : { VK_SUCCESS, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY, VK_ERROR_DEVICE_LOST })
		{
			nextResult = result;
			const auto before = submitCalls;
			Require(queue->Submit(info) == result && submitCalls == before + 1u && lastSubmitCount == 1u,
				"single submission must forward the actual queue result");
			Require(queue->Submit(TVector<VkSubmitInfo>{ info, info }) == result &&
				submitCalls == before + 2u && lastSubmitCount == 2u,
				"batch submission must forward its complete count and queue result");
		}
	}

	class SubmitOverride
	{
	public:
		SubmitOverride(VulkanQueuePtr queue, VkResult result, bool accepted = false, uint32_t precedingSubmits = 0u) : m_queue(std::move(queue))
		{
			nextResult = result;
			submitBeforeFailure = accepted;
			submitsBeforeFailure = precedingSubmits;
			rejectNativeSubmit = true;
			m_previous = VulkanSubmissionTestAccess::ExchangeSubmit(*m_queue, NativeSubmit, &forwardNativeSubmit);
		}

		~SubmitOverride()
		{
			VulkanSubmissionTestAccess::ExchangeSubmit(*m_queue, m_previous);
			rejectNativeSubmit = false;
		}

	private:
		VulkanQueuePtr m_queue;
		PFN_vkQueueSubmit m_previous;
	};

	struct RecordedFrame
	{
		RHICommandListPtr command;
		RHIBufferPtr readback;
		std::array<uint32_t, 64> expected;
	};

	RecordedFrame RecordFrame(uint32_t seed)
	{
		RecordedFrame frame;
		for (uint32_t i = 0; i < frame.expected.size(); ++i) frame.expected[i] = seed + 37u * i;
		frame.readback = Renderer::GetDriver()->CreateBuffer(sizeof(frame.expected), EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		std::fill_n(static_cast<uint32_t*>(frame.readback->GetPointer()), frame.expected.size(), 0xdeadbeefu);
		frame.command = Renderer::GetDriver()->CreateCommandList(false, ECommandListQueue::Graphics);
		auto commands = Renderer::GetDriverCommands();
		commands->BeginCommandList(frame.command, true);
		frame.command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
		commands->UpdateBuffer(frame.command, frame.readback, frame.expected.data(), sizeof(frame.expected));
		frame.command->m_vulkan.m_commandBuffer->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		commands->EndCommandList(frame.command);
		return frame;
	}

	void CheckReadback(RecordedFrame& frame, bool submitted)
	{
		const auto* actual = static_cast<const uint32_t*>(frame.readback->GetPointer());
		for (size_t i = 0; i < frame.expected.size(); ++i)
			Require(actual[i] == (submitted ? frame.expected[i] : 0xdeadbeefu),
				"GPU frame readback does not match accepted/rejected submission");
	}

	struct FailedFrame
	{
		RecordedFrame frame;
		VulkanFencePtr fence;
		VulkanSemaphorePtr acquire;
		VulkanSemaphorePtr finished;
	};

	void TestNativeFrameFailure(bool present, VkResult error, FailedFrame& failed)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		Require(device->WaitIdle() == VK_SUCCESS, "native setup must drain successfully");
		VulkanSubmissionTestAccess::CheckSyncCounts(*device);
		if (present)
		{
			uint32_t flight;
			bool hasImage;
			Require(device->BeginRenderSubmission(flight, hasImage) && hasImage,
				"present test must acquire a real swapchain image");
		}
		const auto flight = VulkanSubmissionTestAccess::Flight(*device);
		failed.fence = VulkanSubmissionTestAccess::FlightFence(*device);
		failed.acquire = VulkanSubmissionTestAccess::AcquireSemaphore(*device);
		failed.finished = VulkanSubmissionTestAccess::PresentSemaphore(*device);
		failed.frame = RecordFrame(713u);
		auto& command = failed.frame.command;
		const auto callsBefore = submitCalls;
		bool submitted;
		{
			SubmitOverride override(device->GetGraphicsQueue(), error);
			submitted = present ? device->PresentFrame(Sailor::FrameState{}, { command->m_vulkan.m_commandBuffer }, {}) :
				device->SubmitFrameWithoutPresent({ command->m_vulkan.m_commandBuffer }, {});
		}
		Require(!submitted && submitCalls == callsBefore + 1u, "the actual native queue call must report the injected failure");
		Require(failed.fence->Status() == VK_NOT_READY, "a rejected submission must not signal its real native fence");
		// Stop the predecessor before it can cycle back to an unsignalable fence.
		Require(VulkanSubmissionTestAccess::Flight(*device) == flight,
			"failed submission advanced the flight slot");
		Require(!device->WasLastFrameSubmitSuccessful(), "failed frame must not report successful submission");
		uint32_t nextFlight;
		bool hasNextImage;
		Require(!device->BeginRenderSubmission(nextFlight, hasNextImage),
			"failed flight must be rejected before another native fence wait or acquire");
		Require(!device->PresentFrame(Sailor::FrameState{}, {}, {}) && !device->SubmitFrameWithoutPresent({}, {}),
			"both frame paths must reject further work until recovery");
		Require(command->m_vulkan.m_commandBuffer.NumRefs() == 2u,
			"the failed flight must retain the recorded command until recovery drains it");
		CheckReadback(failed.frame, false);
	}

	void TestRecoveredFrames(bool present, FailedFrame& failed)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		VulkanSubmissionTestAccess::CheckSyncCounts(*device);
		Require(failed.fence->Status() == VK_NOT_READY && VulkanSubmissionTestAccess::FlightFence(*device) != failed.fence,
			"recovery must replace the failed fence, not signal it");
		Require(VulkanSubmissionTestAccess::AcquireSemaphore(*device) != failed.acquire &&
			VulkanSubmissionTestAccess::PresentSemaphore(*device) != failed.finished,
			"recovery must replace possibly signaled semaphores");
		Require(failed.frame.command->m_vulkan.m_commandBuffer.NumRefs() == 1u,
			"drained failed commands must no longer be retained by the device");
		TVector<VkSemaphore> imageSignals;
		imageSignals.Resize(device->GetSwapchain()->GetImageViews().Num());
		for (size_t i = 0; i < 3u * VulkanApi::MaxFramesInFlight; ++i)
		{
			uint32_t flight = static_cast<uint32_t>(VulkanSubmissionTestAccess::Flight(*device));
			bool hasImage = false;
			if (present)
				Require(device->BeginRenderSubmission(flight, hasImage) && hasImage, "recovered frame must acquire an image");
			auto fence = VulkanSubmissionTestAccess::FlightFence(*device);
			auto frame = RecordFrame(19u + static_cast<uint32_t>(i));
			VkSemaphore acquire = *VulkanSubmissionTestAccess::AcquireSemaphore(*device);
			VkSemaphore finished = *VulkanSubmissionTestAccess::PresentSemaphore(*device);
			const uint32_t imageIndex = VulkanSubmissionTestAccess::ImageIndex(*device);
			bool submitted;
			{
				SubmitOverride observe(device->GetGraphicsQueue(), VK_SUCCESS);
				submitted = present ? device->PresentFrame(Sailor::FrameState{}, { frame.command->m_vulkan.m_commandBuffer }, {}) :
					device->SubmitFrameWithoutPresent({ frame.command->m_vulkan.m_commandBuffer }, {});
			}
			Require(submitted && device->WasLastFrameSubmitSuccessful(), "recovered frame must submit real GPU work");
			Require(VulkanSubmissionTestAccess::Flight(*device) == (flight + 1u) % VulkanApi::MaxFramesInFlight,
				"successful frames must advance exactly one flight");
			Require(lastWait == (present ? acquire : VK_NULL_HANDLE) && lastSignal == (present ? finished : VK_NULL_HANDLE),
				"native submission must use the flight acquire and image presentation semaphore");
			if (present)
			{
				Require(!imageSignals[imageIndex] || imageSignals[imageIndex] == finished,
					"a reacquired image must retain its own presentation semaphore");
				imageSignals[imageIndex] = finished;
			}
			Require(fence->Wait(5000000000ull) == VK_SUCCESS, "recovered frame fence did not complete within five seconds");
			CheckReadback(frame, true);
		}
		Require(device->WaitIdle() == VK_SUCCESS, "recovered frames must drain");
	}

	void TestNativeUploadFailure(VkResult error, bool accepted)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		auto frame = RecordFrame(177u);
		auto resource = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto fence = RHIFencePtr::Make();
		driver->TrackDelayedInitialization(resource.GetRawPtr(), fence);
		const auto callsBefore = submitCalls;
		bool submitted;
		{
			SubmitOverride override(VulkanSubmissionTestAccess::UploadQueue(*device), error, accepted);
			submitted = driver->SubmitCommandList(frame.command, fence);
		}
		Require(!submitted && submitCalls == callsBefore + 1u, "upload must reach the injected native submit");
		Require(fence->HasFailed() && !fence->IsFinished() && resource->HasInitializationFailed() && !resource->IsReady(),
			"a failed native upload must not publish successful initialization");
		Require(fence->m_vulkan.m_fence->Status() == (accepted ? VK_ERROR_DEVICE_LOST : VK_NOT_READY),
			"potentially submitted work must drain before failed-upload dependencies are released");
		Require(resource.NumRefs() == 1u && fence.NumRefs() == 1u && frame.command.NumRefs() == 1u,
			"a native refusal must release the upload's initialization owners");
		CheckReadback(frame, accepted);
		if (error != VK_ERROR_DEVICE_LOST)
		{
			auto replacement = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
			auto retry = RHIFencePtr::Make();
			driver->TrackDelayedInitialization(replacement.GetRawPtr(), retry);
			Require(driver->SubmitCommandList(frame.command, retry), "a rejected upload must be retryable with a fresh fence");
			retry->Wait(5000000000ull);
			Require(retry->IsFinished(), "retried upload must complete on the GPU");
			driver->TrackResources_ThreadSafe();
			Require(replacement->IsReady() && !resource->IsReady(), "retry must not resurrect the failed resource");
			CheckReadback(frame, true);
		}
	}

	template<typename Function>
	void OnRender(Function function)
	{
		auto task = Tasks::CreateTaskWithResult<std::string>("Native frame failure validation", [function = std::move(function)]()
			{
				try { function(); return std::string{}; }
				catch (const std::exception& error) { return std::string(error.what()); }
			}, EThreadType::Render);
		task->Run();
		task->Wait();
		const auto error = task->GetResult();
		if (!error.empty()) throw std::runtime_error(error);
	}

	struct RecordedEditorReadback
	{
		RHICommandListPtr command;
		RHIRenderSubmissionContextPtr context;
		VulkanFencePtr nativeFence;
		bool hasImage = false;
	};

	RecordedEditorReadback RecordEditorReadback(Framegraph::EditorReadbackNode& node,
		glm::ivec2 extent, ETextureFormat format, const void* pixels, size_t size, uint64_t generation = 0u)
	{
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		auto texture = driver.CreateImage_Immediate(pixels, size, glm::ivec3(extent, 1), 1u, ETextureType::Texture2D, format);
		Require(texture.IsValid(), "editor readback needs a real initialized source texture");
		RecordedEditorReadback result;
		uint32_t flight;
		Require(driver.BeginRenderSubmission(flight, result.hasImage), "editor readback must acquire its flight");
		result.nativeFence = VulkanApi::GetInstance()->GetMainDevice()->GetCurrentFrameFence();
		result.context = RHIRenderSubmissionContextPtr::Make();
		result.context->BeginSubmission(1u, flight, 0u, 0u, generation);
		RHISceneViewSnapshot scene;
		scene.m_submissionContext = result.context;
		result.command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
		driver.BeginCommandList(result.command, true);
		node.SetRHIResource("src", texture);
		node.Process({}, {}, result.command, scene);
		driver.EndCommandList(result.command);
		return result;
	}

	FrameSubmissionResult SubmitEditorReadback(RecordedEditorReadback& recorded)
	{
		auto& driver = Renderer::GetDriver();
		auto completion = recorded.context->GetFrameCompletion();
		return recorded.hasImage ? driver->PresentFrame(Sailor::FrameState{}, { recorded.command }, {}, completion) :
			driver->SubmitFrameWithoutPresent({ recorded.command }, {}, completion);
	}

#if defined(__APPLE__)
	uint32_t CheckDeviceIdleObserver(VulkanDevice& device)
	{
		const auto beforeControl = deviceIdleCalls.load();
		auto waitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(dlsym(RTLD_DEFAULT, "vkDeviceWaitIdle"));
		Require(waitIdle && waitIdle(device) == VK_SUCCESS && deviceIdleCalls == beforeControl + 1u,
			"native idle observer must see the real dynamically resolved Vulkan call");
		return deviceIdleCalls.load();
	}

	void TestMetalTextureExport()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		Require(device->IsMetalObjectsSupported(), "native texture export test requires VK_EXT_metal_objects");
		const auto idleCalls = CheckDeviceIdleObserver(*device);
		for (const auto extent : { glm::ivec2(64, 48), glm::ivec2(129, 73), glm::ivec2(1280, 720), glm::ivec2(3840, 2160) })
		{
			const uint32_t color = 0xff432100u | static_cast<uint32_t>(extent.x & 0xff);
			uint32_t flight;
			bool hasImage = false;
			Require(driver.BeginRenderSubmission(flight, hasImage), "native texture export needs an actual renderer flight");
			auto nativeFence = device->GetCurrentFrameFence();
			auto completion = RHIFencePtr::Make();
			auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
			driver.BeginCommandList(command, true);
			auto texture = driver.CreateRenderTarget(command, extent, 1, ETextureFormat::B8G8R8A8_UNORM);
			Require(texture.IsValid(), "native texture export must allocate an actual GPU render target");
			driver.ImageMemoryBarrier(command, texture, EImageLayout::TransferDstOptimal);
			driver.ClearImage(command, texture, glm::vec4(0x43 / 255.0f, 0x21 / 255.0f, (color & 0xffu) / 255.0f, 1.0f));
			driver.ImageMemoryBarrier(command, texture, EImageLayout::General);
			driver.EndCommandList(command);
			uintptr_t exported = 0;
			Require(EditorRemote::ExportMacMetalTextureFromVulkanRenderTarget(*texture, *completion, exported).IsOk() && exported == 0,
				"unsubmitted renderer frame must not export its image");
			const auto submission = hasImage ? driver.PresentFrame(Sailor::FrameState{}, { command }, {}, completion) :
				driver.SubmitFrameWithoutPresent({ command }, {}, completion);
			Require(submission.m_bSubmitted && nativeFence->Wait(5000000000ull) == VK_SUCCESS,
				"native texture fixture must finish the actual renderer submission");
			{
				FenceDispatchOverride dispatch(*device);
				observedFences[0] = *nativeFence;
				const auto waits = allFenceWaitCalls;
				for (VkResult status : { VK_NOT_READY, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					fenceResults[0] = status;
					Require(EditorRemote::ExportMacMetalTextureFromVulkanRenderTarget(*texture, *completion, exported).IsOk() && exported == 0,
						"an unavailable renderer completion must defer export without guessing synchronization");
				}
				Require(allFenceWaitCalls == waits, "export must only poll the frame completion, never wait for a fence");
			}
			const auto begin = std::chrono::steady_clock::now();
			Require(EditorRemote::ExportMacMetalTextureFromVulkanRenderTarget(*texture, *completion, exported).IsOk() && exported != 0,
				"completed renderer frame must export its real Metal texture");
			const auto elapsed = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
			try { Tests::CheckMacVulkanTexturePresentation(exported, extent.x, extent.y, color); }
			catch (...) { EditorRemote::ReleaseMacExportedTexture(exported); throw; }
			EditorRemote::ReleaseMacExportedTexture(exported);
			Require(deviceIdleCalls == idleCalls, "native texture export must not idle the Vulkan device");
			std::cout << "Native Vulkan texture " << extent.x << 'x' << extent.y << ": export " << elapsed << " us, 8 GPU-only presentations\n";
		}
		std::cout << "Native texture export device-idle calls: " << deviceIdleCalls - idleCalls << '\n';
	}

	void TestMetalTextureRetirement()
	{
		auto& driver = *Renderer::GetDriver().DynamicCast<VulkanGraphicsDriver>();
		RHIFencePtr completion;
		auto createSource = [&](glm::ivec2 extent, uint32_t color)
			{
				uint32_t flight;
				bool hasImage = false;
				Require(driver.BeginRenderSubmission(flight, hasImage), "Vulkan retirement fixture needs a renderer flight");
				completion = RHIFencePtr::Make();
				auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
				driver.BeginCommandList(command, true);
				auto texture = driver.CreateRenderTarget(command, extent, 1, ETextureFormat::B8G8R8A8_UNORM);
				driver.ImageMemoryBarrier(command, texture, EImageLayout::TransferDstOptimal);
				driver.ClearImage(command, texture, glm::vec4((color >> 16u & 0xffu) / 255.0f,
					(color >> 8u & 0xffu) / 255.0f, (color & 0xffu) / 255.0f, 1.0f));
				driver.ImageMemoryBarrier(command, texture, EImageLayout::General);
				driver.EndCommandList(command);
				const auto submission = hasImage ? driver.PresentFrame(Sailor::FrameState{}, { command }, {}, completion) :
					driver.SubmitFrameWithoutPresent({ command }, {}, completion);
				Require(submission.m_bSubmitted && completion->Wait(5000000000ull) == EFenceStatus::Finished,
					"Vulkan retirement source must finish its actual GPU clear");
				return texture;
			};
		const glm::ivec2 extents[] = { {64, 48}, {80, 56} };
		const uint32_t colors[] = { 0xff173a6cu, 0xffb25429u };
		RHITexturePtr sources[2];
		EditorRemote::MacRendererFrameSource frames[2];
		const auto idleCalls = CheckDeviceIdleObserver(*VulkanApi::GetInstance()->GetMainDevice());
		try
		{
			for (uint32_t i = 0; i < 2; ++i)
			{
				sources[i] = createSource(extents[i], colors[i]);
				auto& frame = frames[i];
				Require(EditorRemote::ExportMacMetalTextureFromVulkanRenderTarget(*sources[i], *completion, frame.m_textureObject).IsOk() &&
					frame.m_textureObject != 0, "retirement fixture must export the actual completed Vulkan image");
				frame.m_kind = EditorRemote::MacRendererFrameSourceKind::RendererOwnedMetalTexture;
				frame.m_width = extents[i].x;
				frame.m_height = extents[i].y;
				frame.m_pixelFormat = EditorRemote::PixelFormat::B8G8R8A8_UNorm;
			}
			Tests::CheckMacVulkanTextureRetirement(frames[0], colors[0], frames[1], colors[1], [&]()
				{
					for (uint32_t i = 0; i < 16; ++i) createSource(extents[i % 2], 0xffccdd00u | i);
				});
		}
		catch (...)
		{
			for (auto& frame : frames) EditorRemote::ReleaseMacExportedTexture(frame.m_textureObject);
			throw;
		}
		for (auto& frame : frames) EditorRemote::ReleaseMacExportedTexture(frame.m_textureObject);
		Require(deviceIdleCalls == idleCalls, "resize and viewport shutdown must not idle Vulkan for Metal consumption");
		std::cout << "Vulkan-source retirement: delayed copy, resize, viewport shutdown, 32 churn submissions, device-idle calls 0\n";
	}

	void CheckReadbackUpload(const EditorRemote::MacRendererFrameSource& source)
	{
		using namespace EditorRemote;
		class Source final : public IMacRendererFrameSourceProvider
		{
		public:
			MacRendererFrameSource m_frame;
			Failure AcquireFrameSource(const MacViewportSurfaceState&, FrameIndex, MacRendererFrameSource& out) override
			{
				out = m_frame;
				return Failure::Ok();
			}
		} input;
		input.m_frame = source;
		MacLoopbackIOSurfaceProvider provider(&input);
		ViewportDescriptor viewport;
		viewport.m_viewportId = 201;
		viewport.m_width = source.m_width;
		viewport.m_height = source.m_height;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		viewport.m_presentMode = PresentMode::Mailbox;
		MacViewportSurfaceState state;
		Require(provider.CreateOrResizeSurface(viewport, 1, 1, state).IsOk() && provider.BeginFrame(state).IsOk(),
			"completed Vulkan readback must feed a real Metal IOSurface upload");
		FramePacket packet;
		Require(provider.ExportFrame(state, packet).IsOk() &&
			state.m_nativeAllocation->m_lastRendererSource.m_readback == source.m_readback,
			"native upload must accept and retain the actual completed readback owner");
		MacNativeSurfaceFrameEvidence evidence;
		Require(CaptureMacIOSurfaceFrameEvidence(state.m_transport.m_macSurfaces.front(), source.m_width, source.m_height,
			PixelFormat::B8G8R8A8_UNorm, evidence).IsOk(), "uploaded IOSurface must expose its actual pixels");
		const auto* pixel = source.GetCpuBytes() + (source.m_height / 2u) * source.m_bytesPerRow + (source.m_width / 2u) * 4u;
		Require(evidence.m_center.m_b == pixel[0] && evidence.m_center.m_g == pixel[1] &&
			evidence.m_center.m_r == pixel[2] && evidence.m_center.m_a == pixel[3],
			"Metal upload must preserve normalized readback channels and alpha");
		const auto copyToken = state.m_nativeAllocation->m_currentCopyToken;
		for (uint32_t i = 0; i < 100; ++i)
		{
			Require(provider.BeginFrame(state).IsOk() && provider.ExportFrame(state, packet).IsOk(),
				"the same completed GPU readback must remain exportable for presentation retry");
		}
		Require(state.m_nativeAllocation->m_currentCopyToken == copyToken &&
			state.m_nativeAllocation->m_lastProducerCopyToken == copyToken && packet.m_frameIndex == 101,
			"repeated export of an actual Vulkan readback must not upload its pixels again");
		Require(provider.ReleaseSurface(state).IsOk() && provider.GetLiveAllocationCount() == 0,
			"native readback upload fixture must release its registration");
		Tests::CheckMacReadbackPresentation(source);
	}
#endif

	void TestEditorReadback()
	{
		Require(App::HasEditor(), "the readback test must execute the actual editor node path");
		auto node = TRefPtr<Framegraph::EditorReadbackNode>::Make();
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		std::vector<uint32_t> firstPixels(15);
		for (uint32_t i = 0; i < firstPixels.size(); ++i) firstPixels[i] = 0xff123456u + i;
		EditorReadbackFramePtr first;
		OnRender([&]()
			{
				auto recorded = RecordEditorReadback(*node, { 5, 3 }, ETextureFormat::R8G8B8A8_UNORM,
					firstPixels.data(), firstPixels.size() * sizeof(uint32_t));
				const auto completion = recorded.context->GetFrameCompletion();
				Require(completion && completion->GetStatus() == EFenceStatus::Pending && !node->TakeCompletedFrame(),
					"recording must not publish mapped pixels before submission");
				Require(SubmitEditorReadback(recorded).m_bSubmitted, "readback frame must submit");
				Require(recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS, "readback native copy did not finish");
				{
					auto device = VulkanApi::GetInstance()->GetMainDevice();
					FenceDispatchOverride dispatch(*device);
					observedFences[0] = *recorded.nativeFence;
					for (auto status : { VK_NOT_READY, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
					{
						fenceResults[0] = status;
						Require(!node->TakeCompletedFrame(), "pending or failed status queries must not expose pixels");
					}
				}
				first = node->TakeCompletedFrame();
				Require(first && first->m_completion == completion && first->m_extent == glm::ivec2(5, 3) &&
					first->m_bytesPerRow == 20u && first->m_format == ETextureFormat::R8G8B8A8_UNORM && first->m_generation == 0u,
					"completed readback metadata must belong to the same capture");
				Require(std::memcmp(first->m_buffer->GetPointer(), firstPixels.data(), firstPixels.size() * sizeof(uint32_t)) == 0,
					"readback must contain the actual source pixels");
				Require(!node->TakeCompletedFrame(), "one completion must not publish repeatedly");
				renderer->QueueEditorReadback(first);
			});
		EditorRemote::MacRendererFrameSource source;
		Require(!renderer->GetEditorReadback() && !EditorRuntime::TryAcquireEditorReadbackFrameSource(source),
			"Render must not mutate Main's published source before the handoff task runs");
		scheduler->ProcessTasksOnMainThread();
		Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_width == 5u && source.m_height == 3u &&
			source.m_bytesPerRow == 20u && source.m_pixelFormat == EditorRemote::PixelFormat::B8G8R8A8_UNorm &&
			source.m_readback == first && !source.m_cpuBytes, "bridge must retain the completed frame without another payload allocation");
		for (size_t i = 0; i < firstPixels.size(); ++i)
		{
			const auto* rgba = reinterpret_cast<const uint8_t*>(&firstPixels[i]);
			const auto* bgra = source.GetCpuBytes() + 4u * i;
			Require(bgra[0] == rgba[2] && bgra[1] == rgba[1] && bgra[2] == rgba[0] && bgra[3] == rgba[3],
				"bridge must convert RGBA to BGRA without changing alpha");
		}
		const auto retainedSource = source;
#if defined(__APPLE__)
		CheckReadbackUpload(source);
#endif
		Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) &&
			source.GetCpuBytes() == retainedSource.GetCpuBytes(),
			"reacquiring the same completed frame must reuse its owned pixels");
		EditorReadbackStats initialStats;
		OnRender([&]() { initialStats = node->GetStats(); });
		Require(initialStats.m_recordedReadbackBytes == 60u && initialStats.m_convertedBytes == 60u &&
			initialStats.m_bufferAllocatedBytes == 60u && initialStats.m_conversionAllocatedBytes >= 60u,
			"readback cost counters must describe the actual capture and one conversion");
		for (uint32_t i = 0; i < 100; ++i)
		{
			Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.GetCpuBytes() == retainedSource.GetCpuBytes(),
				"unchanged completed frames must not recopy or reallocate pixels");
		}
		OnRender([&]()
			{
				Require(node->GetStats().m_convertedBytes == initialStats.m_convertedBytes &&
					node->GetStats().m_conversionAllocatedBytes == initialStats.m_conversionAllocatedBytes,
					"Main acquisition must not repeat conversion or allocation");
			});
		RecordedEditorReadback pendingResize;
		OnRender([&]()
			{
				const std::vector<uint32_t> pixels(20u, 0xff331155u);
				pendingResize = RecordEditorReadback(*node, { 5, 4 }, ETextureFormat::B8G8R8A8_UNORM,
					pixels.data(), pixels.size() * sizeof(uint32_t));
				Require(!node->TakeCompletedFrame(), "a recorded resize must not replace the previous completed image");
			});
		Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_width == 5u && source.m_height == 3u &&
			renderer->GetEditorReadback() == first, "Main must keep old metadata while the resized frame is pending");
		OnRender([&]()
			{
				Require(SubmitEditorReadback(pendingResize).m_bSubmitted && pendingResize.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"resize copy must complete before simulating delayed notification");
				auto device = VulkanApi::GetInstance()->GetMainDevice();
				FenceDispatchOverride dispatch(*device);
				observedFences[0] = *pendingResize.nativeFence;
				fenceResults[0] = VK_NOT_READY;
				Require(!node->TakeCompletedFrame(), "delayed GPU completion must keep the resized capture unpublished");
				const std::vector<uint32_t> newerPixels(18u, 0xff772299u);
				auto newer = RecordEditorReadback(*node, { 6, 3 }, ETextureFormat::B8G8R8A8_UNORM,
					newerPixels.data(), newerPixels.size() * sizeof(uint32_t));
				Require(SubmitEditorReadback(newer).m_bSubmitted && newer.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"a different free slot must permit progress while completion notification is delayed");
				auto latest = node->TakeCompletedFrame();
				Require(latest && latest->m_extent == glm::ivec2(6, 3) &&
					std::memcmp(latest->m_buffer->GetPointer(), newerPixels.data(), newerPixels.size() * sizeof(uint32_t)) == 0,
					"late completion must not mix the new frame's pixels with earlier dimensions");
				fenceResults[0] = VK_SUCCESS;
				Require(!node->TakeCompletedFrame(), "an older delayed completion must not publish after a newer capture");
			});

		std::vector<EditorReadbackFramePtr> readers{ first };
		const auto capture = [&](uint32_t width, uint64_t generation = 0u)
			{
				EditorReadbackFramePtr frame;
				OnRender([&]()
					{
						const std::vector<uint32_t> pixels(width * 2u, 0xffabc000u + width);
						auto recorded = RecordEditorReadback(*node, { width, 2 }, ETextureFormat::B8G8R8A8_UNORM,
							pixels.data(), pixels.size() * sizeof(uint32_t), generation);
						Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
							"resized readback frame must finish");
						frame = node->TakeCompletedFrame();
						if (frame)
						{
							Require(frame->m_extent == glm::ivec2(width, 2) && frame->m_bytesPerRow == width * 4u &&
								std::memcmp(frame->m_buffer->GetPointer(), pixels.data(), pixels.size() * sizeof(uint32_t)) == 0,
								"resize must keep extent, row stride and pixel contents together");
							renderer->QueueEditorReadback(frame);
						}
						else Require(!recorded.context->GetFrameCompletion(), "full reader ring must skip recording, not overwrite a reader");
					});
				scheduler->ProcessTasksOnMainThread();
				return frame;
			};
		const uint32_t capacity = Renderer::GetDriver()->GetMaxFramesInFlight() + 1u;
		EditorRemote::MacRendererFrameSource retainedBgra;
		for (uint32_t i = 1; i < capacity; ++i)
		{
			auto frame = capture(7u + i);
			Require(frame.IsValid(), "readback ring must permit outstanding readers up to its capacity");
			for (const auto& reader : readers) Require(reader->m_buffer != frame->m_buffer, "held buffers must not be reused");
			readers.push_back(frame);
			if (i == 1)
			{
				Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(retainedBgra) && retainedBgra.m_readback == frame &&
					!retainedBgra.m_cpuBytes && retainedBgra.GetCpuBytes() == frame->m_buffer->GetPointer(),
					"BGRA source must borrow mapped pixels through the immutable frame owner, without copying");
#if defined(__APPLE__)
				CheckReadbackUpload(retainedBgra);
#endif
			}
		}
		Require(!capture(32u) && renderer->GetEditorReadback() == readers.back(),
			"ring pressure must retain the previous presentation without growing buffers");
		const auto releasedBuffer = readers[1]->m_buffer;
		readers[1].Clear();
		Require(!capture(32u) && *reinterpret_cast<const uint32_t*>(retainedBgra.GetCpuBytes()) == 0xffabc008u,
			"a native source owner alone must prevent reuse of its readback slot");
		retainedBgra = {};
		readers[1] = capture(6u);
		Require(readers[1] && readers[1]->m_buffer == releasedBuffer, "released completed slot should reuse its allocation");
		Require(std::memcmp(first->m_buffer->GetPointer(), firstPixels.data(), firstPixels.size() * sizeof(uint32_t)) == 0,
			"old reader pixels must survive resizing and ring reuse");
		const auto latest = renderer->GetEditorReadback();
		readers[2].Clear();
		auto otherGeneration = capture(9u, 1u);
		Require(otherGeneration && renderer->GetEditorReadback() == latest, "stale graph generations must not publish");
		OnRender([&]() { renderer->QueueEditorReadback(first); });
		scheduler->ProcessTasksOnMainThread();
		Require(renderer->GetEditorReadback() == latest, "an older capture must not replace a newer publication");
		otherGeneration.Clear();
		OnRender([&]()
			{
				const std::array<uint16_t, 8> halfPixels{ 0x3c00, 0x3800, 0, 0x3c00, 0, 0, 0x3c00, 0x3800 };
				auto recorded = RecordEditorReadback(*node, { 1, 2 }, ETextureFormat::R16G16B16A16_SFLOAT,
					halfPixels.data(), sizeof(halfPixels));
				Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"half-float readback must finish");
				auto frame = node->TakeCompletedFrame();
				Require(frame && frame->m_bytesPerRow == 8u && frame->m_format == ETextureFormat::R16G16B16A16_SFLOAT,
					"half-float source must use its real eight-byte pixel stride");
				renderer->QueueEditorReadback(frame);
			});
		scheduler->ProcessTasksOnMainThread();
		const std::array<uint8_t, 8> expectedHalf{ 0, 128, 255, 255, 255, 0, 0, 128 };
		Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_bytesPerRow == 4u &&
			std::memcmp(source.GetCpuBytes(), expectedHalf.data(), expectedHalf.size()) == 0,
			"half-float bridge conversion must preserve rows, channel order and alpha");
#if defined(__APPLE__)
		CheckReadbackUpload(source);
#endif
		OnRender([&]() { node->Clear(); });
		Require(std::memcmp(first->m_buffer->GetPointer(), firstPixels.data(), firstPixels.size() * sizeof(uint32_t)) == 0,
			"clearing the graph must not invalidate a retained completed reader");
		Require(std::memcmp(source.GetCpuBytes(), expectedHalf.data(), expectedHalf.size()) == 0,
			"clearing the graph must preserve a retained converted source too");

		EditorReadbackStats warmStats;
		for (uint32_t i = 0; i < 24; ++i)
		{
			OnRender([&]()
				{
					const std::vector<uint32_t> pixels(32u * 24u, 0xff123400u + i);
					auto recorded = RecordEditorReadback(*node, { 32, 24 }, ETextureFormat::R8G8B8A8_UNORM,
						pixels.data(), pixels.size() * sizeof(uint32_t));
					Require(SubmitEditorReadback(recorded).m_bSubmitted && recorded.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
						"reused conversion slot needs actual completed GPU pixels");
					auto frame = node->TakeCompletedFrame();
					Require(frame.IsValid(), "readback must continue while the previous source is retained");
					renderer->QueueEditorReadback(frame);
					if (i == 4) warmStats = node->GetStats();
					if (i > 4)
					{
						const auto& stats = node->GetStats();
						Require(stats.m_bufferAllocatedBytes == warmStats.m_bufferAllocatedBytes &&
							stats.m_conversionAllocatedBytes == warmStats.m_conversionAllocatedBytes &&
							stats.m_convertedBytes - warmStats.m_convertedBytes == (i - 4u) * 32u * 24u * 4u,
							"warm ring must reuse readback and conversion allocations while converting each new frame once");
					}
				});
			scheduler->ProcessTasksOnMainThread();
			Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.GetCpuBytes()[0] == 0x12 &&
				source.GetCpuBytes()[1] == 0x34 && source.GetCpuBytes()[2] == i && source.GetCpuBytes()[3] == 0xff,
				"reused conversion storage must contain the new frame, not stale pixels");
		}
	}

	void TestEditorReadbackRefusal(bool lost)
	{
		Require(App::HasEditor(), "refusal test requires editor readback");
		auto node = TRefPtr<Framegraph::EditorReadbackNode>::Make();
		const uint32_t pixel = 0xffabcdefu;
		OnRender([&]()
			{
				auto recorded = RecordEditorReadback(*node, { 1, 1 }, ETextureFormat::B8G8R8A8_UNORM, &pixel, sizeof(pixel));
				auto completion = recorded.context->GetFrameCompletion();
				SubmitOverride failure(VulkanApi::GetInstance()->GetMainDevice()->GetGraphicsQueue(),
					lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY);
				Require(!SubmitEditorReadback(recorded).m_bSubmitted && completion->HasFailed() && !node->TakeCompletedFrame(),
					"a refused native frame must fail its observer without publishing a readback");
			});
		EditorRemote::MacRendererFrameSource source;
		Require(!EditorRuntime::TryAcquireEditorReadbackFrameSource(source), "bridge must not expose refused frame bytes");
		if (lost) return;
		App::GetSubmodule<Renderer>()->FixLostDevice();
		OnRender([&]()
			{
				auto retry = RecordEditorReadback(*node, { 1, 1 }, ETextureFormat::B8G8R8A8_UNORM, &pixel, sizeof(pixel));
				Require(SubmitEditorReadback(retry).m_bSubmitted && retry.nativeFence->Wait(5000000000ull) == VK_SUCCESS,
					"readback must recover with a new frame completion after device repair");
				auto frame = node->TakeCompletedFrame();
				Require(frame && *static_cast<const uint32_t*>(frame->m_buffer->GetPointer()) == pixel,
					"recovered readback must contain its actual pixels");
			});
	}

	void TestFrameCompletionReuse()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		TVector<RHIFencePtr> completions;
		for (uint32_t i = 0; i < 3u * driver->GetMaxFramesInFlight(); ++i)
		{
			bool hasImage;
			uint32_t flight;
			{
				FenceDispatchOverride dispatch(*device);
				observedFences[0] = *device->GetCurrentFrameFence();
				Require(forwardFenceWait(*device, 1u, observedFences.data(), VK_TRUE, 5000000000ull) == VK_SUCCESS,
					"real flight must finish before synthetic query failure");
				fenceResults[0] = VK_ERROR_OUT_OF_HOST_MEMORY;
				fenceWaitResult = VK_SUCCESS;
				Require(driver->BeginRenderSubmission(flight, hasImage), "flight acquisition must survive a later status-query error");
				if (i >= driver->GetMaxFramesInFlight())
					Require(completions[i - driver->GetMaxFramesInFlight()]->GetStatus() == EFenceStatus::Finished,
						"successful flight wait must latch old observers without relying on another status query");
			}
			auto frame = RecordFrame(50u + i);
			auto completion = RHIFencePtr::Make();
			const auto submitted = hasImage ? driver->PresentFrame(Sailor::FrameState{}, { frame.command }, {}, completion) :
				driver->SubmitFrameWithoutPresent({ frame.command }, {}, completion);
			Require(submitted.m_bSubmitted, "observed frame must submit actual work");
			Require(completion->m_vulkan.m_fence->Wait(5000000000ull) == VK_SUCCESS, "observed native flight must finish");
			CheckReadback(frame, true);
			completions.Add(completion);
			FenceDispatchOverride dispatch(*device);
			observedFences[0] = *completion->m_vulkan.m_fence;
			fenceResults[0] = VK_NOT_READY;
			if (i >= driver->GetMaxFramesInFlight())
				Require(completions[i - driver->GetMaxFramesInFlight()]->IsFinished(),
					"resetting a reused native flight must not make an older observer pending again");
		}
	}

	void TestAcceptedUploadLoss(bool waitForLoss)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		driver->TrackResources_ThreadSafe();
		auto first = RecordFrame(301u);
		auto second = RecordFrame(701u);
		auto shared = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto firstOnly = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto secondOnly = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto firstFence = RHIFencePtr::Make();
		auto secondFence = RHIFencePtr::Make();
		driver->TrackDelayedInitialization(shared.GetRawPtr(), firstFence);
		driver->TrackDelayedInitialization(firstOnly.GetRawPtr(), firstFence);
		driver->TrackDelayedInitialization(shared.GetRawPtr(), secondFence);
		driver->TrackDelayedInitialization(secondOnly.GetRawPtr(), secondFence);
		try
		{
			Require(driver->SubmitCommandList(first.command, firstFence) && driver->SubmitCommandList(second.command, secondFence),
				"both uploads must be accepted before injecting a later fence error");
			// Real completion makes both the injected error and predecessor cleanup safe.
			Require(firstFence->m_vulkan.m_fence->Wait(5000000000ull) == VK_SUCCESS &&
				secondFence->m_vulkan.m_fence->Wait(5000000000ull) == VK_SUCCESS, "native uploads did not finish");
			CheckReadback(first, true);
			CheckReadback(second, true);
			FenceDispatchOverride dispatch(*device);
			observedFences = { *firstFence->m_vulkan.m_fence, *secondFence->m_vulkan.m_fence };
			for (VkResult result : { VK_NOT_READY, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				fenceResults = { result, result };
				driver->TrackResources_ThreadSafe();
				Require(first.command.NumRefs() == 2u && second.command.NumRefs() == 2u &&
					firstFence.NumRefs() == 4u && secondFence.NumRefs() == 4u && shared.NumRefs() == 3u &&
					firstOnly.NumRefs() == 2u && secondOnly.NumRefs() == 2u,
					"pending/error queries must retain every accepted upload owner");
				Require(!firstFence->HasFailed() && !secondFence->HasFailed() && !shared->IsReady(),
					"query OOM is neither upload failure nor proof of GPU completion");
			}
			fenceResults = { VK_NOT_READY, VK_NOT_READY };
			for (VkResult result : { VK_TIMEOUT, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				fenceWaitResult = result;
				Require(firstFence->Wait(0u) == EFenceStatus::Pending && !firstFence->HasFailed() && first.command.NumRefs() == 2u,
					"timeout/wait OOM must leave the upload pending with its recorded dependencies");
			}
			fenceStatusCalls = 0u;
			fenceWaitCalls = 0u;
			if (waitForLoss)
			{
				fenceWaitResult = VK_ERROR_DEVICE_LOST;
				firstFence->Wait(5000000000ull);
				fenceResults[0] = VK_SUCCESS;
			}
			else fenceResults[0] = VK_ERROR_DEVICE_LOST;
			driver->TrackResources_ThreadSafe();
			Require(firstFence->HasFailed() && !firstFence->IsFinished() && firstOnly->HasInitializationFailed(),
				"accepted upload loss was not published as terminal initialization failure");
			Require(first.command.NumRefs() == 1u && firstFence.NumRefs() == 1u && firstOnly.NumRefs() == 1u &&
				shared.NumRefs() == 2u && secondFence.NumRefs() == 4u && second.command.NumRefs() == 2u,
				"loss must retire its own owners without releasing a still-pending second upload");
			Require(!shared->IsReady() && shared->HasInitializationFailed() && !secondOnly->HasInitializationFailed(),
				"shared initialization must remember failure without poisoning pending observers");
			const auto queries = fenceStatusCalls;
			const auto waits = fenceWaitCalls;
			firstFence->Wait(0u);
			Require(!firstFence->Reset(), "a failed fence cannot be reset for reuse");
			Require(firstFence->HasFailed() && !firstFence->IsFinished() &&
				fenceStatusCalls == queries && fenceWaitCalls == waits &&
				forwardFenceStatus(*device, observedFences[0]) == VK_SUCCESS,
				"terminal failure must stay latched without resetting or re-querying its native fence");
			fenceResults[1] = VK_SUCCESS;
			driver->TrackResources_ThreadSafe();
			driver->TrackResources_ThreadSafe();
			Require(secondFence->HasFailed() && !secondFence->IsFinished() && secondOnly->HasInitializationFailed() &&
				!secondOnly->IsReady() && !shared->IsReady(), "a lost device must not publish subsequent completions as ready");
			Require(second.command.NumRefs() == 1u && secondFence.NumRefs() == 1u &&
				shared.NumRefs() == 1u && secondOnly.NumRefs() == 1u, "all completed failure owners must be released exactly once");
			auto retry = RHIFencePtr::Make();
			Require(!driver->SubmitCommandList(first.command, retry) && retry->HasFailed(),
				"loss detected by a fence must stop subsequent native submissions");
			uint32_t flight = 0u;
			bool hasImage = false;
			Require(!device->BeginRenderSubmission(flight, hasImage) &&
				!device->FixLostDevice(App::GetMainWindow().GetRawPtr()),
				"fence-reported device loss must stop new frames and cannot be repaired by recreating the swapchain");
		}
		catch (...)
		{
			device->WaitIdle();
			firstFence->MarkSubmissionFailed();
			secondFence->MarkSubmissionFailed();
			driver->TrackResources_ThreadSafe();
			throw;
		}
	}

	void TestFenceCompletionAndReuse()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		driver->TrackResources_ThreadSafe();
		auto resource = RHITexturePtr::Make(ETextureFiltration::Linear, ETextureClamping::Clamp, false);
		auto fence = RHIFencePtr::Make();
		for (uint32_t round = 0; round < 3u; ++round)
		{
			auto frame = RecordFrame(400u + 300u * round);
			driver->TrackDelayedInitialization(resource.GetRawPtr(), fence);
			Require(!resource->IsReady() && driver->SubmitCommandList(frame.command, fence), "upload must become pending on each reuse");
			Require(fence->m_vulkan.m_fence->Wait(5000000000ull) == VK_SUCCESS, "reused native fence did not complete");
			CheckReadback(frame, true);
			{
				FenceDispatchOverride dispatch(*device);
				observedFences = { *fence->m_vulkan.m_fence, VK_NULL_HANDLE };
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				Require(fence->GetStatus() == EFenceStatus::Pending, "reset must discard cached completion");
				driver->TrackResources_ThreadSafe();
				Require(!resource->IsReady() && frame.command.NumRefs() == 2u, "pending reuse must retain commands and initialization");
				fenceResults[0] = VK_SUCCESS;
				fenceStatusCalls = 0u;
				Require(fence->IsFinished(), "successful poll must complete the fence");
				fenceResults[0] = VK_NOT_READY;
				driver->TrackResources_ThreadSafe();
				driver->TrackResources_ThreadSafe();
				Require(fence->Wait(0u) == EFenceStatus::Finished && fence->IsFinished() && fenceStatusCalls == 1u,
					"all completion consumers must share one terminal result, not re-poll the driver");
				Require(resource->IsReady() && !resource->HasInitializationFailed() && resource.NumRefs() == 1u &&
					fence.NumRefs() == 1u && frame.command.NumRefs() == 1u, "successful completion must release all initialization owners");
			}
			Require(fence->Reset() && fence->m_vulkan.m_fence->Status() == VK_NOT_READY && !fence->IsFinished(),
				"successful reset must clear both native and cached completion");
		}
	}

	void TestImmediateSubmission(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		auto success = RecordFrame(811u);
		const bool completed = driver->SubmitCommandList_Immediate(success.command);
		Require(completed && success.command.NumRefs() == 1u,
			"immediate success must report completion and release its command owner");
		CheckReadback(success, true);
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			if (!lost)
			{
				auto rejected = RecordFrame(911u);
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					const bool accepted = driver->SubmitCommandList_Immediate(rejected.command);
					Require(!accepted && rejected.command.NumRefs() == 1u,
						"immediate refusal must report failure without retaining an unsubmitted command");
				}
				CheckReadback(rejected, false);
				Require(driver->SubmitCommandList_Immediate(rejected.command), "a refused immediate command must remain retryable");
				CheckReadback(rejected, true);
			}
			auto pending = RecordFrame(1011u);
			{
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				fenceWaitCalls = 0u;
				Require(!driver->SubmitCommandList_Immediate(pending.command) && capturedFenceCompleted && fenceWaitCalls == 1u,
					"an unsuccessful immediate wait must reach the caller as failure");
				Require(pending.command.NumRefs() == (lost ? 1u : 2u),
					"immediate failure must retire terminal loss but retain a pending command after wait OOM");
				if (!lost)
				{
					driver->TrackResources_ThreadSafe();
					Require(pending.command.NumRefs() == 2u, "pending immediate ownership must survive repeated collection");
					fenceResults[0] = VK_SUCCESS;
					driver->TrackResources_ThreadSafe();
					Require(pending.command.NumRefs() == 1u, "later completion must release the pending immediate command");
				}
			}
			CheckReadback(pending, true);
			if (lost)
			{
				Require(!driver->SubmitCommandList_Immediate(pending.command), "a lost device must refuse later immediate work");
				break;
			}
		}
	}

	void TestExtendedSubmission(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = static_cast<VulkanGraphicsDriver&>(*Renderer::GetDriver());
		VkProtectedSubmitInfo extension{ VK_STRUCTURE_TYPE_PROTECTED_SUBMIT_INFO };
		extension.protectedSubmit = VK_FALSE;
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			auto frame = RecordFrame(2251u);
			auto fence = RHIFencePtr::Make();
			if (!lost)
			{
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					const bool submitted = driver.SubmitCommandList(frame.command, fence, {}, {}, &extension);
					Require(!submitted, "extended submission must report native refusal");
					Require(lastSubmitNext == &extension, "refused submission must forward its native extension chain");
					Require(fence->HasFailed() && frame.command.NumRefs() == 1u,
						"extended submit refusal must fail its fence and release the command");
				}
				CheckReadback(frame, false);
				fence = RHIFencePtr::Make();
			}
			{
				SubmitOverride observe(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(driver.SubmitCommandList(frame.command, fence, {}, {}, &extension) && lastSubmitNext == &extension,
					"extended submission must preserve its native extension chain");
				Require(fence->Wait() == (lost ? EFenceStatus::Failed : EFenceStatus::Pending) && capturedFenceCompleted,
					"extended fence must report actual wait failure after the real test copy finishes");
				driver.TrackResources_ThreadSafe();
				Require(frame.command.NumRefs() == (lost ? 1u : 2u),
					"extended work must share ordinary command retention and terminal-loss cleanup");
				if (!lost)
				{
					driver.TrackResources_ThreadSafe();
					Require(frame.command.NumRefs() == 2u, "pending extended submission must survive collection");
					fenceResults[0] = VK_SUCCESS;
					driver.TrackResources_ThreadSafe();
					Require(fence->IsFinished() && frame.command.NumRefs() == 1u, "extended completion must release command ownership");
				}
			}
			CheckReadback(frame, true);
			if (lost) break;
		}
	}

#if defined(_WIN32)
	void TestWindowsSharedSurfaceCopy(bool lost)
	{
		using namespace Sailor::EditorRemote;
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		SailorWindowsSharedSurfaceProvider provider;
		WindowsViewportTransportBackend backend(provider);
		SailorWindowsViewportPresenter presenter;
		ViewportDescriptor viewport;
		viewport.m_viewportId = 96u;
		viewport.m_width = viewport.m_height = 16u;
		viewport.m_pixelFormat = PixelFormat::B8G8R8A8_UNorm;
		viewport.m_colorSpace = ColorSpace::Srgb;
		TransportDescriptor transport;
		Require(backend.EnsureSurface(viewport, 1u, 1u, transport).IsOk(), "native Windows shared surface creation failed");
		Require(presenter.ImportSurface(viewport, transport, 1u, 1u).IsOk(), "native D3D presenter import failed");
		FrameIndex exported = 0u;
		auto exportAndRelease = [&]()
			{
				FramePacket frame;
				Require(backend.ExportFrame(viewport, 1u, 1u, frame).IsOk() && frame.m_frameIndex == ++exported &&
					frame.m_sync.m_acquireValue == exported * 2u - 1u && frame.m_sync.m_releaseValue == exported * 2u &&
					frame.m_sync.m_crossApiCpuWaited, "completed Windows copy must export exactly the accepted keyed-mutex frame");
				if (exported == 1u)
				{
					auto unavailable = frame;
					unavailable.m_sync.m_acquireValue += 2u;
					const auto result = presenter.PresentFrame(viewport.m_viewportId, unavailable);
					Require(!result.IsOk() && result.m_nativeCode == WAIT_TIMEOUT,
						"an unavailable mutex key must report timeout without consuming the ready frame");
				}
				Require(presenter.PresentFrame(viewport.m_viewportId, frame).IsOk(), "D3D must acquire and release the completed frame");
				Require(!backend.ExportFrame(viewport, 1u, 1u, frame).IsOk(), "completed copy must export only once");
			};
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			if (!lost)
			{
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
					Require(!backend.BeginFrame(viewport, 1u, 1u).IsOk(), "Windows begin must report submit refusal");
				}
				Require(backend.BeginFrame(viewport, 1u, 1u).IsOk(), "refused Windows copy must retry successfully");
				exportAndRelease();
			}
			{
				SubmitOverride observe(VulkanSubmissionTestAccess::UploadQueue(*device), VK_SUCCESS);
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(!backend.BeginFrame(viewport, 1u, 1u).IsOk() && capturedFenceCompleted,
					"Windows begin must report an accepted wait failure");
				const auto submitted = submitCalls;
				FramePacket frame;
				auto state = *std::as_const(backend).FindSurface(viewport.m_viewportId, 1u, 1u);
				Require(!provider.ExportFrame(state, frame).IsOk() && !backend.ExportFrame(viewport, 1u, 1u, frame).IsOk(),
					"neither provider nor backend may publish an incomplete Windows copy");
				Require(!backend.BeginFrame(viewport, 1u, 1u).IsOk() && submitCalls == submitted,
					"pending or lost Windows copy must not resubmit using its previous mutex key");
				if (!lost)
				{
					fenceWaitResult = VK_SUCCESS;
					fenceResults[0] = VK_SUCCESS;
					Require(backend.BeginFrame(viewport, 1u, 1u).IsOk() && submitCalls == submitted,
						"Windows retry must finish the original accepted copy");
					exportAndRelease();
				}
			}
			if (lost) break;
		}
		presenter.ResetViewport(viewport.m_viewportId);
		Require(backend.ReleaseSurface(viewport.m_viewportId, 1u, 1u).IsOk(), "Windows surface release failed");
		if (!lost)
		{
			Require(backend.EnsureSurface(viewport, 1u, 2u, transport).IsOk(), "replacement Windows surface creation failed");
			FenceDispatchOverride dispatch(*device);
			fenceResults = { VK_NOT_READY, VK_NOT_READY };
			fenceWaitResult = VK_ERROR_OUT_OF_HOST_MEMORY;
			captureNextFenceWait = true;
			capturedFenceCompleted = false;
			Require(!backend.BeginFrame(viewport, 1u, 2u).IsOk() && capturedFenceCompleted, "replacement copy must enter pending state");
			Require(backend.ReleaseSurface(viewport.m_viewportId, 1u, 2u).IsOk(), "pending surface must be releasable");
			Require(forwardFenceStatus(*device, observedFences[0]) == VK_SUCCESS,
				"releasing the allocation must not destroy the tracked native fence");
			fenceResults[0] = VK_SUCCESS;
			driver.TrackResources_ThreadSafe();
		}
	}
#endif

	void TestImmediateBindingUpdate(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		auto commands = Renderer::GetDriverCommands();
		std::array<uint32_t, 64> data{};
		for (uint32_t i = 0; i < data.size(); ++i) data[i] = 171u + 43u * i;
		auto buffer = driver->CreateBuffer(sizeof(data), EBufferUsageBit::UniformBuffer_Bit |
			EBufferUsageBit::BufferTransferSrc_Bit | EBufferUsageBit::BufferTransferDst_Bit, EMemoryPropertyBit::DeviceLocal);
		auto bindings = driver->CreateShaderBindings();
		driver->AddBufferToShaderBindings(bindings, buffer, "immediate", 0u);
		auto checkData = [&]()
			{
				auto readback = driver->CreateBuffer(sizeof(data), EBufferUsageBit::BufferTransferDst_Bit,
					EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
				auto command = driver->CreateCommandList(false, ECommandListQueue::Transfer);
				commands->BeginCommandList(command, true);
				auto& native = command->m_vulkan.m_commandBuffer;
				native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
				native->CopyBuffer(*buffer->m_vulkan.m_buffer->Get(), *readback->m_vulkan.m_buffer->Get(), sizeof(data));
				native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
				commands->EndCommandList(command);
				Require(driver->SubmitCommandList_Immediate(command), "binding readback did not complete");
				const auto* actual = static_cast<const uint32_t*>(readback->GetPointer());
				Require(std::equal(data.begin(), data.end(), actual), "immediate binding contents mismatch");
			};
		Require(driver->UpdateShaderBinding_Immediate(bindings, "immediate", data.data(), sizeof(data)),
			"successful binding upload must report completion");
		checkData();
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			if (!lost)
			{
				auto refused = data;
				refused.fill(0xdeadbeefu);
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device, true), error);
					Require(!driver->UpdateShaderBinding_Immediate(bindings, "immediate", refused.data(), sizeof(refused)),
						"binding update must propagate submit refusal");
				}
				checkData();
			}
			for (auto& value : data) value += 121u;
			{
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(!driver->UpdateShaderBinding_Immediate(bindings, "immediate", data.data(), sizeof(data)) && capturedFenceCompleted,
					"binding update must propagate accepted wait failure");
				if (!lost)
				{
					fenceResults[0] = VK_SUCCESS;
					driver->TrackResources_ThreadSafe();
				}
			}
			if (lost) break;
			checkData();
		}
	}

	void TestImmediateBufferCreation(bool lost = false)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		std::array<uint32_t, 64> data{};
		data.fill(0x17bac012u);
		FenceDispatchOverride dispatch(*device);
		fenceResults = { VK_NOT_READY, VK_NOT_READY };
		fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
		captureNextFenceWait = true;
		capturedFenceCompleted = false;
		auto buffer = driver->CreateBuffer_Immediate(data.data(), sizeof(data), EBufferUsageBit::StorageBuffer_Bit);
		fenceResults[0] = VK_SUCCESS;
		driver->TrackResources_ThreadSafe();
		Require(capturedFenceCompleted, "buffer creation fixture must complete native work before injecting wait failure");
		Require(!buffer, "an incomplete immediate upload must not publish a usable buffer");
	}

	void TestImmediateImageCreation(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		std::array<uint32_t, 16> data{};
		for (uint32_t i = 0; i < data.size(); ++i) data[i] = 0xff100000u + 739u * i;
		FenceDispatchOverride dispatch(*device);
		fenceResults = { VK_NOT_READY, VK_NOT_READY };
		fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_HOST_MEMORY;
		captureNextFenceWait = true;
		capturedFenceCompleted = false;
		auto texture = driver->CreateImage_Immediate(data.data(), sizeof(data), glm::ivec3(4, 4, 1));
		fenceResults[0] = VK_SUCCESS;
		driver->TrackResources_ThreadSafe();
		Require(capturedFenceCompleted, "image fixture must finish native work before injecting wait failure");
		Require(!texture, "an incomplete immediate upload must not publish a usable texture");
	}

	std::vector<uint32_t> ReadImage(IGraphicsDriver& driver, VulkanImagePtr image, uint32_t mip = 0u, uint32_t layer = 0u)
	{
		const uint32_t width = std::max(1u, image->m_extent.width >> mip);
		const uint32_t height = std::max(1u, image->m_extent.height >> mip);
		const uint32_t depth = std::max(1u, image->m_extent.depth >> mip);
		std::vector<uint32_t> result(width * height * depth);
		auto buffer = driver.CreateBuffer(result.size() * sizeof(uint32_t), EBufferUsageBit::BufferTransferDst_Bit,
			EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
		auto command = driver.CreateCommandList(false, ECommandListQueue::Graphics);
		auto& native = command->m_vulkan.m_commandBuffer;
		native->BeginCommandList(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
		native->ImageMemoryBarrier(image, image->m_format, image->m_defaultLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		native->CopyImageToBuffer(*buffer->m_vulkan.m_buffer->Get(), image, width, height, depth, mip, layer);
		native->ImageMemoryBarrier(image, image->m_format, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image->m_defaultLayout);
		native->MemoryBarrier(VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT);
		native->EndCommandList();
		Require(driver.SubmitCommandList_Immediate(command), "image readback did not complete");
		std::copy_n(static_cast<const uint32_t*>(buffer->GetPointer()), result.size(), result.begin());
		return result;
	}

	void TestImmediateImageContents()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = *Renderer::GetDriver();
		std::array<uint32_t, 16> data{};
		for (uint32_t i = 0; i < data.size(); ++i) data[i] = 0xff102030u + 137u * i;
		auto create = [&]() { return driver.CreateImage_Immediate(data.data(), sizeof(data), glm::ivec3(4, 4, 1), 1u,
			ETextureType::Texture2D, ETextureFormat::R8G8B8A8_UNORM, ETextureFiltration::Nearest, ETextureClamping::Repeat); };
		auto verify = [&]()
			{
				auto image = create();
				Require(image && image->IsReady() && image->m_vulkan.m_imageView, "completed upload must publish a ready texture and view");
				Require(image->GetFiltration() == ETextureFiltration::Nearest && image->GetClamping() == ETextureClamping::Repeat &&
					image->GetFormat() == ETextureFormat::R8G8B8A8_UNORM && !image->HasMipMaps() &&
					image->GetDefaultLayout() == EImageLayout::ShaderReadOnlyOptimal, "immediate upload changed texture sampling properties");
				const auto readback = ReadImage(driver, image->m_vulkan.m_image);
				Require(std::equal(data.begin(), data.end(), readback.begin()), "2D upload changed source pixels");
			};
		verify();
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			{
				SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device), error);
				Require(!create(), "refused image upload must not publish a texture");
			}
			verify();
			{
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(!create() && capturedFenceCompleted, "image upload must propagate accepted wait failure");
				driver.TrackResources_ThreadSafe();
				fenceResults[0] = VK_SUCCESS;
				driver.TrackResources_ThreadSafe();
			}
			verify();
		}

		std::array<uint32_t, 96> faces{};
		for (uint32_t face = 0; face < 6u; ++face)
			std::fill_n(faces.begin() + 16u * face, 16u, 0xff123420u + 17u * face);
		auto cubemap = driver.CreateImage_Immediate(faces.data(), sizeof(faces), glm::ivec3(4, 4, 1), 3u,
			ETextureType::Cubemap, ETextureFormat::R8G8B8A8_UNORM);
		Require(cubemap && cubemap->m_vulkan.m_image->m_arrayLayers == 6u && cubemap->m_vulkan.m_image->m_mipLevels == 3u &&
			cubemap->HasMipMaps() && cubemap->m_vulkan.m_imageView->m_viewType == VK_IMAGE_VIEW_TYPE_CUBE,
			"cubemap upload lost its layers, mip chain or cube view");
		for (uint32_t mip = 0; mip < 3u; ++mip)
		{
			for (uint32_t face = 0; face < 6u; ++face)
			{
				const auto readback = ReadImage(driver, cubemap->m_vulkan.m_image, mip, face);
				Require(std::all_of(readback.begin(), readback.end(), [&](uint32_t value) { return value == faces[16u * face]; }),
					"cubemap face or generated mip contains incorrect pixels");
			}
		}
		auto volume = driver.CreateImage_Immediate(data.data(), 8u * sizeof(uint32_t), glm::ivec3(2), 1u,
			ETextureType::Texture3D, ETextureFormat::R8G8B8A8_UNORM);
		Require(volume && volume->m_vulkan.m_imageView->m_viewType == VK_IMAGE_VIEW_TYPE_3D, "volume upload lost its 3D view");
		const auto volumeData = ReadImage(driver, volume->m_vulkan.m_image);
		Require(std::equal(data.begin(), data.begin() + 8u, volumeData.begin()), "volume upload changed source voxels");
	}

	class BootstrapDriver final : public VulkanGraphicsDriver
	{
	public:
		BootstrapDriver(uint32_t failureCall, VkResult error, bool waitFailure) :
			m_failureCall(failureCall), m_error(error), m_waitFailure(waitFailure) {}

		RHITexturePtr CreateImage_Immediate(const void* data, size_t size, glm::ivec3 extent, uint32_t mipLevels,
			ETextureType type, ETextureFormat format, ETextureFiltration filtration,
			ETextureClamping clamping, ETextureUsageFlags usage) override
		{
			if (++imageCalls == m_failureCall)
			{
				auto device = VulkanApi::GetInstance()->GetMainDevice();
				if (m_waitFailure)
				{
					m_wait = TUniquePtr<FenceDispatchOverride>::Make(*device);
					fenceResults = { VK_NOT_READY, VK_NOT_READY };
					fenceWaitResult = m_error;
					captureNextFenceWait = true;
					capturedFenceCompleted = false;
				}
				else m_submit = TUniquePtr<SubmitOverride>::Make(VulkanSubmissionTestAccess::UploadQueue(*device), m_error);
			}
			return VulkanGraphicsDriver::CreateImage_Immediate(data, size, extent, mipLevels, type, format, filtration, clamping, usage);
		}

		bool SubmitCommandList(RHICommandListPtr command, RHIFencePtr fence, RHISemaphorePtr signal, RHISemaphorePtr wait) override
		{
			lastCommand = command;
			return VulkanGraphicsDriver::SubmitCommandList(command, fence, signal, wait);
		}

		bool HasAnyDefault() const { return m_defaultTexture || m_vkDefaultTexture || m_vkDefaultCubemap; }
		VulkanImagePtr DefaultCubemap() const { return m_vkDefaultCubemap ? m_vkDefaultCubemap->GetImage() : nullptr; }
		size_t PendingCount() const { return m_trackedFences.Num(); }
		void ResetFailure() { m_wait.Clear(); m_submit.Clear(); }

		uint32_t imageCalls = 0u;
		RHICommandListPtr lastCommand;

	private:
		uint32_t m_failureCall;
		VkResult m_error;
		bool m_waitFailure;
		TUniquePtr<FenceDispatchOverride> m_wait;
		TUniquePtr<SubmitOverride> m_submit;
	};

	int RunBootstrapGpu(int argc, const char** argv, bool waitFailure, bool lost)
	{
		int result = 1;
		try
		{
			// Stop App bootstrap before it creates a renderer; the driver still gets
			// its normal window and scheduler dependencies, with no Renderer singleton.
			const auto missingManifest = (std::filesystem::temp_directory_path() / "sailor-bootstrap-missing-manifest.yaml").string();
			Require(!std::filesystem::exists(missingManifest), "bootstrap fixture needs an absent manifest");
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--workspace-manifest", missingManifest.c_str() });
			App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size()));
			Require(App::GetInstance() && !App::GetSubmodule<Renderer>() && !VulkanApi::GetInstance(),
				"bootstrap fixture must start without an existing renderer or Vulkan instance");
			App::AddSubmodule(TSubmodule<Tasks::Scheduler>::Make())->Initialize();
			auto& window = App::GetMainWindow();
			window = TUniquePtr<Win32::Window>::Make();
			window->Create("VulkanBootstrapTests", "VulkanBootstrapTests", 320, 240, false, false, 0);
			window->Show(false);

			for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				for (uint32_t failureCall : { 1u, 2u })
				{
					BootstrapDriver driver(failureCall, lost ? VK_ERROR_DEVICE_LOST : error, waitFailure);
					driver.Initialize(window.GetRawPtr(), EMsaaSamples::Samples_1, false);
					Require(driver.imageCalls == failureCall && !driver.IsInitialized() && !driver.HasAnyDefault(),
						"failed bootstrap must stop at the failed upload without publishing either fallback");
					const bool pending = waitFailure && !lost;
					Require(driver.PendingCount() == (pending ? 1u : 0u) && driver.lastCommand.NumRefs() == (pending ? 2u : 1u),
						"bootstrap must retain accepted pending commands but release refused or lost work");
					if (waitFailure) Require(capturedFenceCompleted, "bootstrap failure fixture must finish its real GPU upload");
					if (pending)
					{
						driver.TrackResources_ThreadSafe();
						Require(driver.PendingCount() == 1u && driver.lastCommand.NumRefs() == 2u,
							"partial initialization must retain the upload across resource collection");
						{
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							QueueWaitOverride refusal(device->GetGraphicsQueue(), error);
							Require(!driver.BeginConditionalDestroy() && driver.PendingCount() == 1u && driver.lastCommand.NumRefs() == 2u,
								"failed partial shutdown must retain the original pending upload");
						}
					}
					driver.ResetFailure();
					Require(driver.BeginConditionalDestroy() && driver.PendingCount() == 0u && driver.lastCommand.NumRefs() == 1u,
						"completed partial shutdown must release the tracked upload");
					driver.lastCommand.Clear();
				}
				if (lost) break;
			}
			Require(!VulkanApi::GetInstance(), "failed bootstrap destruction must release the Vulkan instance");
			{
				BootstrapDriver driver(0u, VK_SUCCESS, false);
				driver.Initialize(window.GetRawPtr(), EMsaaSamples::Samples_1, false);
				Require(driver.IsInitialized() && driver.imageCalls == 2u && driver.GetDefaultTexture() && driver.DefaultCubemap(),
					"fresh bootstrap must publish both fallback resources together");
				Require(ReadImage(driver, driver.GetDefaultTexture()->m_vulkan.m_image) == std::vector<uint32_t>{ 0x00e567ffu },
					"fallback texture color changed");
				for (uint32_t face = 0; face < 6u; ++face)
					Require(ReadImage(driver, driver.DefaultCubemap(), 0u, face) == std::vector<uint32_t>{ 0x00e567ffu },
						"fallback cubemap face color changed");
				driver.lastCommand.Clear();
				Require(driver.BeginConditionalDestroy(), "successful bootstrap did not drain");
			}
			Require(App::Shutdown(), "bootstrap harness did not shut down");
			App::Initialize(argv, argc);
			Require(App::IsRendererInitialized(), "normal App must initialize after failed backend bootstraps");
			std::cout << "Native fallback bootstrap test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}

	void TestImmediateBufferCopy(bool lost)
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		std::array<uint32_t, 64> data{};
		for (uint32_t i = 0u; i < data.size(); ++i) data[i] = 719u + 97u * i;
		auto createSource = [&]()
			{
				return driver->CreateBuffer_Immediate(data.data(), sizeof(data), EBufferUsageBit::BufferTransferSrc_Bit);
			};
		auto createDestination = [&]()
			{
				auto result = driver->CreateBuffer(sizeof(data), EBufferUsageBit::BufferTransferDst_Bit,
					EMemoryPropertyBit::HostVisible | EMemoryPropertyBit::HostCoherent);
				std::fill_n(static_cast<uint32_t*>(result->GetPointer()), data.size(), 0xdeadbeefu);
				return result;
			};
		auto source = createSource();
		auto destination = createDestination();
		Require(source.IsValid(), "successful immediate creation must publish a buffer");
		Require(driver->CopyBuffer_Immediate(source, destination, 31u * sizeof(uint32_t), 7u * sizeof(uint32_t), 11u * sizeof(uint32_t)),
			"immediate subrange copy did not complete");
		const auto* copied = static_cast<const uint32_t*>(destination->GetPointer());
		for (uint32_t i = 0u; i < data.size(); ++i)
			Require(copied[i] == (i >= 11u && i < 42u ? data[i - 11u + 7u] : 0xdeadbeefu),
				"immediate copy must preserve offsets and untouched neighboring bytes");
		Require(driver->CopyBuffer_Immediate(source, destination, sizeof(data)), "immediate full copy did not complete");
		Require(std::equal(data.begin(), data.end(), copied), "immediate create/copy data mismatch");
		source.Clear();
		destination.Clear();

		using BufferOwner = Memory::TManagedMemory<Memory::VulkanBufferMemoryPtr, RHIBuffer::VulkanBufferAllocator>;
		for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
		{
			if (!lost)
			{
				auto refusedDestination = createDestination();
				auto refusedSource = createSource();
				{
					SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device, true), error);
					Require(!createSource(), "rejected immediate creation must not publish a buffer");
					Require(!driver->CopyBuffer_Immediate(refusedSource, refusedDestination, sizeof(data)),
						"immediate copy must propagate submission refusal");
				}
				const auto* untouched = static_cast<const uint32_t*>(refusedDestination->GetPointer());
				Require(std::all_of(untouched, untouched + data.size(), [](uint32_t value) { return value == 0xdeadbeefu; }),
					"rejected copy changed its destination");
				Require(driver->CopyBuffer_Immediate(refusedSource, refusedDestination, sizeof(data)), "copy retry did not complete");
				Require(std::equal(data.begin(), data.end(), untouched), "copy retry data mismatch");
			}
			auto pendingSource = createSource();
			auto pendingDestination = createDestination();
			TWeakPtr<BufferOwner> sourceOwner(pendingSource->m_vulkan.m_buffer);
			TWeakPtr<BufferOwner> destinationOwner(pendingDestination->m_vulkan.m_buffer);
			{
				FenceDispatchOverride dispatch(*device);
				fenceResults = { VK_NOT_READY, VK_NOT_READY };
				fenceWaitResult = lost ? VK_ERROR_DEVICE_LOST : error;
				captureNextFenceWait = true;
				capturedFenceCompleted = false;
				Require(!driver->CopyBuffer_Immediate(pendingSource, pendingDestination, sizeof(data)) && capturedFenceCompleted,
					"immediate copy must report an accepted wait failure");
				Require(std::equal(data.begin(), data.end(), static_cast<const uint32_t*>(pendingDestination->GetPointer())),
					"the fault fixture must finish its real copy before simulating loss");
				pendingSource.Clear();
				pendingDestination.Clear();
				driver->TrackResources_ThreadSafe();
				Require(static_cast<bool>(sourceOwner.TryLock()) == !lost && static_cast<bool>(destinationOwner.TryLock()) == !lost,
					"pending copies must retain original managed allocations; terminal loss must release them");
				if (!lost)
				{
					fenceResults[0] = VK_SUCCESS;
					driver->TrackResources_ThreadSafe();
					Require(!sourceOwner.TryLock() && !destinationOwner.TryLock(), "completed copies must release both allocation owners");
				}
			}
			if (lost) break;
		}
	}

	struct ParticleBufferNode : Framegraph::Experimental::ParticlesNode
	{
		using ParticlesNode::InitializeBuffers;
		using ParticlesNode::m_instances;
		using ParticlesNode::m_particlesFrames;
		using ParticlesNode::m_particlesDataBinary;
		using ParticlesNode::m_perInstanceData;
		using ParticlesNode::m_numInstances;
	};

	void TestParticleBufferPublication()
	{
		auto device = VulkanApi::GetInstance()->GetMainDevice();
		auto& driver = Renderer::GetDriver();
		TVector<ParticleBufferNode::PerInstanceData> instances(3u);
		for (auto& instance : instances)
		{
			instance = {};
			instance.model = glm::mat4(1.0f);
			instance.color = glm::vec4(1.0f);
		}
		for (uint32_t failedUpload : { 0u, 1u })
		{
			for (bool waitFailure : { false, true })
			{
				for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					ParticleBufferNode node;
					node.m_particlesDataBinary.Resize(3u);
					for (auto& particle : node.m_particlesDataBinary) particle = {};
					node.m_particlesDataBinary[1].m_x2 = 123.5f;
					if (waitFailure)
					{
						FenceDispatchOverride dispatch(*device);
						fenceResults = { VK_NOT_READY, VK_NOT_READY };
						fenceWaitResult = error;
						waitsBeforeCapture = failedUpload;
						captureNextFenceWait = true;
						capturedFenceCompleted = false;
						Require(!node.InitializeBuffers(instances) && capturedFenceCompleted,
							"particle initialization must propagate either upload's wait failure");
						fenceResults[0] = VK_SUCCESS;
						driver->TrackResources_ThreadSafe();
					}
					else
					{
						SubmitOverride refusal(VulkanSubmissionTestAccess::UploadQueue(*device, true), error, false, failedUpload);
						Require(!node.InitializeBuffers(instances), "particle initialization must propagate either upload's submission refusal");
					}
					Require(!node.m_instances && !node.m_particlesFrames && !node.m_perInstanceData && node.m_numInstances == 0u &&
						node.m_particlesDataBinary.Num() == 3u && node.m_particlesDataBinary[1].m_x2 == 123.5f,
						"partial particle initialization must publish nothing and retain its CPU source for retry");
					Require(node.InitializeBuffers(instances), "particle initialization retry did not complete");
					Require(node.m_instances && node.m_particlesFrames && node.m_perInstanceData && node.m_numInstances == 3u &&
						node.m_particlesDataBinary.IsEmpty(), "successful particle initialization must publish both buffers and consume its CPU source");
					Require(node.m_perInstanceData->GetOrAddShaderBinding("data")->m_vulkan.m_valueBinding == node.m_instances->m_vulkan.m_buffer &&
						node.m_perInstanceData->GetOrAddShaderBinding("particlesData")->m_vulkan.m_valueBinding == node.m_particlesFrames->m_vulkan.m_buffer,
						"particle bindings must retain the published buffers' original allocations");
				}
			}
		}
	}

	int RunInitializationGpu(int argc, const char** argv)
	{
		Tests::TempDirectory workspace("host-initialization");
		int result = 1;
		try
		{
			std::filesystem::create_directory(workspace.Path("Content"));
			std::string enginePath = std::filesystem::current_path().string();
			for (int i = 1; i + 1 < argc; ++i)
				if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
			YAML::Node manifest;
			manifest["manifestVersion"] = 1;
			manifest["workspaceId"] = "00000000-0000-0000-0000-000000000120";
			manifest["name"] = "Initialization test";
			manifest["enginePath"] = enginePath;
			manifest["engineReferenceKind"] = "source";
			manifest["contentPath"] = "Content";
			manifest["sourcePath"] = "Source";
			manifest["generatedProjectPath"] = "Generated";
			manifest["cachePath"] = "Cache";
			manifest["buildPath"] = "Cache/Build";
			manifest["logicOutputPath"] = "Binaries";
			manifest["logicModuleName"] = "MissingInitializationModule";
			std::ofstream(workspace.Path("workspace.sailor")) << manifest;
			std::filesystem::copy_file(std::filesystem::path(enginePath) / "Content/Models/DuckGlb/Duck.glb",
				workspace.Path("Content/Test.glb"));
			YAML::Node model;
			model["assetInfoType"] = "Sailor::ModelAssetInfo";
			model["fileId"] = "{00000000-0000-0000-0000-000000000120}";
			model["filename"] = "Test.glb";
			model["bShouldGenerateMaterials"] = false;
			model["bShouldKeepCpuBuffers"] = true;
			model["bGenerateBLAS"] = true;
			model["unitScale"] = 1;
			std::ofstream(workspace.Path("Content/Test.glb.asset")) << model;
			const std::string workspacePath = workspace.Get().string();
			const std::string output = workspace.Path("command.png").string();
			std::vector<const char*> commandArguments(argv, argv + argc);
			commandArguments.insert(commandArguments.end(), { "--workspace", workspacePath.c_str(), "--editor", "--port", "0",
				"--pathtracer", "--in", "Test.glb",
				"--out", output.c_str(), "--height", "8", "--samples", "1", "--ambientSamples", "1", "--bounces", "1" });
			Require(App::Initialize(commandArguments.data(), static_cast<int32_t>(commandArguments.size())) ==
				EAppInitializationResult::Completed && std::filesystem::is_regular_file(output) &&
				std::filesystem::file_size(output) > 32u, "an offline command must complete without claiming an interactive session");
			App::Start();
			Require(!App::GetSubmodule<EngineLoop>(), "a completed command must not create or enter a game loop");
			Require(App::Shutdown() && !App::GetInstance(), "a completed command must release its App before another startup");

			const char* missingWorld = "Tests/Visual/MissingRequiredStartup.world";
			std::vector<const char*> arguments(argv, argv + argc);
			arguments.insert(arguments.end(), { "--world", missingWorld });
			const auto initialization = App::Initialize(arguments.data(), static_cast<int32_t>(arguments.size()));
			Require(initialization == EAppInitializationResult::Failed && App::IsRendererInitialized() && App::GetExitCode() != 0,
				"a missing required startup world must fail even after the renderer has initialized");
			App::Start();
			Require(App::GetSubmodule<EngineLoop>()->GetWorlds().IsEmpty() &&
				App::Initialize() == EAppInitializationResult::Failed, "failed startup must not create a fallback game world or become Ready");
			Require(App::Shutdown() && !App::GetInstance(), "failed startup must release its GPU and App resources");

			std::string initialize;
			for (int i = 0; i < argc; ++i) Tests::ProtocolWire::AppendBytesField(initialize, 1u, argv[i]);
			for (const auto& argument : { std::string("--workspace"), workspace.Get().string(),
				std::string("--new-world"), std::string("--port"), std::string("0") })
			{
				Tests::ProtocolWire::AppendBytesField(initialize, 1u, argument);
			}
			Require(ix::initNetSystem(), "port reservation must initialize the network system");
			const int port = ix::getFreePort();
			Require(ix::uninitNetSystem() && port > 0 && port <= 65535, "port reservation must complete");
			const std::string token = "0123456789abcdef0123456789abcdef";
			auto startHost = [&]()
				{
					const std::string payload = Tests::ProtocolWire::MakeRequest(1u, 10u, initialize);
					return static_cast<Protocol::EEditorEngineWebSocketHostStatus>(SailorProtocolStartLocalHost(
						reinterpret_cast<const uint8_t*>(payload.data()), static_cast<uint32_t>(payload.size()),
						static_cast<uint16_t>(port), token.data(), static_cast<uint32_t>(token.size())));
				};
			Require(startHost() == Protocol::EEditorEngineWebSocketHostStatus::InitializationFailed && !App::GetInstance(),
				"a game session without its required module must fail and roll back its native host");

			Tests::ProtocolWire::AppendBytesField(initialize, 1u, "--editor");
			for (uint32_t attempt = 0u; attempt < 2u; ++attempt)
			{
				Require(startHost() == Protocol::EEditorEngineWebSocketHostStatus::Ok,
					"the editor must start without its workspace module, including after shutdown and retry");
				auto* loop = App::GetSubmodule<EngineLoop>();
				Require(App::IsRendererInitialized() && App::HasEditor() && loop && loop->GetWorlds().Num() == 1u &&
					App::GetLoadedWorldPath().empty(), "limited editor mode must still own a renderer and editable empty world");
				Require(App::Initialize() == EAppInitializationResult::Ready,
					"an already initialized editor must retain its successful result");
				App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker,
					EThreadType::RHI, EThreadType::Render, EThreadType::Editor });

				SailorProtocolRequestLocalHostStop();
				const std::string payload = Tests::ProtocolWire::MakeRequest(2u, 11u);
				uint8_t* responseData = nullptr;
				uint32_t responseSize = 0u;
				const auto status = SailorProtocolInvoke(reinterpret_cast<const uint8_t*>(payload.data()),
					static_cast<uint32_t>(payload.size()), &responseData, &responseSize);
				std::string responsePayload;
				if (responseData) responsePayload.assign(reinterpret_cast<const char*>(responseData), responseSize);
				SailorProtocolFreeBuffer(responseData);
				Tests::ProtocolWire::TProtocolResponseWire response;
				Require(status == static_cast<int32_t>(Protocol::EEditorEngineTransportStatus::Ok) &&
					Tests::ProtocolWire::ParseResponse(responsePayload, response) &&
					response.m_protocolVersion == Protocol::EditorEngineProtocolVersion && response.m_requestId == 2u &&
					!response.m_bSuccess && !response.m_error.empty(),
					"the real native Stop request must prevent a later protocol Start");
				Require(SailorProtocolStopLocalHost(true) != 0 && !App::GetInstance(),
					"editor host shutdown must release its initialized App before retry");
			}
			std::cout << "Native initialization outcomes test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		if (SailorProtocolStopLocalHost(true) == 0) result = 1;
		return result;
	}

	int RunShutdownGpu(int argc, const char** argv, bool idleFailure, bool localHost = false)
	{
		App::Initialize(argv, argc);
		int result = 1;
		auto shutdown = [localHost]() { return localHost ? SailorProtocolStopLocalHost(true) != 0 : App::Shutdown(); };
		try
		{
			Require(App::IsRendererInitialized(), "shutdown test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			{
				FailedFrame failed;
				OnRender([&]() { TestNativeFrameFailure(true, VK_ERROR_OUT_OF_HOST_MEMORY, failed); });
				auto device = VulkanApi::GetInstance()->GetMainDevice();
				auto swapchain = device->GetSwapchain();
				auto* renderer = App::GetSubmodule<Renderer>();
				auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
				for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
				{
					bool stopped;
					if (idleFailure)
					{
						const auto before = queueWaitCalls;
						QueueWaitOverride refusal(device->GetGraphicsQueue(), error);
						stopped = shutdown();
						Require(queueWaitCalls > before, "shutdown must reach the actual queue-idle failure");
					}
					else
					{
						const auto before = submitCalls;
						SubmitOverride refusal(device->GetGraphicsQueue(), error);
						stopped = shutdown();
						Require(submitCalls == before + 1u && lastWait == *failed.acquire &&
							lastSignal == VK_NULL_HANDLE && lastCommandCount == 0u,
							"shutdown must consume the outstanding acquisition before releasing its owners");
					}
					Require(!stopped && App::GetSubmodule<Renderer>() == renderer && App::GetSubmodule<Tasks::Scheduler>() == scheduler,
						"failed shutdown must retain the App, renderer and scheduler for retry");
					Require(device->GetSwapchain() == swapchain, "failed shutdown must retain the swapchain");
					VulkanSubmissionTestAccess::CheckSyncCounts(*device);
					Require(VulkanSubmissionTestAccess::FlightFence(*device) == failed.fence &&
						failed.frame.command->m_vulkan.m_commandBuffer.NumRefs() == 2u,
						"failed shutdown must retain frame synchronization and command dependencies");
					CheckReadback(failed.frame, false);
				}
			}
			// Release test-owned native references before the successful App teardown.
			Require(shutdown() && App::GetSubmodule<Renderer>() == nullptr && App::GetSubmodule<Tasks::Scheduler>() == nullptr,
				"shutdown must complete after the refusal is removed");
			Require(shutdown(), "a completed shutdown must remain idempotent");
			App::Initialize(argv, argc);
			Require(App::IsRendererInitialized(), "a completed shutdown must allow a new native session");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			OnRender([]() { TestImmediateSubmission(false); });
			std::cout << "Native shutdown ownership test passed: " << (idleFailure ? "idle" : "acquire") << (localHost ? " host" : " App") << '\n';
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!shutdown()) result = 1;
		return result;
	}

#if defined(__APPLE__)
	void WriteEditorReadbackGraph(const std::filesystem::path& path, size_t firstTarget,
		glm::ivec2 extent, bool authored = false, bool surface = false, bool unsupportedFirst = false, bool multiple = false)
	{
		constexpr const char* names[] = { "EditorOutput", "Main", "BackBuffer", "Secondary" };
		constexpr const char* colors[] = { "1, 0, 0, 1", "0, 1, 0, 1", "0, 0, 1, 1", "1, 1, 1, 1" };
		std::ofstream graph(path);
		graph << "renderTargets:\n";
		for (size_t i = firstTarget; i < std::size(names); ++i)
		{
			graph << "- name: " << names[i] << "\n  format: " << (unsupportedFirst && i == 0 ? "R32_SFLOAT" : "B8G8R8A8_UNORM") <<
				"\n  width: " << extent.x <<
				"\n  height: " << extent.y << "\n  bIsSurface: " << (surface ? "true" : "false") << '\n';
		}
		graph << "frame:\n";
		for (size_t i = firstTarget; i < std::size(names); ++i)
		{
			graph << "- name: Clear\n  vec4:\n  - clearColor: [" << colors[i] <<
				"]\n  renderTargets:\n  - target: " << names[i] << '\n';
		}
		if (authored)
			graph << "- name: EditorReadback\n  tag: CaptureForInspector\n  renderTargets:\n  - src: Secondary\n";
		if (multiple)
			graph << "- name: EditorReadback\n  renderTargets:\n  - src: Main\n";
		graph.close();
		Require(static_cast<bool>(graph), "temporary editor graph must be written");
	}

	void TestEditorReadbackGraph(const std::filesystem::path& path)
	{
		auto* renderer = App::GetSubmodule<Renderer>();
		auto* scheduler = App::GetSubmodule<Tasks::Scheduler>();
		auto world = App::GetSubmodule<EngineLoop>()->CreateEmptyWorld("Readback graph", static_cast<uint8_t>(EWorldBehaviourBit::EcsTickable));
		uint64_t tick = 0;
		auto pushFrame = [&]()
			{
				Sailor::FrameState frame(world.GetRawPtr(), static_cast<int64_t>(++tick * 16u), {}, { 32, 24 });
				world->Tick(frame);
				Require(renderer->PushFrame(frame), "actual renderer must accept the readback graph frame");
				scheduler->WaitIdle({ EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
				scheduler->ProcessTasksOnMainThread();
			};
		EditorReadbackFramePtr previousGeneration;
		for (size_t scenario = 0; scenario < 11; ++scenario)
		{
			const size_t firstTarget = scenario < 4 ? scenario : 0;
			const bool authored = scenario == 4 || scenario == 9;
			const bool multiple = scenario == 9;
			const bool surface = scenario == 5;
			const glm::ivec2 extent = scenario == 10 ? glm::ivec2(3840, 2160) :
				scenario == 6 ? glm::ivec2(1280, 720) : scenario == 7 ? glm::ivec2(1920, 1080) :
				scenario < 4 ? glm::ivec2(32, 24) : glm::ivec2(67, 39);
			WriteEditorReadbackGraph(path, firstTarget, extent, authored, surface, scenario == 8, multiple);
			const bool appViewport = scenario == 6 || scenario == 7 || scenario == 10;
			if (appViewport)
			{
				App::SetEditorRenderTargetSize(extent.x, extent.y);
				Require(EditorRuntime::ApplyPendingEditorViewportOnEngineThread(), "App viewport sweep must apply each new render extent");
			}
			renderer->RefreshFrameGraph();
			Require(renderer->EnsureFrameGraph() && renderer->HasEditorReadback(),
				"an editor graph without an authored readback must receive an asynchronous producer");
			auto graph = renderer->GetFrameGraph()->GetRHI();
			size_t producers = 0;
			TRefPtr<Framegraph::EditorReadbackNode> node;
			for (auto& candidate : graph->GetGraph())
				if (auto capture = candidate.DynamicCast<Framegraph::EditorReadbackNode>()) { node = capture; ++producers; }
			Require(producers == (multiple ? 2u : 1u) && (multiple || !authored || node->GetTag() == "CaptureForInspector"),
				"authored capture tags must be preserved without adding a duplicate producer");
			Require(renderer->EnsureFrameGraph() && graph == renderer->GetFrameGraph()->GetRHI(),
				"repeated EnsureFrameGraph must preserve the active producer and graph");
			if (surface)
				Require(graph->GetSurface("EditorOutput")->NeedsResolve() &&
					graph->GetSurface("EditorOutput")->GetTarget()->GetMsaaSamples() == EMsaaSamples::Samples_4,
					"surface coverage must exercise actual multisampling, not two single-sample aliases");
			if (previousGeneration)
			{
				OnRender([&]() { renderer->QueueEditorReadback(previousGeneration); });
				scheduler->ProcessTasksOnMainThread();
				Require(!renderer->GetEditorReadback(), "a previous graph generation must not republish after replacement");
			}
			for (uint32_t frame = 0; frame < 12u && !renderer->GetEditorReadback(); ++frame) pushFrame();
			EditorRemote::MacRendererFrameSource source;
			Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_readback &&
				source.m_readback->m_extent == extent, "actual Render-to-Main publication must expose completed pixels at the current size");
			constexpr uint32_t expected[] = { 0xffff0000u, 0xff00ff00u, 0xff0000ffu, 0xffffffffu };
			const uint32_t color = expected[multiple || scenario == 8 ? 1 : authored ? 3 : firstTarget];
			for (int y = 0; y < extent.y; ++y)
				for (int x = 0; x < extent.x; ++x)
					Require(std::memcmp(source.GetCpuBytes() + y * source.m_bytesPerRow + x * 4, &color, sizeof(color)) == 0,
						"capture must preserve actual pixels and source priority, including resolved surfaces");
			const auto retained = source;
			{
				auto device = VulkanApi::GetInstance()->GetMainDevice();
				FenceDispatchOverride fences(*device);
				SubmitOverride submits(device->GetGraphicsQueue(), VK_ERROR_OUT_OF_HOST_MEMORY);
				const auto beforeSubmits = submitCalls;
				const auto beforeWaits = allFenceWaitCalls;
				const auto start = std::chrono::steady_clock::now();
				for (uint32_t i = 0; i < 1000u; ++i)
					Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_readback == retained.m_readback,
						"repeated Main acquisition must retain the same immutable completed frame");
				const auto microseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count();
				Require(submitCalls == beforeSubmits && allFenceWaitCalls == beforeWaits,
					"Main frame acquisition must neither submit commands nor wait for a GPU fence");
				std::cout << "Readback graph scenario " << scenario << ": 1000 Main acquisitions " << microseconds << " us\n";
			}
			CheckReadbackUpload(source);
			if (appViewport)
			{
				uint32_t nextColor = 0;
				EditorReadbackStats warm, final;
				Tests::CheckMacAppViewportPump(source, [&]()
					{
						++nextColor;
						const uint32_t expectedColor = 0xff000000u | (nextColor * 0x00010101u);
						OnRender([&]() { graph->GetGraph()[0]->SetVec4("clearColor", glm::vec4(glm::vec3(nextColor / 255.0f), 1.0f)); });
						EditorRemote::MacRendererFrameSource next;
						for (uint32_t attempt = 0; attempt < 12; ++attempt)
						{
							pushFrame();
							if (EditorRuntime::TryAcquireEditorReadbackFrameSource(next) &&
								std::memcmp(next.GetCpuBytes(), &expectedColor, 4) == 0) break;
						}
						Require(next.m_readback && std::memcmp(next.GetCpuBytes(), &expectedColor, 4) == 0,
							"new App-frame capture must contain the newly rendered color");
						OnRender([&]() { final = node->GetStats(); });
						if (nextColor == 5) warm = final;
						if (nextColor > 5)
							Require(final.m_bufferAllocatedBytes == warm.m_bufferAllocatedBytes &&
								final.m_conversionAllocatedBytes == 0 && final.m_convertedBytes == 0,
								"warm BGRA App presentation must reuse readback buffers without full-frame CPU conversion allocations");
						return next;
					});
				const uint64_t payload = static_cast<uint64_t>(extent.x) * extent.y * 4u;
				Require(final.m_recordedReadbackBytes >= 23u * payload && final.m_bufferAllocatedBytes != 0,
					"App sweep counters must include actual new GPU readbacks and their bounded buffer allocation");
				std::cout << "App readback bytes " << extent.x << 'x' << extent.y << ": allocated " << final.m_bufferAllocatedBytes <<
					", recorded " << final.m_recordedReadbackBytes << ", conversion allocated " << final.m_conversionAllocatedBytes <<
					", converted " << final.m_convertedBytes << '\n';
			}
			if (scenario == 0 || scenario == 5 || scenario == 6)
			{
				EditorReadbackStats before, after;
				OnRender([&]()
					{
						before = node->GetStats();
						if (scenario == 0)
							for (const char* name : { "EditorOutput", "Main", "BackBuffer", "Secondary" }) graph->SetRenderTarget(name, {});
						else if (scenario == 5) node->SetRHIResource("src", graph->GetSurface("Main")->GetTarget());
						else node->SetRHIResource_Unresolved("src", "LateOutput");
					});
				// Existing pending captures may still finish; no new capture may be recorded.
				for (uint32_t frame = 0; frame < 4; ++frame) pushFrame();
				const auto last = renderer->GetEditorReadback();
				pushFrame();
				OnRender([&]() { after = node->GetStats(); });
				Require(last && renderer->GetEditorReadback() == last &&
					after.m_recordedReadbackBytes == before.m_recordedReadbackBytes,
					"missing or unresolved sources and raw MSAA images must retain the last completed image without copying");
				if (scenario == 6)
				{
					OnRender([&]() { graph->SetRenderTarget("LateOutput", graph->GetRenderTarget("Secondary")); });
					for (uint32_t frame = 0; frame < 12 && renderer->GetEditorReadback() == last; ++frame) pushFrame();
					Require(EditorRuntime::TryAcquireEditorReadbackFrameSource(source) && source.m_readback != last,
						"a per-frame source must become readable when its resource is installed");
					const uint32_t white = 0xffffffffu;
					Require(std::memcmp(source.GetCpuBytes(), &white, sizeof(white)) == 0,
						"explicit late source must win over the automatic EditorOutput fallback");
				}
			}
			previousGeneration = source.m_readback;
		}
	}

	int RunEditorReadbackGraphGpu(int argc, const char** argv)
	{
		Tests::TempDirectory workspace("editor-readback-graph");
		std::filesystem::create_directories(workspace.Path("Content"));
		std::string enginePath = std::filesystem::current_path().string();
		for (int i = 1; i + 1 < argc; ++i)
			if (std::string_view(argv[i]) == "--workspace") enginePath = argv[i + 1];
		YAML::Node manifest;
		manifest["manifestVersion"] = 1;
		manifest["workspaceId"] = "00000000-0000-0000-0000-000000000105";
		manifest["name"] = "Readback graph test";
		manifest["enginePath"] = enginePath;
		manifest["engineReferenceKind"] = "source";
		manifest["contentPath"] = "Content";
		manifest["sourcePath"] = "Source";
		manifest["generatedProjectPath"] = "Generated";
		manifest["cachePath"] = "Cache";
		manifest["buildPath"] = "Cache/Build";
		manifest["logicOutputPath"] = "Binaries";
		manifest["logicModuleName"] = "ReadbackGraphTest";
		std::ofstream(workspace.Path("workspace.sailor")) << manifest;
		auto graphics = YAML::LoadFile((std::filesystem::path(enginePath) / "ProjectSettings.yaml").string());
		graphics["graphics"]["defaultQuality"] = "High";
		graphics["graphics"]["presets"]["High"]["msaaSamples"] = 4;
		std::ofstream(workspace.Path("ProjectSettings.yaml")) << graphics;
		const auto graphPath = workspace.Path("Content/EditorRenderer.renderer");
		WriteEditorReadbackGraph(graphPath, 0, { 32, 24 });
		const std::string workspacePath = workspace.Get().string();
		std::vector<const char*> arguments(argv, argv + argc);
		arguments.insert(arguments.end(), { "--workspace", workspacePath.c_str(), "--new-world", "--editor", "--port", "0" });
		App::Initialize(arguments.data(), static_cast<int>(arguments.size()));
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "editor graph test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			TestEditorReadbackGraph(graphPath);
			std::cout << "Native editor readback graph test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}
#endif

	int RunFenceGpu(int argc, const char** argv, std::string_view mode)
	{
		std::vector<const char*> arguments(argv, argv + argc);
		const bool editorReadback = mode.starts_with("--gpu-editor-readback") || mode.starts_with("--gpu-metal-");
		if (editorReadback) arguments.insert(arguments.end(), { "--editor", "--port", "0" });
		App::Initialize(arguments.data(), static_cast<int>(arguments.size()));
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "fence test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			if (mode == "--gpu-editor-readback") TestEditorReadback();
#if defined(__APPLE__)
			else if (mode == "--gpu-metal-export") OnRender([]() { TestMetalTextureExport(); });
			else if (mode == "--gpu-metal-retirement") OnRender([]() { TestMetalTextureRetirement(); });
#endif
			else if (editorReadback) TestEditorReadbackRefusal(mode == "--gpu-editor-readback-lost");
			else OnRender([&]()
				{
					if (mode == "--gpu-extended-submit") TestExtendedSubmission(false);
					else if (mode == "--gpu-frame-completion") TestFrameCompletionReuse();
					else if (mode == "--gpu-extended-submit-lost") TestExtendedSubmission(true);
#if defined(_WIN32)
					else if (mode == "--gpu-windows-shared") TestWindowsSharedSurfaceCopy(false);
					else if (mode == "--gpu-windows-shared-lost") TestWindowsSharedSurfaceCopy(true);
#endif
					else if (mode == "--gpu-immediate-image-create") TestImmediateImageCreation(false);
					else if (mode == "--gpu-immediate-images") TestImmediateImageContents();
					else if (mode == "--gpu-immediate-image-create-lost") TestImmediateImageCreation(true);
					else if (mode == "--gpu-immediate-buffer-create") TestImmediateBufferCreation();
					else if (mode == "--gpu-immediate-buffers")
					{
						TestImmediateBufferCreation();
						TestImmediateBufferCopy(false);
						TestParticleBufferPublication();
					}
					else if (mode == "--gpu-immediate-buffer-create-lost") TestImmediateBufferCreation(true);
					else if (mode == "--gpu-immediate-buffer-copy-lost") TestImmediateBufferCopy(true);
					else if (mode == "--gpu-immediate")
					{
						TestImmediateSubmission(false);
						TestImmediateBindingUpdate(false);
					}
					else if (mode == "--gpu-immediate-lost") TestImmediateSubmission(true);
					else if (mode == "--gpu-immediate-binding-lost") TestImmediateBindingUpdate(true);
					else if (mode == "--gpu-fence-completion") TestFenceCompletionAndReuse();
					else TestAcceptedUploadLoss(mode == "--gpu-fence-wait-loss");
				});
			std::cout << "Native fence test passed: " << mode << '\n';
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}

	int RunGpu(int argc, const char** argv, bool present, bool lost, bool upload = false, bool accepted = false)
	{
		App::Initialize(argv, argc);
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "GPU test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			for (VkResult error : { VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY })
			{
				if (upload)
				{
					OnRender([&]() { TestNativeUploadFailure(lost ? VK_ERROR_DEVICE_LOST : error, accepted); });
					if (lost) break;
					continue;
				}
				FailedFrame failed;
				OnRender([&]() { TestNativeFrameFailure(present, lost ? VK_ERROR_DEVICE_LOST : error, failed); });
				if (present && !lost)
				{
					OnRender([&]()
						{
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							const auto before = submitCalls;
							SubmitOverride refusal(device->GetGraphicsQueue(), error);
							Require(!device->FixLostDevice(App::GetMainWindow().GetRawPtr()),
								"failed recovery discarded a pending acquired image");
							Require(submitCalls == before + 1u && lastWait == *failed.acquire &&
								lastSignal == VK_NULL_HANDLE && lastCommandCount == 0u,
								"recovery must consume exactly the outstanding acquisition without signaling a failed flight");
							Require(VulkanSubmissionTestAccess::FlightFence(*device) == failed.fence &&
								failed.frame.command->m_vulkan.m_commandBuffer.NumRefs() == 2u,
								"recovery refusal must retain the old flight and dependencies");
						});
				}
				const bool recovered = Renderer::GetDriver()->FixLostDevice(App::GetMainWindow().GetRawPtr());
				Require(recovered != lost, "only swapchain failure can be recovered without replacing VkDevice");
				if (lost)
				{
					OnRender([&]()
						{
							auto device = VulkanApi::GetInstance()->GetMainDevice();
							Require(VulkanSubmissionTestAccess::FlightFence(*device) == failed.fence &&
								failed.fence->Status() == VK_NOT_READY, "device loss must not replace or signal frame sync objects");
						});
					break;
				}
				OnRender([&]() { TestRecoveredFrames(present, failed); });
				std::cout << "Recovered error " << error << " with verified GPU frame readbacks\n";
			}
			std::cout << "Native frame failure test passed\n";
			result = 0;
		}
		catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
		App::Stop();
		if (!App::Shutdown()) result = 1;
		return result;
	}
}

int main(int argc, const char** argv)
{
	for (int i = 1; i < argc; ++i)
	{
		const std::string_view mode(argv[i]);
#if defined(__APPLE__)
		if (mode == "--gpu-editor-readback-graph") return RunEditorReadbackGraphGpu(argc, argv);
		if (mode == "--gpu-metal-export" || mode == "--gpu-metal-retirement") return RunFenceGpu(argc, argv, mode);
#else
		if (mode == "--gpu-editor-readback-graph" || mode.starts_with("--gpu-metal-")) return 77;
#endif
#if defined(_WIN32)
		if (mode == "--gpu-windows-shared" || mode == "--gpu-windows-shared-lost") return RunFenceGpu(argc, argv, mode);
#else
		if (mode == "--gpu-windows-shared" || mode == "--gpu-windows-shared-lost")
		{
			std::cout << "Windows shared-surface GPU tests require Windows\n";
			return 77;
		}
#endif
		if (mode == "--gpu-bootstrap-submit") return RunBootstrapGpu(argc, argv, false, false);
		if (mode == "--gpu-initialization") return RunInitializationGpu(argc, argv);
		if (mode == "--gpu-pathtracer" || mode == "--gpu-pathtracer-1x") return Tests::RunPathTracerCommandTests(argc, argv);
		if (mode == "--gpu-bootstrap-submit-lost") return RunBootstrapGpu(argc, argv, false, true);
		if (mode == "--gpu-bootstrap-wait") return RunBootstrapGpu(argc, argv, true, false);
		if (mode == "--gpu-bootstrap-lost") return RunBootstrapGpu(argc, argv, true, true);
		if (mode == "--gpu-shutdown-acquire") return RunShutdownGpu(argc, argv, false);
		if (mode == "--gpu-shutdown-idle") return RunShutdownGpu(argc, argv, true);
		if (mode == "--gpu-host-shutdown-acquire") return RunShutdownGpu(argc, argv, false, true);
		if (mode == "--gpu-host-shutdown-idle") return RunShutdownGpu(argc, argv, true, true);
		if (mode == "--gpu-editor-readback" || mode == "--gpu-editor-readback-refused" || mode == "--gpu-editor-readback-lost" ||
			mode == "--gpu-frame-completion" ||
			mode == "--gpu-fence-poll-loss" || mode == "--gpu-fence-wait-loss" || mode == "--gpu-fence-completion" ||
			mode == "--gpu-immediate" || mode == "--gpu-immediate-lost" || mode == "--gpu-immediate-binding-lost" ||
			mode == "--gpu-immediate-buffer-create" || mode == "--gpu-immediate-buffers" ||
			mode == "--gpu-immediate-image-create" || mode == "--gpu-immediate-image-create-lost" ||
			mode == "--gpu-immediate-images" ||
			mode == "--gpu-extended-submit" || mode == "--gpu-extended-submit-lost" ||
			mode == "--gpu-immediate-buffer-create-lost" || mode == "--gpu-immediate-buffer-copy-lost")
			return RunFenceGpu(argc, argv, mode);
		if (std::string_view(argv[i]) == "--gpu-present") return RunGpu(argc, argv, true, false);
		if (std::string_view(argv[i]) == "--gpu-offscreen") return RunGpu(argc, argv, false, false);
		if (std::string_view(argv[i]) == "--gpu-lost-present") return RunGpu(argc, argv, true, true);
		if (std::string_view(argv[i]) == "--gpu-lost-offscreen") return RunGpu(argc, argv, false, true);
		if (std::string_view(argv[i]) == "--gpu-upload") return RunGpu(argc, argv, false, false, true);
		if (std::string_view(argv[i]) == "--gpu-upload-lost") return RunGpu(argc, argv, false, true, true);
		if (std::string_view(argv[i]) == "--gpu-upload-pending-loss") return RunGpu(argc, argv, false, true, true, true);
	}
	try
	{
		TestQueueDispatch();
		std::cout << "Vulkan submission tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
