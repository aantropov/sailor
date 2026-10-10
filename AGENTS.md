# Sailor agent rules

## Repository

- C++/CMake engine; Windows x64 supports MSVC and clang. C#/.NET MAUI editor: `Editor/`.
- Dependencies: `External/renderdoc` and `External/vcpkg` submodules. After repository/submodule updates, run `python update_deps.py` from the root.
- C++ uses tabs; C# uses spaces. Branch names and Markdown use English.
- Put control-flow bodies on following lines with braces, including single statements; no inline `if (...) return ...;` or loop bodies.
- Preserve unrelated changes. One logical change per commit; stage explicit paths. Do not merge without approval.

## Code and data

- Prefer existing engine types, ownership and helpers over redundant wrappers, validation or dependencies. Comments explain non-obvious decisions.
- Create RHI/Vulkan resources only on Main, Render or RHI threads. Worker tasks may load, decode and prepare CPU data; hand GPU resource creation to an allowed queue.
- Keep MSAA in scene geometry, depth prepass and debug drawing. Fog, post-processing, particles and tracer composition use resolved 1x targets. Describe copies back to a later MSAA geometry pass explicitly in the render graph.
- Before public release, all project-owned asset/generated-data/settings/layout schemas, generator versions and editor/runtime protocols stay at version 1. Update producers/consumers together and regenerate derived data. No migrations, compatibility paths or version bumps unless explicitly requested.
- Never commit generated `.probes` or `.probes.asset` files; tests generate temporary payloads.
- Tests are opt-in (`SAILOR_BUILD_TESTS=ON`); CI enables them explicitly. Do not add standalone benchmark executables or benchmark build/CI steps.
- Test behavior, parsed configuration relationships or compiled/reflected interfaces. Never test raw source for identifiers/snippets/flags; if only source presence can be checked, do not add a test.

## C++ strings

- Identifiers/retained labels: `StringHash` by value; literals use `"Name"_h`. Convert dynamic identifiers once with `StringHash::Runtime`; numeric-only lookup uses `HashString(view)`. Do not intern unbounded text or add implicit string-to-hash conversions.
- Immediate read-only text: `std::string_view`; retained/mutable text: `std::string`. Queued work owns text or retains a hash. Never outlive or invalidate a view's owner.
- Views need not be null-terminated: pass lengths or provide owned terminated text at C boundaries. Keep `const std::string&` when callers already own the required C string. Avoid view-to-string copies in downstream readers.
- Paths, serialized keys/text, shader source and format strings remain text. Borrow YAML/reflection/enum text until ownership is needed; build diagnostics only on failure.
- Path text is UTF-8: use `Workspace::PathFromUtf8`/`PathToUtf8`, native filesystem operations and `IsPathWithin` for containment. No ANSI fallback. Convert Windows wide argv once; do not retokenize argv/protocol values.
- Update callers and relevant behavioral tests together; rebuild consuming C++ workspace modules.

## Workflow

- Board columns only: `Backlog`, `To Do`, `In Progress`, `Done`. Stages/owners/blockers use labels and comments.
- Stages: `needs-planning`, `planning`, `ready-for-implementation`, `implementation`, `needs-tech-review`, `tech-review`, `needs-qa`, `qa-validation`, `ready-for-human-review`.
- Owners: `agent-manager`, `agent-team-lead`, `agent-implementer`, `agent-tech-lead`, `agent-lead-qa`. States: `changes-requested`, `blocked`.
- Manager owns GitHub state/routing, not architecture or implementation unless assigned. Keep one stage and owner. Closed tickets require exactly one `done-rejected`, `done-fixed`, `done-duplicate`, `done-implemented` or `done-cannot-reproduce`; no `done-*` before closure.
- Team Lead defines scope/non-goals, approach, risks, acceptance and tests. Implementer follows the approved plan and explains deviations. Tech Lead reviews architecture/lifetime/threading. QA validates behavior and records failures/skips/limits. Humans decide merge.
- Each handoff posts a concise issue/PR comment: result, evidence, risks and next action. No QA stage before Tech Lead approval. Only Manager marks human-review readiness after QA approval or a documented human-approved exception; readiness cannot coexist with blocked/changes-requested.

## Context budget

Durable agent rules live here. `Docs/` is local reference material: do not stage or commit its changes unless explicitly requested. Do not delete already-tracked documents merely to hide local edits.
Read only task-relevant references. Keep current-state notes short and link historical evidence instead of reloading/appending full session reports. `Docs/Plans/` remains Git-ignored.
