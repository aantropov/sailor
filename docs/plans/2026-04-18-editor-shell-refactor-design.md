# Editor Shell Refactor Design

Date: 2026-04-18
Project: Sailor Editor
Status: Approved design; implementation largely landed by 2026-04-19

## Closure Note — 2026-04-19

This design was directionally correct, but the shipped result is intentionally narrower than some of the original wording implied.

What actually landed:
- MAUI editor shell refactor with persistent dock/tab layout foundations
- command/history-based workflow slices across hierarchy, inspector, content, settings, AI, and scene integration
- scene viewport lifecycle and selection/focus routing updates
- selected-object scene gizmos via the world debug command list

What v1 explicitly does not claim yet:
- a full standalone gizmo/manipulator framework
- dedicated interactive gizmo transaction architecture matching the original idealized Phase 4 wording
- every planned polish/testing item from this document

For repo status and the accepted v1 scope cut, treat the implementation plan as the authoritative closure note.

## Goal

Refactor the current Sailor editor UI into a mature desktop-grade editor shell, still based on .NET MAUI, with Unity-like workflow and interaction quality.

Primary goals:
- Docking windows
- Tab groups
- Floating windows
- Panel resizing
- Saved/restored layouts
- Toolbar
- Status bar
- Unified Settings experience
- Native AI panel integrated into editor workflows
- Real command architecture with Undo/Redo

## Product Direction

### Chosen direction
- Keep MAUI as the UI stack
- Allow a full refactor of the editor shell and surrounding editor architecture
- Prioritize delivery order:
  1. Layout
  2. Workflow
  3. Interaction

### UX target
Functional Unity-grade shell, not just visual resemblance.

Must-have capabilities in v1:
- Dock targets
- Tab groups
- Undock/redock
- Floating windows
- Panel resizing
- Saved layouts
- Toolbar
- Status bar
- Unified Settings entry point

### AI target
The AI subsystem is a native part of the editor shell.

Delivery path:
- Phase A: AI as editor operator, capable of performing editor operations through commands/APIs
- Phase B: AI evolves into a full agent workspace for creative tasks, asset generation, code assistance, and broader task execution

For v1, start with editor operations first.

## Architectural Direction

### Chosen approach
Approach B: full editor shell refactor inside MAUI.

Implementation style:
- Hybrid / domain-first with vertical slices
- Build proper editor core first
- Deliver in phased, runnable slices

### Core principle
The editor should become:
- Shell
- Panels
- Command bus
- Projections/state
- Undo/Redo
- Settings subsystem
- AI operator layer

Not a hardcoded page composed from a fixed XAML grid.

## Current State Assessment

The current editor is built around a fixed page layout in `Editor/MainPage.xaml`, with hardcoded regions for:
- ContentFolderView
- HierarchyView
- InspectorView
- SceneView
- ConsoleView

This confirms that the current editor composition must be replaced by a real shell/layout system.

The existing engine bridge already provides valuable editor operations and viewport/session functionality, including:
- remote viewport lifecycle
- remote viewport input
- object create/delete/reparent
- component add/remove/reset
- prefab instantiation
- world/object update

That bridge is a strong foundation, but it should sit beneath the command layer instead of being called ad hoc from UI.

## High-Level Architecture

The refactored editor should be divided into the following core subsystems:

1. Editor Shell
2. Panel System
3. Editor State + Command Bus
4. Undo/Redo + Command History
5. Domain Services + Engine Bridge
6. Unified Settings System
7. AI Operator Layer

### 1. Editor Shell
Responsibilities:
- Main editor host window
- Toolbar
- Status bar
- Menu bar
- Docking areas
- Floating windows
- Layout loading/saving/restoring/resetting

### 2. Panel System
Panels are first-class modules registered with the shell.

Panel lifecycle:
- create
- show
- focus
- hide
- close
- persist/restore

Two panel classes:

#### Tool Panels
Examples:
- Hierarchy
- Inspector
- Console
- Project/Content
- AI
- Settings

Characteristics:
- Usually singleton by default
- Dockable/tabbable/floating
- Can be shown/hidden from menus
- Persisted across sessions

#### Document Panels
Examples:
- Scene
- Game
- Material editor
- Shader graph
- Asset editor
- Prefab editor

Characteristics:
- Multiple instances allowed where appropriate
- Active document semantics
- Reopenable/persistable where relevant
- Dirty/save lifecycle where relevant

### 3. Editor State + Command Bus
UI, hotkeys, menus, drag-and-drop, and AI do not mutate editor state directly.
All write operations should flow through commands.

Command sources:
- Toolbar
- Menu items
- Hotkeys
- Context menus
- Drag-and-drop interactions
- Panel-local UI actions
- AI operator
- Future scripting/macros/tests

