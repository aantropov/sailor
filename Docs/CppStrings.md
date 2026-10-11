# C++ strings

Use the same distinction throughout Sailor: identifiers, borrowed text and
owned text are different contracts.
The rules also apply to test helpers: assertion messages can be views on the
successful path, with an owned copy only when constructing an exception.

Choose the type at the API boundary, not just at a call site: fixed engine
identifiers use `"Name"_h`, immediate text readers take `std::string_view`, and
stored or edited text owns its bytes. Do not add an implicit string-to-hash
conversion to keep old call sites compiling.

## Identifiers

Use `StringHash` by value for names used as keys or retained labels. Constant
names use the `_h` literal:

```cpp
auto upload = Tasks::CreateTask("Independent model-test buffer upload"_h,
	[] { /* Record the upload. */ }, EThreadType::RHI);
```

Pass the literal directly to the identifier API. Do not wrap a fixed name in
`std::string` or `StringHash::Runtime`, or call `ToString()` just to convert it
back to an identifier in the next helper.

For a conditional fixed label, select identifiers rather than selecting text
and hashing it at runtime: `bUpload ? "Upload mesh"_h : "Prepare mesh"_h`.
Do not build a dotted `binding.member` string when both identifiers are already
available; use the two-identifier overload instead.

The literal computes its hash at compile time. Its first runtime use registers
the readable name; subsequent uses do not hash the text or access the string
table. A task stores the identifier, not a separate string allocation. Resolve
the text only where it is needed for profiling, logging or serialization.

Convert dynamic identifiers at their ownership boundary with
`StringHash::Runtime(text)`. The registry owns a copy, so the caller's text may
go out of scope. Do not intern arbitrary log messages, file contents or other
unbounded text. A `constexpr` hash used only for comparison does not register
text; use a runtime `_h` value when its readable name must be retained.
The same rule applies to static or thread-local hashes that undergo constant
initialization: a stored numeric hash alone cannot recover unregistered text.
Sentinel IDs such as `FileId::Invalid` register their canonical literal when
their text is requested, so constant initialization does not change serialization.

Reuse the converted identifier when one imported name feeds several runtime
objects; do not call `StringHash::Runtime` again for each consumer. When only a
numeric hash is required and no readable name is retained, use `HashString(view)`
instead. It produces the same hash without touching the string registry. Render
queue comparisons use this path; fixed numeric tags use `"Opaque"_h.GetHash()`.
Console command lookup follows the same rule: hash the borrowed input without
interning unknown commands, and define fixed command keys with `_h`.

Scheduler worker names are retained identifiers too. Fixed labels use `_h`;
numbered worker labels are interned once at scheduler initialization. Thread
diagnostic names return `StringHash`, with the generated thread-ID label cached
on that thread. Resolve the text only at the OS, profiler or GPU-debug boundary.

GPU timestamp labels are identifiers too. Pass `"Lighting"_h` to
`BeginGpuTimestamp`; query records, rolling histories and published snapshots
retain the hash, and the overlay resolves it to text when drawing. Frame-graph
nodes cache composite labels instead of formatting and hashing them each frame.
Changing a node's tag, shader or graph position refreshes that label without
changing the readable names held by already submitted queries.

## Borrowed text

Use `std::string_view` by value for read-only string parameters. It accepts
literals, owned strings and bounded substrings without allocating a temporary
string. A view is not necessarily null-terminated; pass its size to APIs that
accept a length. Do not turn a view into a C string by assuming `data()` ends at
the view's boundary.

Keep views returned by APIs such as `magic_enum::enum_name` as views instead of
copying them into a temporary `std::string`. For printf-style output, use
`%.*s` with `static_cast<int>(view.size())` and
`view.empty() ? "" : view.data()`; a default-constructed view has a null pointer.
Do not construct a temporary string just to print an enum name with `c_str()`.
Serialization may still copy that name into an owned YAML/JSON value.

Views returned from parsing must have an explicit live owner. Prefer returning
owned strings when a caller may pass a temporary. A queued task must capture
owned text or an interned identifier, not a view of a local string.
Consume a view of a temporary within the same full expression. Do not store
that view in one statement and read it in the next. `SAILOR_PROFILE_TEXT`
consumes its argument synchronously, before temporary owners are destroyed.
For in-place edits, pattern/replacement views must not borrow the destination
buffer: an edit can reallocate it or overwrite the pattern.

