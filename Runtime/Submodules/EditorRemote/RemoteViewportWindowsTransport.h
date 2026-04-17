#pragma once

#include <optional>
#include <unordered_map>
#include <utility>

#include "EditorViewportSession.h"
#include "RemoteViewportRuntime.h"

namespace Sailor::EditorRemote
{
	struct WindowsViewportSurfaceKey
	{
		ViewportId m_viewportId = 0;
		ConnectionEpoch m_epoch = 0;
		SurfaceGeneration m_generation = 0;

		auto operator<=>(const WindowsViewportSurfaceKey&) const = default;
	};

	struct WindowsViewportSurfaceKeyHasher
	{
		size_t operator()(const WindowsViewportSurfaceKey& key) const noexcept
		{
			size_t seed = std::hash<uint64_t>{}(key.m_viewportId);
			seed ^= std::hash<uint64_t>{}(key.m_epoch) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
			seed ^= std::hash<uint64_t>{}(key.m_generation) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
			return seed;
		}
	};

	struct WindowsViewportSurfaceState
	{
		WindowsViewportSurfaceKey m_key{};
		ViewportDescriptor m_viewport{};
		TransportDescriptor m_transport{};
		FrameIndex m_lastExportedFrameIndex = 0;
		bool m_frameBegun = false;
	};

