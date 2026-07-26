# Editor/Engine Protocol

`editor_engine.proto` is the single wire contract between SailorEditor and
SailorLib. The editor and engine exchange only serialized protobuf envelopes
through a fixed-width C transport ABI; generated language objects never cross
the shared-library boundary.

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