### 4. Undo/Redo + Command History
Undo/Redo is core architecture, not a later add-on.

In v1, Undo/Redo covers editor/domain actions, but not layout operations.

Undoable examples:
- object create/delete
- reparent
- duplicate
- component add/remove/reset
- property changes
- asset metadata edits where meaningful
- settings changes where feasible

Not in Undo/Redo v1:
- layout moves/docking changes
- focus changes
- viewport navigation
- transient UI state

Layout history in v1 uses:
- Save Layout
- Restore Layout
- Reset Layout
- Layout presets

### 5. Domain Services + Engine Bridge
The engine interop layer remains essential, but should be wrapped by editor-facing services and handlers.

Expected layering:
- command handler
- domain service
- engine bridge
- state/projection refresh
- history record if reversible

### 6. Unified Settings System
Settings are one unified subsystem, not separate disconnected windows.

Single entry point:
- Settings

Scopes:
- Project
- Editor/User
- Engine/Runtime

This should provide one coherent experience with categories, search, validation, and persistence.

### 7. AI Operator Layer
AI is a native editor subsystem.

v1 AI responsibilities:
- inspect current editor context
- propose editor action plans
- preview actions
- require approvals where needed
- execute through the same command bus used by the rest of the editor
- surface results and diagnostics

AI must not bypass the command system.

## Panel and Layout Model

### Layout model
The editor shell should use a persistent layout tree rather than a hardcoded page grid.

Core layout nodes:
- RootLayout
- SplitNode
- TabGroupNode
- FloatingWindowNode

#### SplitNode
- orientation: horizontal/vertical
- children
- size ratios
- min/max constraints

#### TabGroupNode
- list of panel instances
- active tab
- role: tool/document

#### FloatingWindowNode
- separate window bounds
- nested layout subtree

### Resizing
Panel resizing is mandatory in v1.
Requirements:
- Split ratios adjustable with mouse
- Min/max constraints
- Stable behavior when panels move/show/hide
- Persist size ratios after restart
- Avoid broken zero-size layouts

### Docking requirements in v1
Supported:
- drag panel tab
- dock left/right/top/bottom/center
- create tab groups
- move panel between groups
- undock into floating window
- redock floating window
- close/hide panel
- reopen hidden panel
- save layout
- restore layout
- reset layout

### Layout persistence
Persist layout in a versioned serializable format (JSON or YAML).

Should include:
- layout version
- main window bounds
- layout tree
- active tabs
- floating windows
- panel instance metadata
- reopenable documents

Must support:
- migration path for layout schema changes
- safe fallback to default layout when restore fails

### Layout presets
Initial presets should support:
- Default
- Authoring
- Debug
- AI

## Panel Registry

A panel registry should define all panel types and instance behavior.

Each panel instance should be represented by:
- PanelId (instance)
- PanelTypeId (type)
- PanelDescriptor

PanelDescriptor should capture:
- title
- icon
- singleton vs multi-instance
- tool vs document
- default docking preference
- factory for view + viewmodel
- persistence hooks

This enables:
- menu-driven panel opening
- layout restore
- future panel extensibility
- AI/window commands like opening or focusing panels

## Focus and Activation Model

The shell should explicitly track:
- focused panel
- active tab group
- active document
- primary scene viewport
- selection owner
- keyboard input owner

This is required for:
- hotkeys
- delete/duplicate actions
- context-sensitive commands
- viewport input routing
- AI context interpretation

## Workflow Panels and Ownership

### Hierarchy
Responsibilities:
- tree view of world/game objects
- selection
- expand/collapse
- reparent by drag-and-drop
- rename
- create/delete/duplicate entry actions

Owns:
- hierarchy projection state only

Does not own:
- world truth
- mutation logic
- selection source of truth

### Inspector
Responsibilities:
- present selected object details
- component editing
- add/remove/reset component
- validation/errors

Owns:
- inspector projection and editing state

Does not own:
- direct mutation logic
- undo logic
- raw engine serialization hacks in UI

### Scene
Responsibilities:
- interactive viewport
- camera navigation
- picking
- transform/gizmo interactions
- scene overlays later
- viewport mode controls

Owns:
- viewport-local transient interaction state

Does not own:
- global selection truth alone
- persistent mutations outside commands

### Game
Responsibilities:
- runtime/game preview output
- later play/debug overlays

### Project/Content
Responsibilities:
- folder tree and asset browser
- import/create/move/rename/delete assets
- drag asset references into scene/inspector
- open asset documents

Owns:
- content browser projection only