	class IWindowsSharedSurfaceProvider
	{
	public:
		virtual ~IWindowsSharedSurfaceProvider() = default;
		virtual Failure CreateOrResizeSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, WindowsViewportSurfaceState& inOutState) = 0;
		virtual Failure BeginFrame(WindowsViewportSurfaceState& state) = 0;
		virtual Failure ExportFrame(WindowsViewportSurfaceState& state, FramePacket& outFrame) = 0;
		virtual Failure ReleaseSurface(const WindowsViewportSurfaceState& state) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	class WindowsViewportTransportBackend : public IViewportTransportBackend
	{
	public:
		explicit WindowsViewportTransportBackend(IWindowsSharedSurfaceProvider& provider) :
			m_provider(provider)
		{
		}

		Failure EnsureSurface(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, TransportDescriptor& outTransport) override
		{
			WindowsViewportSurfaceState state{};
			state.m_key = { viewport.m_viewportId, epoch, generation };
			state.m_viewport = viewport;
			state.m_transport.m_transportType = TransportType::WinSharedHandle;
			state.m_transport.m_syncMode = SyncMode::ExplicitFence;
			state.m_transport.m_protocolVersion = 1;
			state.m_transport.m_width = viewport.m_width;
			state.m_transport.m_height = viewport.m_height;
			state.m_transport.m_pixelFormat = viewport.m_pixelFormat;
			state.m_transport.m_usageFlags = viewport.m_usageFlags;
			state.m_transport.m_ready = false;

			auto result = m_provider.CreateOrResizeSurface(viewport, epoch, generation, state);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			state.m_transport.m_transportType = TransportType::WinSharedHandle;
			state.m_transport.m_ready = true;
			result = state.m_transport.Validate();
			if (!result.IsOk())
			{
				m_lastFailure = result;
				return result;
			}

			outTransport = state.m_transport;
			m_surfaces[state.m_key] = state;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure BeginFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			auto* state = FindSurface(viewport.m_viewportId, epoch, generation);
			if (!state)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Missing Windows transport surface for frame begin");
				return m_lastFailure;
			}

			auto result = m_provider.BeginFrame(*state);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			state->m_frameBegun = true;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ExportFrame(const ViewportDescriptor& viewport, ConnectionEpoch epoch, SurfaceGeneration generation, FramePacket& outFrame) override
		{
			auto* state = FindSurface(viewport.m_viewportId, epoch, generation);
			if (!state)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Missing Windows transport surface for frame export");
				return m_lastFailure;
			}
			if (!state->m_frameBegun)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Protocol, 1, "Windows transport export requires BeginFrame first");
				return m_lastFailure;
			}

			auto result = m_provider.ExportFrame(*state, outFrame);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			state->m_lastExportedFrameIndex = outFrame.m_frameIndex;
			state->m_frameBegun = false;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure ReleaseSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			WindowsViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.find(key);
			if (it == m_surfaces.end())
			{
				m_lastFailure = Failure::Ok();
				return Failure::Ok();
			}

			auto result = m_provider.ReleaseSurface(it->second);
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			m_surfaces.erase(it);
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

		const WindowsViewportSurfaceState* FindSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation) const
		{
			WindowsViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.find(key);
			return it != m_surfaces.end() ? &it->second : nullptr;
		}

		size_t GetSurfaceCount() const { return m_surfaces.size(); }

	private:
		WindowsViewportSurfaceState* FindSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation)
		{
			WindowsViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.find(key);
			return it != m_surfaces.end() ? &it->second : nullptr;
		}

		IWindowsSharedSurfaceProvider& m_provider;
		std::unordered_map<WindowsViewportSurfaceKey, WindowsViewportSurfaceState, WindowsViewportSurfaceKeyHasher> m_surfaces{};
		Failure m_lastFailure = Failure::Ok();
	};

	class IWindowsViewportPresenter
	{
	public:
		virtual ~IWindowsViewportPresenter() = default;
		virtual Failure ImportSurface(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) = 0;
		virtual Failure PresentFrame(ViewportId viewportId, const FramePacket& frame) = 0;
		virtual void ResetViewport(ViewportId viewportId) = 0;
		virtual Failure GetLastFailure() const = 0;
	};

	class WindowsViewportNativeHost : public IEditorViewportHost
	{
	public:
		explicit WindowsViewportNativeHost(IWindowsViewportPresenter& presenter) :
			m_presenter(presenter)
		{
		}

		Failure ImportTransport(const ViewportDescriptor& viewport, const TransportDescriptor& transport, ConnectionEpoch epoch, SurfaceGeneration generation) override
		{
			if (transport.m_transportType != TransportType::WinSharedHandle)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Capability, 1, "Windows host supports WinSharedHandle transport only");
				return m_lastFailure;
			}

			auto validation = transport.Validate();
			if (!validation.IsOk())
			{
				m_lastFailure = validation;
				return validation;
			}

			auto result = m_presenter.ImportSurface(viewport, transport, epoch, generation);
			if (!result.IsOk())
			{
				m_lastFailure = m_presenter.GetLastFailure();
				return result;
			}

			m_importedViewport = viewport.m_viewportId;
			m_importedEpoch = epoch;
			m_importedGeneration = generation;
			m_lastAcceptedFrame.reset();
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure AcceptFrame(const FramePacket& frame) override
		{
			if (!m_importedViewport.has_value() || *m_importedViewport != frame.m_viewportId)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Frame rejected because no matching Windows viewport is imported");
				return m_lastFailure;
			}
			if (frame.m_connectionEpoch != m_importedEpoch || frame.m_generation != m_importedGeneration)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Session, 2, "Frame rejected because imported Windows transport generation is stale");
				return m_lastFailure;
			}

			m_lastAcceptedFrame = frame;
			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		Failure PresentLatestFrame(ViewportId viewportId) override
		{
			if (!m_lastAcceptedFrame.has_value() || m_lastAcceptedFrame->m_viewportId != viewportId)
			{
				m_lastFailure = Failure::FromDomain(ErrorDomain::Transport, 1, "No accepted Windows frame is available for presentation");
				return m_lastFailure;
			}

			auto result = m_presenter.PresentFrame(viewportId, *m_lastAcceptedFrame);
			if (!result.IsOk())
			{
				m_lastFailure = m_presenter.GetLastFailure();
				return result;
			}

			m_lastFailure = Failure::Ok();
			return Failure::Ok();
		}

		void ResetViewport(ViewportId viewportId) override
		{
			if (m_importedViewport.has_value() && *m_importedViewport == viewportId)
			{
				m_importedViewport.reset();
				m_importedEpoch = 0;
				m_importedGeneration = 0;
				m_lastAcceptedFrame.reset();
			}
			m_presenter.ResetViewport(viewportId);
		}

		Failure GetLastFailure() const override
		{
			return m_lastFailure;
		}

	private:
		IWindowsViewportPresenter& m_presenter;
		std::optional<ViewportId> m_importedViewport{};
		ConnectionEpoch m_importedEpoch = 0;
		SurfaceGeneration m_importedGeneration = 0;
		std::optional<FramePacket> m_lastAcceptedFrame{};
		Failure m_lastFailure = Failure::Ok();
	};
}
