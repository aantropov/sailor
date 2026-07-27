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

Compatibility rules:

- Keep `protocol_version` at `1` for additive, wire-compatible changes.
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
