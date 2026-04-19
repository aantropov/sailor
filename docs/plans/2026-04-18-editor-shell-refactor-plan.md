# Editor Shell Refactor Implementation Plan

Date: 2026-04-18
Project: Sailor Editor
Depends on: `docs/plans/2026-04-18-editor-shell-refactor-design.md`
Status: Substantially delivered; closed as "done enough" for v1 on 2026-04-19

## Status Update — 2026-04-19

This plan is no longer a strict forward-looking task list.

Delivered in the implementation that landed after this plan was written:
- shell foundation and YAML-first layout persistence
- shell skeleton with dockable panels and shell state rebuild fixes
- workflow/content/settings/AI slices integrated into the new shell
- scene viewport lifecycle isolation and scene focus/selection routing scaffolding
- hierarchy projection and inspector explicit-commit editing flow
- drag/drop command routing and destructive world/component undo restoration
- selected-object scene gizmos exposed through the world debug command list

Accepted scope cut for v1:
- selected-object gizmos are considered sufficient via the world debug command list path
- the originally implied full gizmo/manipulator framework with dedicated interactive transaction work is deferred

Still deferred follow-up work after this refactor milestone:
- fuller manipulator/tooling architecture beyond the current selected-object gizmo path
- more polish/test coverage around shell UX and scene interactions where needed
- any broader AI/editor ambitions beyond the integrated v1 shell panel/workflow slice

The rest of this document remains useful as implementation history and decomposition notes, but some tasks below were partially merged, reordered, or cut during delivery.

## Execution Mode

Recommended mode: subagent-driven implementation in the current session.

Principles:
- TDD-first for core architecture
- Small vertical slices with runnable checkpoints
- Frequent commits after green tests
- Delete obsolete legacy paths as soon as replacement slices are stable
- Keep the editor launchable throughout the migration

## Global Constraints

- Keep .NET MAUI as the UI stack
- Refactor editor shell fully inside the existing repo
- Preserve engine/editor bridge value, but move it behind the new command/service boundaries
- Prioritize order:
  1. layout
  2. workflow
  3. interaction
- Do not implement AI v2 creative/code workflows before AI v1 editor-operator flow is stable
- Do not implement layout Undo/Redo in v1

## Deliverables

1. New shell architecture
2. Docking/tab/floating/resizing/layout persistence support
3. Panel registry and lifecycle
4. Command bus and action context
5. Undo/Redo for editor/domain actions
6. Stable workflow slice for hierarchy + inspector + selection
7. Scene panel integrated into the new shell
8. Unified settings subsystem
9. Content/project workflows migrated
10. AI operator panel v1

## Phase 0 — Foundation / Carve-Out

### Objective
Introduce the new architectural foundation without changing user-visible behavior significantly.

### Task 0.1 — Create architecture folders and namespaces
Expected effort: 2–5 minutes

Do:
- Create new folders/namespaces for:
  - Shell
  - Panels
  - Layout
  - Commands
  - History
  - State
  - Settings
  - AI
- Keep naming consistent with existing Editor project conventions

Verify:
- Project builds
- No existing editor startup regression

Commit:
- `editor: add shell architecture namespaces`

### Task 0.2 — Define panel contracts
Expected effort: 2–5 minutes

Do:
- Add contracts for:
  - PanelId
  - PanelTypeId
  - PanelDescriptor
  - panel role (tool/document)
  - singleton/multi-instance metadata
  - persistence hooks

Verify:
- Unit tests for descriptor/identity basics
- Project builds

Commit:
- `editor: add panel contracts`

### Task 0.3 — Define layout model contracts
Expected effort: 2–5 minutes

Do:
- Add layout model types for:
  - root layout
  - split node
  - tab group node
  - floating window node
- Add version field to persisted layout model

Verify:
- Unit tests construct and serialize minimal layouts

Commit:
- `editor: add layout model contracts`

### Task 0.4 — Define command system contracts
Expected effort: 2–5 minutes