### Console
Responsibilities:
- logs/warnings/errors
- filtering/search
- command and engine feedback surface
- AI action diagnostics later

### Toolbar
Responsibilities:
- global high-frequency commands
- active tool presentation
- undo/redo entry point
- save/play/layout shortcuts where appropriate

Toolbar is a command surface, not a business-logic owner.

### Status Bar
Responsibilities:
- concise current status
- active world/document
- selection summary
- background task state
- engine connection/viewport status
- AI state later

### Settings
One unified settings experience.

Scopes:
- Project
- Editor/User
- Engine/Runtime

Capabilities:
- category tree
- search
- dirty/validation state
- apply/revert where needed
- default/reset actions

### AI Panel
v1 responsibilities:
- chat/instruction surface
- current context visibility
- action proposal preview
- approval queue
- execution through command bus
- action result log

v2 future direction:
- task queue
- asset generation
- code assistance
- broader creative workflows

## Command System Design

### Command principle
Neither UI nor AI should mutate editor state directly.
All mutating actions go through the command bus.

### Command groups
1. Scene/World commands
2. Asset/Project commands
3. Editor shell commands
4. Viewport/tool commands
5. AI/system commands

### Command model expectations
Each command should support appropriate metadata such as:
- can execute
- execute
- whether it can undo
- undo when reversible
- description for history UI
- merge/coalesce policy for interactive edits

### Transactions
Transactions are mandatory.
Required categories:
- single command
- compound command
- interactive transaction

Examples:
- Add Component => single command
- Instantiate prefab and select it => compound command
- Drag gizmo / slider edits => interactive transaction merged into one history entry

### Action context
A centralized editor action context should contain:
- active world
- active selection
- active panel
- active document
- focused viewport
- current asset/document
- play/edit mode
- origin metadata (UI/hotkey/AI)
- permission/safety metadata

This should drive:
- command applicability
- menu/toolbar enablement
- context-aware routing
- AI targeting

## Undo/Redo Policy

### In scope for v1
- scene/world edits
- component/property edits
- object lifecycle changes
- reparent/duplicate
- asset metadata edits where reasonable
- settings changes where feasible

### Out of scope for v1
- layout operations
- focus changes
- viewport navigation
- transient UI state

## AI Safety and Execution Model

AI actions must use the same command path as human actions.

AI flow:
1. inspect context
2. prepare action plan
3. preview actions
4. require approval where needed
5. execute through command handlers
6. report results/diagnostics

### v1 AI safety classification
#### Safe auto-execute
- open panel
- focus object
- search/select
- inspect/read-only actions
- frame camera
- navigate to asset

#### Confirm required
- create/delete objects
- reparent
- add/remove components
- property changes
- asset create/move/delete
- settings changes
- batch edits

#### Future elevated bucket
- code generation
- broad filesystem writes
- engine rebuild/restart
- external/network operations

## Data Flow

### Read path
engine/domain/services
-> editor state store / projections
-> panel viewmodels
-> UI

### Write path
UI / AI / hotkey / menu / drag-drop
-> command bus
-> handlers/services
-> engine/domain mutation
-> state refresh / projection update
-> UI

This replaces ad hoc UI-to-service mutation patterns.

## v1 Baseline Panel Set

Core workflow panels:
- Scene
- Hierarchy
- Inspector
- Project/Content
- Console

Shell surfaces:
- Menu bar
- Toolbar
- Status bar
- Layout manager

System panels:
- Settings
- AI

Near-future slots:
- Game
- Profiler
- Asset Preview
- Output/Tasks
- Search/References

## Delivery Strategy

The refactor should be implemented in phased, runnable vertical slices.

### General rules
- Architecture designed up front
- Execution by vertical slices
- Each phase ends in a runnable editor state
- Core abstractions tested before UI polish
- Old code removed in phases, not in one massive rewrite

## Migration Phases

### Phase 0 — Foundation / carve-out
- create new architecture namespaces/layers
- panel registry contracts
- command bus contracts
- undo/redo contracts
- layout model contracts
- unified settings contracts
- document current editor dependencies

Definition of done:
- editor still compiles
- core contracts and initial tests exist
- no visible regressions required yet

### Phase 1 — New Shell Skeleton
- introduce new shell host
- menu/toolbar/status surfaces
- panel registry
- split/tab/focus/open/close primitives
- panel resizing
- default layout
- layout save/restore/reset
- adapt existing views into the new shell initially

Definition of done:
- core panels open in the shell
- panels dock/tab/resize/hide/reopen
- layout persists across restart
- invalid layout falls back safely

### Phase 2 — Command Bus + Undo/Redo Backbone
- editor action context
- command dispatcher
- history stack
- transactions
- origin metadata (UI/hotkey/AI)
- migrate first set of world/object/component actions
- wire undo/redo into menu/toolbar/hotkeys

