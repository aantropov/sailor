# Editor/Engine Protocol

`editor_engine.proto` is the single wire contract between SailorEditor and
SailorLib. The editor and engine exchange only serialized protobuf envelopes
through an authenticated WebSocket endpoint; generated language objects never
cross the transport boundary.

Each binary WebSocket message contains exactly one `ProtocolRequest` or
`ProtocolResponse`. The endpoint is `/sailor/editor/v1`, the requested
protocol marker is `sailor.editor.v1`, compression is disabled, and the
aggregate message limit is 64 MiB. IXWebSocket currently validates that marker
from the request header after upgrade but does not echo it as a negotiated
WebSocket subprotocol. Text, malformed, oversized, unauthorized, and wrong-path
traffic is rejected.

The current local editor host still loads SailorLib because the macOS viewport
data plane passes same-process `CAMetalLayer` and `IOSurface` object handles.
The one-time local bootstrap and fail-safe teardown use a small lifecycle-only
C surface; normal command traffic, including orderly stop and shutdown, uses
WebSocket. The bundled native host binds only to `127.0.0.1` and requires an
ephemeral bearer token; it cannot be configured for a network-visible
plaintext listener. A future process/remote host can expose the same endpoint
over `wss://` without changing protobuf clients after the viewport presenter is
moved across the process boundary. Before a native host is enabled for
non-loopback traffic, it must also enforce the payload limit before buffering a
complete WebSocket message.

Managed protocol calls are asynchronous end to end. Waiting for a lane,
connecting, sending, and receiving must use the corresponding asynchronous
.NET APIs; normal RPC code must not block an editor or UI thread with
`Wait`, `Result`, or `GetAwaiter().GetResult()`. Each transport lane still
allows one in-flight request, while independent lanes may make progress
concurrently. Cancelling the managed wait closes the affected lane but does not
imply that an already admitted native mutation was rolled back.

The local `Initialize` bootstrap is the only platform-affine exception: on
macOS it executes on the MAUI/Cocoa main thread because `App::Initialize`
creates the native `NSWindow`, `NSView`, and `CAMetalLayer`. Once that bootstrap
has established the loopback host, all normal Editor RPC waits and WebSocket
I/O are asynchronous.

After WebSocket validation, regular Engine operations are serialized through
the single Sailor `Editor` worker. An operation may synchronously marshal from
that worker to the Engine main thread when its runtime contract requires main
thread affinity. `Initialize`, `Start`, `Stop`, `Shutdown`, and
`IsEngineRunning` do not enter the Editor worker. `GetExitCode` and
`IsEngineMainThreadReady` also bypass it because those probes are valid before
Scheduler creation and after shutdown. Initialization precedes Scheduler
availability, and stop/shutdown must remain able to interrupt and drain a
blocked regular operation. Shutdown waits for admitted operations to leave the
Editor worker before destroying the Scheduler, so it must never run as an
Editor-worker task and attempt to join itself.

`trace_viewport_ray` resolves a finite world-space target for normalized
top-left viewport coordinates. The current scene viewport chooses the nearest
ready-mesh world AABB entry, then a forward intersection with the `y=0` plane,
then a point 1000 units forward. A request fails only when the viewport,
coordinates, active camera, or resulting ray is invalid; an empty scene is not
a trace failure.

`update_asset` revalidates one registered `FileId` against its metadata,
source-file, and cache revisions. A metadata-only edit reloads that exact
AssetInfo; a changed source reloads every registered AssetInfo backed by the
same source (for example, the assets generated from one glTF file). It does not
scan Content or wait for unrelated Worker, RHI, or Render work. Commands that
change Content topology still use `request_asset_reload`.

`set_animator_parameter` updates one runtime Animator instance using a typed
Float, Int, Bool, Trigger, or ResetTrigger value. `get_animator_state` returns
the active state, optional destination state, controller revision, state clocks,
and crossfade alpha for Editor preview and diagnostics. Both commands identify
the component by its full `InstanceId`, execute through the regular Editor
worker, and marshal to the Engine main thread. They never persist runtime
parameter values into scene or controller YAML.

`create_model_instance` performs one atomic Engine-side model drop. With
`create_hierarchy = true`, the Engine creates the asset root and the editable
parent-before-child glTF hierarchy, applies each node's local TRS, and attaches
one mesh renderer using the source mesh index to every mesh node. With
`create_hierarchy = false`, or when skinning/non-decomposable node transforms
make an editable hierarchy unsafe, it creates one GameObject with one mesh
renderer using `meshIndex = -1` for the complete model. World mutation is
marshalled from the Editor worker to the Engine main thread; any failure rolls
back the owned root recursively before the command reports failure.

`set_editor_simulation` starts or stops physics preview for the current Editor
world. Start first serializes the authored native world into an in-memory
snapshot and only then enables fixed-step physics. Stop instantiates that
snapshot as a replacement Editor world, destroys the simulated world, and the
managed Editor refreshes its hierarchy and inspector from the restored native
state. Scene save and Play/Debug are rejected while simulation is active so
transient physics poses cannot be persisted. `get_editor_simulation_state`
queries the native authority; toolbar state is never inferred solely from a
previous button click. Both commands use the regular Editor worker and marshal
world mutations to the Engine main thread.

`preview_audio_asset` accepts an audio clip `fileId`, loads it through the
regular asset registry, and plays it as a non-spatial one-shot voice. Starting
another preview stops and destroys the previous preview voice. The Editor sends
this command asynchronously through the normal WebSocket transport.

Compatibility rules:

- Ordinary commands use baseline `protocol_version = 1`. Strict InstanceId
  restoration uses feature version `2`; the current host accepts version `2`
  only for `instantiate_prefab_from_yaml` with `strict_instance_ids = true`.
- Every current host response advertises
  `supports_strict_instance_ids = true`. A new Editor probes this additive
  capability over a baseline v1 request before sending a strict restore. An
  older v1 host omits the field, so ordinary v1 commands remain available but
  strict restore fails closed without sending the mutation.
- Never reuse a field number. Reserve removed fields and enum values.
- Keep `InstanceId` and `FileId` strings byte-for-byte compatible with their
  existing serialized forms.
- YAML fields are limited to dynamic reflected scene data that is already YAML
  inside the engine. Repository persistence formats are outside this protocol.
- C++ output belongs in `Runtime/Protocol/Generated`.
- C# output belongs in `Editor/Protocol/Generated`.

Generated files are checked in so a source checkout remains inspectable. Every
engine build generates C++ into its build directory and compares it byte for
byte with `Runtime/Protocol/Generated`. Every editor protocol build generates
C# into its `obj` directory and compares its SHA-256 hash with
`Editor/Protocol/Generated`. A mismatch fails the build without modifying the
checked-in output.