Do:
- Add interfaces/models for:
  - command
  - command result
  - undoable command
  - command origin
  - action context
  - transaction scope abstraction

Verify:
- Unit tests for contract-level assumptions

Commit:
- `editor: add command contracts`

### Task 0.5 — Define history contracts
Expected effort: 2–5 minutes

Do:
- Add undo/redo history interfaces/models
- Add history entry description requirements
- Add merge/coalesce policy abstractions

Verify:
- Unit tests for push/pop/clear semantics on stub history

Commit:
- `editor: add history contracts`

### Task 0.6 — Define unified settings contracts
Expected effort: 2–5 minutes

Do:
- Add settings scope model:
  - Project
  - Editor/User
  - Engine/Runtime
- Add category/item abstractions and validation result models

Verify:
- Unit tests for scope/category traversal and validation stubs

Commit:
- `editor: add settings contracts`

### Task 0.7 — Document current dependencies and migration mapping
Expected effort: 2–5 minutes

Do:
- Capture current `MainPage`, views, services, and bridge dependencies
- Note which existing views will first be adapted as panel content
- Note legacy hotspots expected to be removed later

Verify:
- Mapping doc added near the plan or in implementation notes

Commit:
- `docs: map legacy editor dependencies for shell refactor`

### Phase 0 exit criteria
- Editor builds
- New contracts exist
- Initial unit tests pass
- No behavior change required yet

## Phase 1 — New Shell Skeleton

### Objective
Replace hardcoded page composition with a shell host and real panel/layout infrastructure.

### Task 1.1 — Introduce shell host page/window
Expected effort: 2–5 minutes

Do:
- Create `EditorShellPage` or equivalent host
- Move top-level composition responsibility out of `MainPage.xaml`
- Preserve app startup path

Verify:
- Editor launches into new shell host

Commit:
- `editor: add shell host page`

### Task 1.2 — Add panel registry implementation
Expected effort: 2–5 minutes

Do:
- Implement runtime panel registry
- Register core panel types:
  - Scene
  - Hierarchy
  - Inspector
  - Project/Content
  - Console
  - Settings
  - AI

Verify:
- Unit tests for panel registration and lookup
- Registry resolves descriptors at startup

Commit:
- `editor: implement panel registry`

### Task 1.3 — Add shell state for open/focused panels
Expected effort: 2–5 minutes

Do:
- Track open panels
- Track focused panel
- Track active document tab group

Verify:
- Unit tests for open/focus/close transitions

Commit:
- `editor: add shell panel state`

### Task 1.4 — Implement layout tree operations
Expected effort: 2–5 minutes

Do:
- Implement operations for:
  - split
  - tab insertion
  - undock
  - redock
  - close/hide
  - floating window creation

Verify:
- Unit tests for tree transformations

Commit:
- `editor: implement layout tree operations`

### Task 1.5 — Implement panel resizing behavior
Expected effort: 2–5 minutes

Do:
- Add split resizing support
- Enforce min/max constraints
- Preserve ratios on hide/show/layout updates

Verify:
- Unit tests for ratio changes and constraints
- Manual verification of resizing in shell UI

Commit:
- `editor: add dock layout resizing`

### Task 1.6 — Add layout persistence
Expected effort: 2–5 minutes

Do:
- Save current layout to versioned file
- Restore layout on startup
- Fallback to default layout on failure

Verify:
- Unit tests for serialization/deserialization/fallback
- Manual restart preserves layout

Commit:
- `editor: add layout persistence`

### Task 1.7 — Adapt existing core views into registered panels
Expected effort: 2–5 minutes

Do:
- Wrap current views as panel content hosted by the shell
- Remove direct fixed-page placement responsibility from `MainPage`

Verify:
- Core panels render in shell
- Open/close/reopen works

Commit:
- `editor: host legacy core views in shell panels`

### Task 1.8 — Add menu/toolbar/status integration to shell
Expected effort: 2–5 minutes

Do:
- Move menu/toolbar/status to shell-driven composition
- Wire panel open/show actions through shell commands

Verify:
- Window menu can reopen panels
- Toolbar/status render through shell host

