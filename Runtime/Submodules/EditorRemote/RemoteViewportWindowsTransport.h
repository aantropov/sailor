#pragma once
#include "Containers/Containers.h"
#include "Memory/UniquePtr.hpp"

#include <algorithm>
#include <optional>
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

		size_t GetHash() const noexcept
		{
			size_t seed = 0;
			HashCombine(seed, m_viewportId, m_epoch, m_generation);
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
			auto release = ReleaseSurface(viewport.m_viewportId, epoch, generation);
			if (!release.IsOk()) return release;
			const WindowsViewportSurfaceKey key{ viewport.m_viewportId, epoch, generation };
			auto& storedState = m_surfaces[key];
			storedState = TUniquePtr<WindowsViewportSurfaceState>::Make();
			auto& state = *storedState;
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
				ReleaseSurface(viewport.m_viewportId, epoch, generation);
				m_lastFailure = result;
				return result;
			}

			state.m_transport.m_transportType = TransportType::WinSharedHandle;
			state.m_transport.m_ready = true;
			result = state.m_transport.Validate();
			if (result.IsOk() && (state.m_transport.m_width != viewport.m_width ||
				state.m_transport.m_height != viewport.m_height || state.m_transport.m_pixelFormat != viewport.m_pixelFormat))
			{
				result = Failure::FromDomain(ErrorDomain::Protocol, 1, "Transport does not match its viewport");
			}
			if (!result.IsOk())
			{
				ReleaseSurface(viewport.m_viewportId, epoch, generation);
				m_lastFailure = result;
				return result;
			}

			outTransport = state.m_transport;
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
			if (state->m_frameBegun)
			{
				m_lastFailure = Failure::Ok();
				return Failure::Ok();
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
			auto it = m_surfaces.Find(key);
			if (it == m_surfaces.end())
			{
				m_lastFailure = Failure::Ok();
				return Failure::Ok();
			}

			auto result = m_provider.ReleaseSurface(*it.Value());
			if (!result.IsOk())
			{
				m_lastFailure = m_provider.GetLastFailure();
				return result;
			}

			m_surfaces.Remove(key);
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
			auto it = m_surfaces.Find(key);
			return it != m_surfaces.end() ? it.Value().GetRawPtr() : nullptr;
		}

		size_t GetSurfaceCount() const { return m_surfaces.Num(); }

		Failure ReleaseSurfaces(ViewportId viewportId, ConnectionEpoch keepEpoch = 0, SurfaceGeneration keepGeneration = 0)
		{
			Failure failure = Failure::Ok();
			for (const auto& key : m_surfaces.GetKeys())
			{
				if (key.m_viewportId == viewportId && (key.m_epoch != keepEpoch || key.m_generation != keepGeneration))
				{
					auto result = ReleaseSurface(key.m_viewportId, key.m_epoch, key.m_generation);
					if (!result.IsOk()) failure = result;
				}
			}
			m_lastFailure = failure;
			return failure;
		}

	private:
		WindowsViewportSurfaceState* FindSurface(ViewportId viewportId, ConnectionEpoch epoch, SurfaceGeneration generation)
		{
			WindowsViewportSurfaceKey key{ viewportId, epoch, generation };
			auto it = m_surfaces.Find(key);
			return it != m_surfaces.end() ? it.Value().GetRawPtr() : nullptr;
		}

		IWindowsSharedSurfaceProvider& m_provider;
		TMap<WindowsViewportSurfaceKey, TUniquePtr<WindowsViewportSurfaceState>> m_surfaces{};
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

	class WindowsViewportLoopbackBinding
	{
	public:
		WindowsViewportLoopbackBinding(
			ViewportDescriptor descriptor,
			IWindowsSharedSurfaceProvider& provider,
			IWindowsViewportPresenter& presenter,
			ConnectionEpoch epoch = 1) :
			m_transportBackend(provider),
			m_host(presenter),
			m_runtimeSession(std::move(descriptor), epoch)
		{
		}

		RemoteViewportSession& GetRuntimeSession() { return m_runtimeSession; }
		const RemoteViewportSession& GetRuntimeSession() const { return m_runtimeSession; }
		WindowsViewportTransportBackend& GetTransportBackend() { return m_transportBackend; }
		const WindowsViewportTransportBackend& GetTransportBackend() const { return m_transportBackend; }
		WindowsViewportNativeHost& GetHost() { return m_host; }
		const WindowsViewportNativeHost& GetHost() const { return m_host; }

		Failure Create(uint64_t nowMs = GetMonotonicTimeMs())
		{
			auto result = m_runtimeSession.BeginNegotiation(nowMs);
			if (!result.IsOk())
			{
				return result;
			}

			m_visible = true;
			m_focused = false;
			m_created = true;
			return EnsureTransportImported();
		}

		Failure Resize(uint32_t width, uint32_t height, uint64_t nowMs = GetMonotonicTimeMs())
		{
			const auto epoch = m_runtimeSession.GetConnectionEpoch();
			const auto generation = m_runtimeSession.GetGeneration();
			auto descriptor = m_runtimeSession.GetDescriptor();
			descriptor.m_width = std::max(width, 1u);
			descriptor.m_height = std::max(height, 1u);
			auto result = m_runtimeSession.ValidateResize(descriptor);
			if (!result.IsOk()) return result;
			result = m_transportBackend.ReleaseSurfaces(descriptor.m_viewportId, epoch, generation);
			if (!result.IsOk()) return result;

			TransportDescriptor transport;
			result = m_transportBackend.EnsureSurface(descriptor, epoch, generation + 1, transport);
			if (!result.IsOk()) return result;
			result = ImportTransport(descriptor, generation + 1, transport);
			if (!result.IsOk())
			{
				m_transportBackend.ReleaseSurface(descriptor.m_viewportId, epoch, generation + 1);
				return result;
			}

			// Publish the candidate only after native import succeeds.
			m_runtimeSession.HandleResize(descriptor, nowMs);
			result = CompleteTransportImport(transport);
			if (!result.IsOk()) return result;
			return m_transportBackend.ReleaseSurfaces(descriptor.m_viewportId, epoch, generation + 1);
		}

		Failure SetVisible(bool visible)
		{
			m_visible = visible;
			return m_runtimeSession.SetVisible(visible);
		}

		Failure SetFocused(bool focused)
		{
			m_focused = focused;
			InputPacket input{};
			input.m_viewportId = m_runtimeSession.GetViewportId();
			input.m_connectionEpoch = m_runtimeSession.GetConnectionEpoch();
			input.m_generation = m_runtimeSession.GetGeneration();
			input.m_kind = InputKind::Focus;
			input.m_focused = focused;
			input.m_timestampNs = ++m_inputTimestampNs;
			return m_runtimeSession.HandleInput(input);
		}

		Failure PumpFrame(uint64_t nowMs = GetMonotonicTimeMs())
		{
			if (!m_created)
			{
				return Failure::FromDomain(
					ErrorDomain::Session,
					1,
					"Windows loopback binding must be created before pumping frames");
			}
			auto timeout = m_runtimeSession.TickTimeouts(nowMs);
			if (!timeout.IsOk()) return timeout;
			if (m_runtimeSession.GetState() == SessionState::Recovering)
			{
				m_host.ResetViewport(m_runtimeSession.GetViewportId());
				auto result = m_transportBackend.ReleaseSurfaces(m_runtimeSession.GetViewportId());
				if (!result.IsOk()) return result;
				result = m_runtimeSession.Recreate(m_runtimeSession.GetConnectionEpoch() + 1, nowMs);
				if (!result.IsOk()) return result;
				result = EnsureTransportImported();
				if (!result.IsOk()) return result;
			}
			if (m_transportBackend.GetSurfaceCount() > 1)
			{
				auto result = m_transportBackend.ReleaseSurfaces(m_runtimeSession.GetViewportId(),
					m_runtimeSession.GetConnectionEpoch(), m_runtimeSession.GetGeneration());
				if (!result.IsOk()) return result;
			}
			if (m_runtimeSession.GetState() != SessionState::Active)
			{
				return m_runtimeSession.GetFailure();
			}

			auto result = m_runtimeSession.PublishFrameFromBackend(m_transportBackend);
			if (!result.IsOk())
			{
				return result;
			}

			result = m_host.AcceptFrame(m_runtimeSession.GetLastFrame());
			if (!result.IsOk())
			{
				return result;
			}

			return m_host.PresentLatestFrame(
				m_runtimeSession.GetDescriptor().m_viewportId);
		}

		Failure Destroy()
		{
			if (!m_created)
			{
				return Failure::Ok();
			}

			auto release = m_transportBackend.ReleaseSurfaces(m_runtimeSession.GetViewportId());
			m_host.ResetViewport(m_runtimeSession.GetDescriptor().m_viewportId);
			auto destroy = m_runtimeSession.Destroy();
			m_created = !release.IsOk();
			return !release.IsOk() ? release : destroy;
		}

	private:
		Failure EnsureTransportImported()
		{
			TransportDescriptor transport;
			auto result = m_runtimeSession.EnsureBackendTransport(m_transportBackend, transport);
			if (!result.IsOk())
			{
				return result;
			}

			result = ImportTransport(m_runtimeSession.GetDescriptor(), m_runtimeSession.GetGeneration(), transport);
			if (!result.IsOk())
			{
				m_transportBackend.ReleaseSurface(m_runtimeSession.GetViewportId(),
					m_runtimeSession.GetConnectionEpoch(), m_runtimeSession.GetGeneration());
				m_runtimeSession.MarkFailure(result);
				return result;
			}
			return CompleteTransportImport(transport);
		}

		Failure ImportTransport(const ViewportDescriptor& descriptor, SurfaceGeneration generation, const TransportDescriptor& transport)
		{
			const auto* surface = std::as_const(m_transportBackend).FindSurface(
				descriptor.m_viewportId,
				m_runtimeSession.GetConnectionEpoch(),
				generation);
			if (!surface)
			{
				return Failure::FromDomain(
					ErrorDomain::Transport,
					1,
					"Windows loopback binding could not resolve imported surface");
			}

			return m_host.ImportTransport(
				descriptor,
				transport,
				m_runtimeSession.GetConnectionEpoch(),
				generation);
		}

		Failure CompleteTransportImport(const TransportDescriptor& transport)
		{
			auto result = m_runtimeSession.MarkTransportReady(transport);
			if (!result.IsOk())
			{
				return result;
			}

			if (!m_visible)
			{
				result = m_runtimeSession.SetVisible(false);
				if (!result.IsOk())
				{
					return result;
				}
			}
			if (m_focused)
			{
				result = SetFocused(true);
				if (!result.IsOk())
				{
					return result;
				}
			}

			return Failure::Ok();
		}

		WindowsViewportTransportBackend m_transportBackend;
		WindowsViewportNativeHost m_host;
		RemoteViewportSession m_runtimeSession;
		uint64_t m_inputTimestampNs = 0;
		bool m_created = false;
		bool m_visible = true;
		bool m_focused = false;
	};
}