Use `Utils::TrimView(text)` when parsing an existing buffer. The result borrows
that buffer; `Utils::Trim(ownedText)` remains the in-place alternative. A parser
should keep substrings as views and allocate only where an API actually needs
owned, null-terminated text, such as the argument to `strtod`.

FrameGraph `TryGetString` supports both forms: a `std::string_view` output for
immediate reads and a `std::string` output for an owned snapshot. Reacquire a
view after changing node parameters; do not use it after destroying the node.
Both overloads leave the output unchanged when the parameter is absent.

Task labels, frame-graph node/resource/parameter names, shader bindings and
members, shader entry-point names, material parameters and compiled animation
lookups use `StringHash`. For a constant binding/member pair, pass both
identifiers directly instead of parsing a dotted name on every update:

```cpp
commands->SetMaterialParameter(cmd, bindings, "material"_h, "albedo"_h, color);
bindings->HasParameter("material"_h, "albedo"_h);
animator->SetFloat("Speed"_h, speed);
```

Importers convert names from authored text when building runtime state. Editable
asset metadata stays textual. Object creation takes a view and copies the name
into the object; arbitrary object names are not interned for the process lifetime.

## Owned text and external APIs

Keep `std::string` for editable values, generated output, serialization payloads
and retained text without a stable external owner. Avoid unnecessary copies
between readers of the same value; use views for those readers.

YAML field lookup borrows its key as a view. Temporary validation sets may also
borrow scalar text from the live document instead of copying each key. Returned
diagnostics still own their field names because they can outlive the document.
After checking `IsScalar()`, use `Scalar()` for an immediate read; use
`as<std::string>()` when the caller needs its own copy. A set of borrowed scalar
views must not survive replacement or destruction of its source document.
When copying a scalar into an owned protobuf field, pass `Scalar()` directly;
do not first construct an intermediate `std::string`. Text-to-enum parsers take
views and allocate diagnostic text only on failure.
The shared YAML serializers accept field names as views. Reflected field names
borrow static refl-cpp metadata; YAML nodes and exported reflection metadata
own their stored names. Enum readers borrow YAML scalars or JSON string
references, and only error messages allocate diagnostic text. MSVC builds use
`/Zc:__cplusplus` so dependency headers expose their C++17 string-view support
under the project's C++20 language mode. This is a public `Sailor::Runtime`
compile option, so consuming workspace modules use the same header interfaces.
Asset field-name normalization also returns a borrowed view. Its callers use
refl-cpp's stable display names; callers passing their own text must retain that
text while using the result. Metadata builders accept filename views and copy
them directly into their returned YAML nodes.

Settings source labels, validation field lists and cache identity inputs also
use views. Cache readers borrow scalar values from the live YAML document while
validating them; returned identities, payloads and diagnostics own their text.
Cache updates borrow their input text and copy it directly into retained entry
fields. Build diagnostic context only on failure; a successful shader-cache
validation must not concatenate a separate label for every artifact.
AssetRegistry content reads, path resolution and asset lookups borrow paths as
views throughout the call chain. Normalization owns its mutable buffer; returned
paths and stored records also own their text. Constant shader and texture paths
can therefore be passed directly without constructing a temporary `std::string`.
Paths remain text: do not put arbitrary filesystem names in the identifier table.
Mount lookup follows the same rule: borrow the path and make one mutable copy
for normalization. Construct diagnostics with `std::format`, passing enum-name
views directly instead of concatenating temporary strings.
Similarly, an asset-drop event copies its borrowed FileId into the typed queue
payload before returning. Native selection and viewport events retain typed
values and are serialized only at the protobuf boundary. None of these APIs
retains the caller's view.

Workspace manifest readers trim scalar views before copying into retained
fields. Path normalization owns only the buffer it edits; UTF-8 path conversion
constructs the native path from the bounded character range without an
intermediate string. Default paths and serialized field names remain text views,
not interned identifiers.

Editor messages borrow their input text; the queue owns the formatted message.
Interop output copies exactly the view's bytes and adds its own terminator.
The YAML emitter requires an owned string, so test-journal messages keep their
`const std::string&` boundary. A view followed by an unconditional copy for the
emitter would add work for callers that already own the message.