Commit:
- `editor: move shell surfaces into new host`

### Phase 1 exit criteria
- Core panels open inside shell
- Panels dock/tab/resize/hide/reopen
- Layout persists across restart
- Editor remains launchable

## Phase 2 — Command Bus + Undo/Redo Backbone

### Objective
Move editor mutations into a unified command architecture.

### Task 2.1 — Implement command dispatcher
Expected effort: 2–5 minutes

Do:
- Add command dispatch pipeline
- Support handler resolution and command origin metadata

Verify:
- Unit tests for dispatch and handler lookup

Commit:
- `editor: implement command dispatcher`

### Task 2.2 — Implement action context provider
Expected effort: 2–5 minutes

Do:
- Centralize:
  - active world
  - active selection
  - active panel
  - active document
  - focused viewport
  - origin metadata

Verify:
- Unit tests for context snapshots

Commit:
- `editor: add action context provider`

### Task 2.3 — Implement undo/redo history stack
Expected effort: 2–5 minutes

Do:
- Add undo stack
- Add redo stack
- Clear redo on new forward mutation
- Expose readable history labels

Verify:
- Unit tests for undo/redo stack behavior

Commit:
- `editor: implement undo redo history`

### Task 2.4 — Implement transaction support
Expected effort: 2–5 minutes

Do:
- Add single-command, compound-command, and interactive transaction support
- Support merge/coalesce policies

Verify:
- Unit tests for grouped history behavior

Commit:
- `editor: add command transactions`

### Task 2.5 — Migrate object lifecycle commands
Expected effort: 2–5 minutes

Do:
- Implement and route:
  - CreateGameObject
  - DestroyObject
  - DuplicateSelection if available
  - SelectObject where needed for flow completeness

Verify:
- Integration tests for create/delete and undo/redo

Commit:
- `editor: migrate object lifecycle commands`

### Task 2.6 — Migrate hierarchy mutation commands
Expected effort: 2–5 minutes

Do:
- Implement and route `ReparentObject`

Verify:
- Integration tests for reparent + undo/redo + hierarchy refresh

Commit:
- `editor: migrate reparent command`

### Task 2.7 — Migrate component commands
Expected effort: 2–5 minutes

Do:
- Implement and route:
  - AddComponent
  - RemoveComponent
  - ResetComponentToDefaults
  - property update commands

Verify:
- Integration tests for add/remove/update/reset + undo/redo

Commit:
- `editor: migrate component commands`

### Task 2.8 — Wire Undo/Redo into menu/toolbar/hotkeys
Expected effort: 2–5 minutes

Do:
- Add shell command descriptors for undo/redo
- Bind to menu/toolbar and platform hotkeys

Verify:
- Manual validation in running editor

Commit:
- `editor: wire undo redo into shell`

### Phase 2 exit criteria
- Core world/component mutations go through commands
- Undo/Redo is stable for core actions
- Readable history labels exist

## Phase 3 — Hierarchy + Inspector + Selection Slice

### Objective
Make the core authoring loop command-driven and state-driven.

### Task 3.1 — Centralize selection state
Expected effort: 2–5 minutes

Do:
- Create central selection store/projection
- Remove fragmented selection truth from individual views where possible

Verify:
- Integration tests for selection changes propagating to hierarchy and inspector

Commit:
- `editor: centralize selection state`

### Task 3.2 — Build hierarchy projection
Expected effort: 2–5 minutes

Do:
- Build hierarchy projection from world state
- Track expanded/collapsed state separately from world truth

Verify:
- Unit/integration tests for projection updates on create/delete/reparent

Commit:
- `editor: add hierarchy projection`

### Task 3.3 — Route hierarchy selection and context actions through commands
Expected effort: 2–5 minutes

Do:
- Make click/context menu actions dispatch commands instead of mutating directly

Verify:
- Integration tests for select/create/delete flows

Commit:
- `editor: route hierarchy actions through commands`

### Task 3.4 — Route hierarchy drag-and-drop through reparent command
Expected effort: 2–5 minutes