Definition of done:
- create/delete/reparent/add/remove/update component use commands
- undo/redo stable for core actions
- readable history labels visible

### Phase 3 — Workflow Slice: Hierarchy + Inspector + Selection
- hierarchy projection
- inspector projection
- centralized selection state
- hierarchy drag-and-drop through commands
- inspector property edits through commands
- unified command-driven context menus

Definition of done:
- stable selection synchronization
- reparent/edit/add/remove fully command-driven
- undo/redo reliable for this loop

### Phase 4 — Scene Panel Integration
- scene panel as proper panel/document type
- viewport lifecycle integrated with shell
- input ownership/focus model
- picking and selection routing
- transform/gizmo transactions
- scene toolbar bindings

Definition of done:
- viewport survives focus/layout changes
- selection sync works across scene/hierarchy/inspector
- transform edits use transaction-based undo

### Phase 5 — Project/Content + Settings
- content browser as proper panel
- asset create/open/move/rename/delete commands
- drag-and-drop asset flows to scene/inspector
- unified settings UI with Project / Editor / Engine scopes

Definition of done:
- content workflows are command-driven
- settings validate and persist correctly
- basic drag/drop flows are stable

### Phase 6 — AI Operator v1
- AI panel UI
- editor context feed
- action preview model
- approval flow
- execution through command bus
- audit/result feedback
- safe vs confirm-required action classification

Definition of done:
- AI can inspect context
- AI can propose actions
- approved actions execute through the same handlers
- reversible AI actions participate in undo/redo

## Testing Strategy

### Unit tests
Required for:
- layout model
- panel registry/restore
- command dispatch
- undo/redo
- transaction grouping and merge/coalesce rules
- settings model
- AI action planning/execution routing

### Integration tests
Required for:
- hierarchy selection -> inspector sync
- command -> world mutation -> projection refresh
- reparent consistency
- component add/remove/update flows
- layout restore to expected panel set
- settings edits affecting effective config
- AI action batches through command handlers

### UI/session tests
Required for critical workflows:
- launch shell
- render default layout
- dock/undock/resize flows
- panel open/close/reopen
- viewport session survives shell interactions
- AI panel basic execution flow

### Manual phase checklists
Every phase should validate:
- startup/shutdown
- panel focus
- resize behavior
- layout persistence
- selection consistency
- undo/redo sanity
- Mac-first validation without breaking Windows architecture

## Explicit v1 Non-Goals

Postpone for later:
- perfect multi-monitor UX
- animation-heavy docking chrome
- full ecosystem of custom asset editors
- macro/scripting system
- collaborative multi-user editing
- full AI creative pipeline
- code generation/build orchestration inside editor
- layout undo/redo
- exhaustive shortcut coverage from day one

## Main Risks

### Technical
1. MAUI limitations for advanced docking/windowing
2. viewport/native interop fragility during panelization
3. legacy view-model coupling
4. undo/redo complexity for interactive edits
5. drag-and-drop differences across platforms

### Product
1. scope explosion
2. overbuilding AI too early
3. polishing shell before command architecture is solid
4. carrying legacy code too long

## Risk Mitigations

- Validate shell/window/docking feasibility early
- Isolate viewport lifecycle behind adapters and tests
- Use compatibility adapters during migration, then delete old paths aggressively
- Build explicit reversible commands and transactions early
- Prevent any AI bypass around the command layer

## Branch / Execution Model

Recommended implementation branch:
- `feature/editor-shell-refactor`

Recommended workflow:
- design doc committed first
- detailed implementation plan next
- phased implementation with runnable checkpoint commits
- avoid one giant long-lived WIP blob without milestones

## Definition of Done for the Initiative

The initiative is successful when all of the following are true:

### UX
- editor feels like a real desktop tool
- docking/tabs/floating/resizing/layouts are reliable
- toolbar/status/settings are integrated
- the hardcoded-page feel is gone

### Architecture
- shell/panels/commands/projections are clearly separated
- undo/redo is real and stable
- layout persistence is versioned
- settings are unified by scopes

### Workflow
- hierarchy/inspector/scene/project/console work coherently
- drag-and-drop and selection sync are reliable
- the core authoring loop is smooth

### AI
- AI panel is native to the editor
- AI executes through the same command system
- approvals/audit/undo exist where appropriate

### Maintainability
- dead legacy paths are mostly removed
- critical architecture is covered by tests
- new panels/tools can be added without structural surgery

## Immediate Next Step

After approving this design, the next phase is writing a detailed implementation plan with task-by-task execution slices.