The `_h` suffix is for identifiers, not a universal replacement for text.
GPU debug-name and debug-region helpers accept `_h` for fixed labels; their text
overloads remain available for generated descriptions without interning them.
Resolve a debug identifier to text inside the backend, only when its debug call
is compiled in. Keep the RHI request observable to command recorders; a disabled
Vulkan debug operation must not look up the string table.
YAML/JSON keys, printf format strings, filesystem paths, shader source and
third-party APIs still require text. Use literal C strings at C APIs and
borrowed views where supported. Preserve readable asset/settings/protocol
formats; do not write numeric hashes in place of existing text fields.

Keep an existing `const std::string&` at a boundary that needs `c_str()` when
the callers already own strings. Taking a view only to copy it into another
string for null termination adds work rather than removing it.

For a retained identifier passed to a C API, resolve its registered text at
that boundary: `entryPoint.ToString().c_str()`. The string registry owns the
text. Do not keep a separate string in every object holding the same identifier.
Synchronous text writers take views and write exactly their length, including
embedded zero bytes. Path-building helpers take views and return owned paths.
Names inside generated filenames remain readable text.

Shader compilation borrows GLSL source and passes its pointer and length to
shaderc. Extension checks, include-path matching and synchronous source rewrites
also accept bounded views. Shaderc's filename and entry-point arguments remain
null-terminated text; a fixed external entry point such as `"main"` does not need
interning. A default-constructed empty view is passed as an empty C string with
zero length, not as a null source pointer.

OS thread-name helpers borrow `std::string_view`. UTF-8-to-wide conversion reads
the view's length, not a trailing zero, and returns an owned wide string for the
OS call. Bounded views and embedded zero bytes are preserved by the converter;
the OS API itself still consumes a null-terminated name.

Wide-to-UTF-8 conversion also accepts `std::wstring_view`: pass the populated
range of a wide buffer, not a pointer to each character. The existing C-string
overload remains for terminated OS text. The bounded reader preserves embedded
zeros and complete surrogate pairs; invalid scalar values become U+FFFD.

Windows standalone startup converts the wide argv to owned UTF-8 strings once,
then builds pointers only after that storage is complete. Keep it alive through
App initialization and command execution. Platform argv and editor protocol
arguments are already tokenized: spaces and literal quotes inside a value are
data, not a command line to split or unquote again.

Use Workspace::PathFromUtf8 and PathToUtf8 where engine text crosses a native
filesystem-path boundary. On Windows, string()/generic_string() are not UTF-8
conversions. Keep filesystem operations on native paths and convert to UTF-8
for API text, diagnostics and retained module-owner names; do not introduce an
ANSI fallback or expose project cache paths through new load parameters.

Workspace containment uses `Workspace::IsPathWithin` on canonical native paths,
after resolving links and parent segments. Compare directory components, not
text prefixes or lowercased UTF-8 bytes. The shared policy is exact comparison
on POSIX and ordinal case-insensitive comparison on Windows; use it for both
workspace resolution and module loading.

Asset filenames, virtual paths, metadata fields and cache index keys use UTF-8
too. Decode them before filesystem operations, including relative path joins;
encode native paths when publishing them back into these text fields. The
AssetRegistry file helpers accept bounded UTF-8 text or an already-native path.
File contents remain bytes and are not transcoded with the filename. Keep
timestamp units, cache ownership and serialized versions unchanged.

Use a library's existing Unicode boundary when available: TinyGLTF accepts
UTF-8 filenames, stb enables them with STBI_WINDOWS_UTF8/STBIW_WINDOWS_UTF8,
and miniaudio provides a wide filename entry point on Windows. Do not add a
parallel file loader or an ANSI retry around those APIs.

Check the complete call chain when replacing a string parameter. A view that
is immediately copied into a string by the next helper has not removed the
allocation. Keep owned storage at the actual retention or mutation boundary.

When changing an API, update its producers, consumers and behavioral tests
together. Test dynamic-name lifetime, bounded/non-null-terminated views and
concurrent use where relevant. Do not test this policy by matching source text.
Consuming C++ workspace modules must update their identifier arguments and be
rebuilt against the new headers; old module binaries are not ABI-compatible.