Do:
- Replace direct DnD mutation path with command dispatch

Verify:
- Integration tests for DnD reparent and undo/redo

Commit:
- `editor: route hierarchy drag drop through commands`

### Task 3.5 — Build inspector projection
Expected effort: 2–5 minutes

Do:
- Separate display/edit metadata from raw UI code
- Build selected object/component projection

Verify:
- Integration tests for selection -> inspector updates

Commit:
- `editor: add inspector projection`

### Task 3.6 — Route inspector edits through property/component commands
Expected effort: 2–5 minutes

Do:
- Move inspector field edits to command dispatch
- Support transaction grouping for repeated/interactive edits where practical

Verify:
- Integration tests for edits and undo/redo

Commit:
- `editor: route inspector edits through commands`

### Phase 3 exit criteria
- Selection synchronization is stable
- Hierarchy and inspector are command-driven
- Undo/Redo is reliable for the main authoring loop

## Phase 4 — Scene Panel Integration

### Objective
Move the viewport-heavy scene workflow into the new shell architecture cleanly.

### Task 4.1 — Register scene as proper panel/document type
Expected effort: 2–5 minutes

Do:
- Integrate scene view with panel lifecycle
- Ensure scene panel can open/focus/close/reopen in shell terms

Verify:
- Manual shell validation

Commit:
- `editor: integrate scene panel into shell`

### Task 4.2 — Isolate viewport lifecycle adapter
Expected effort: 2–5 minutes

Do:
- Wrap remote viewport session lifecycle behind a shell-friendly adapter
- Avoid leaking raw lifecycle logic into panel UI code

Verify:
- Unit/integration tests for viewport create/update/destroy transitions

Commit:
- `editor: isolate scene viewport lifecycle adapter`

### Task 4.3 — Implement scene focus/input ownership model
Expected effort: 2–5 minutes

Do:
- Track focused viewport and keyboard/input ownership
- Integrate with shell activation state

Verify:
- Integration tests for focus transitions

Commit:
- `editor: add scene input ownership model`

### Task 4.4 — Route picking/selection through central command flow
Expected effort: 2–5 minutes

Do:
- Connect scene picking to central selection/command architecture

Verify:
- Integration tests for scene pick -> hierarchy/inspector sync

Commit:
- `editor: route scene selection through command flow`

### Task 4.5 — Add transform/gizmo interactive transactions
Status: deferred by accepted v1 scope cut on 2026-04-19
Expected effort: 2–5 minutes

Original intent:
- Represent transform manipulations as interactive transactions
- Ensure undo produces one meaningful history action

V1 replacement that actually shipped:
- selected-object gizmos are exposed through the world debug command list
- this is the current "done enough" completion point for scene gizmos in the shell refactor milestone

Deferred follow-up:
- dedicated manipulator architecture
- richer gizmo interaction model and transaction grouping/tests

### Phase 4 exit criteria
- Scene lives as a real shell panel
- Viewport survives layout/focus changes
- Scene selection integrates with the shared command/history model
- selected-object gizmos are available through the debug-command path

## Phase 5 — Project/Content + Unified Settings

### Objective
Migrate content workflows and deliver unified settings.

### Task 5.1 — Build project/content projection
Expected effort: 2–5 minutes

Do:
- Separate asset tree/list projection from direct view concerns
- Track current folder, sorting, filtering, and asset selection state

Verify:
- Unit tests for folder/content projection behavior

Commit:
- `editor: add project content projection`

### Task 5.2 — Route asset open/create/rename/delete through commands
Expected effort: 2–5 minutes

Do:
- Convert core content operations to command handlers

Verify:
- Integration tests for command-driven content operations

Commit:
- `editor: route content operations through commands`

### Task 5.3 — Route asset drag-and-drop through typed commands
Expected effort: 2–5 minutes

Do:
- Support drag asset to scene/inspector through resolved typed command flows

Verify:
- Integration tests for asset drop scenarios

Commit:
- `editor: route asset drag drop through commands`

