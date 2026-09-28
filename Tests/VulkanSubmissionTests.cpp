#include "Sailor.h"
#include "Engine/Frame.h"
#include "FrameGraph/ParticlesNode.h"
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
#include "RHI/Texture.h"
#include "Tasks/Tasks.h"

#include <iostream>
#include <algorithm>
#include <array>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

using namespace Sailor;
using namespace Sailor::RHI;
using namespace Sailor::GraphicsDriver::Vulkan;

extern "C" SAILOR_SHARED_API int32_t SailorProtocolStopLocalHost(bool bShutdownEngine) noexcept;

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

	int RunFenceGpu(int argc, const char** argv, std::string_view mode)
	{
		App::Initialize(argv, argc);
		int result = 1;
		try
		{
			Require(App::IsRendererInitialized(), "fence test requires an initialized renderer");
			App::GetSubmodule<Tasks::Scheduler>()->WaitIdle({ EThreadType::Main, EThreadType::Worker, EThreadType::RHI, EThreadType::Render });
			OnRender([&]()
				{
					if (mode == "--gpu-immediate-image-create") TestImmediateImageCreation(false);
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
		if (mode == "--gpu-bootstrap-submit") return RunBootstrapGpu(argc, argv, false, false);
		if (mode == "--gpu-bootstrap-submit-lost") return RunBootstrapGpu(argc, argv, false, true);
		if (mode == "--gpu-bootstrap-wait") return RunBootstrapGpu(argc, argv, true, false);
		if (mode == "--gpu-bootstrap-lost") return RunBootstrapGpu(argc, argv, true, true);
		if (mode == "--gpu-shutdown-acquire") return RunShutdownGpu(argc, argv, false);
		if (mode == "--gpu-shutdown-idle") return RunShutdownGpu(argc, argv, true);
		if (mode == "--gpu-host-shutdown-acquire") return RunShutdownGpu(argc, argv, false, true);
		if (mode == "--gpu-host-shutdown-idle") return RunShutdownGpu(argc, argv, true, true);
		if (mode == "--gpu-fence-poll-loss" || mode == "--gpu-fence-wait-loss" || mode == "--gpu-fence-completion" ||
			mode == "--gpu-immediate" || mode == "--gpu-immediate-lost" || mode == "--gpu-immediate-binding-lost" ||
			mode == "--gpu-immediate-buffer-create" || mode == "--gpu-immediate-buffers" ||
			mode == "--gpu-immediate-image-create" || mode == "--gpu-immediate-image-create-lost" ||
			mode == "--gpu-immediate-images" ||
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