### Task 5.4 — Implement unified settings store and scope resolution
Expected effort: 2–5 minutes

Do:
- Build settings store with Project / Editor / Engine scopes
- Support effective value resolution and validation

Verify:
- Unit tests for scope resolution and validation

Commit:
- `editor: implement unified settings store`

### Task 5.5 — Build settings UI panel
Expected effort: 2–5 minutes

Do:
- Add searchable settings UI with categories and scope switching
- Support dirty state and apply/revert where required

Verify:
- Integration/manual tests for editing and persistence

Commit:
- `editor: add unified settings panel`

### Phase 5 exit criteria
- Content workflows are command-driven
- Unified settings exists with three scopes
- Basic drag/drop flows are stable

## Phase 6 — AI Operator v1

### Objective
Ship AI as a native panel that can operate the editor through the same command system.

### Task 6.1 — Add AI panel shell integration
Expected effort: 2–5 minutes

Do:
- Register AI panel
- Add panel UI shell and basic conversation state

Verify:
- AI panel opens/closes/docks normally

Commit:
- `editor: add ai operator panel`

### Task 6.2 — Build editor context feed for AI
Expected effort: 2–5 minutes

Do:
- Provide active selection/document/world/panel context snapshot to AI layer

Verify:
- Unit tests for context shaping

Commit:
- `editor: add ai editor context feed`

### Task 6.3 — Add action preview model and approval flow
Expected effort: 2–5 minutes

Do:
- Represent proposed actions as previewable command batches
- Support safe/confirm-required classification

Verify:
- Unit tests for approval gating and classification

Commit:
- `editor: add ai action preview and approvals`

### Task 6.4 — Execute approved AI actions through command bus
Expected effort: 2–5 minutes

Do:
- Route approved AI action batches through standard handlers
- Tag history/origin as AI-generated

Verify:
- Integration tests for AI-origin reversible commands participating in undo/redo

Commit:
- `editor: execute ai actions through command bus`

### Task 6.5 — Add AI audit/result feedback surface
Expected effort: 2–5 minutes

Do:
- Show executed actions, failures, and results in AI UI and/or console

Verify:
- Manual validation for traceability

Commit:
- `editor: add ai action audit feedback`

### Phase 6 exit criteria
- AI panel is native to the shell
- AI can inspect context
- AI can preview and execute approved editor actions through the standard command path
- Reversible AI actions participate in Undo/Redo

## Cross-Phase Test Matrix

### Unit test focus areas
- Layout tree transformations
- Panel registry and restore behavior
- Command dispatch and handler resolution
- Undo/Redo stack semantics
- Transaction grouping/merge behavior
- Settings scope resolution
- AI preview/approval/execution routing

### Integration test focus areas
- hierarchy <-> inspector selection synchronization
- create/delete/reparent flows
- component add/remove/update flows
- layout restore to expected panels
- scene selection updates shared state
- viewport lifecycle under focus/layout changes
- asset drag/drop routing
- AI action batch execution through command handlers

### Manual validation per phase
- app startup/shutdown
- docking/tabbing/floating
- panel resizing
- layout persistence
- selection consistency
- undo/redo sanity
- Mac-first behavior with Windows architecture preserved

## Cleanup Rule

At the end of each phase:
- remove dead or bypassed legacy paths if the replacement slice is stable
- do not leave duplicate mutation paths longer than necessary
- keep the migration legible through small commits and updated notes

## Suggested Early Subagent Breakdown

When executing this plan, use focused subagents for tasks such as:
- layout model and persistence
- command/history core
- hierarchy/inspector slice
- scene viewport integration
- settings subsystem
- AI operator integration

Each subagent task should include:
- exact files in scope
- explicit non-goals
- required tests
- success verification commands

## Recommended Next Execution Step

Start with:
1. Phase 0 / Task 0.1
2. Phase 0 / Task 0.2
3. Phase 0 / Task 0.3
4. Phase 0 / Task 0.4

This gives the minimum architectural skeleton needed before touching shell UI composition.
