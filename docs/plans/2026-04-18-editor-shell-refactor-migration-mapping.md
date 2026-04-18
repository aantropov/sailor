# Editor Shell Refactor — Legacy Dependency Mapping

Date: 2026-04-18
Scope: Phase 0 foundation for the editor shell refactor

## Current composition entry points

### Main shell composition
- `Editor/MainPage.xaml`
- `Editor/MainPage.xaml.cs`

Current role:
- hardcoded page grid composition
- fixed placement for the editor's major views
- theme switching remains local to the page code-behind

### Existing shell-adjacent surfaces
- `Editor/Views/ToolbarView.xaml`
- `Editor/Views/ConsoleView.xaml`
- `Editor/Views/HierarchyView.xaml`
- `Editor/Views/InspectorView.xaml`
- `Editor/Views/ContentFolderView.xaml`
- `Editor/Views/SceneView.xaml`

These are currently view-first placements rather than registered shell panels.

## Current service and bridge dependencies

### Editor services in active use
- `Editor/Services/EngineService.cs`
- `Editor/Services/WorldService.cs`
- `Editor/Services/AssetsService.cs`
- `Editor/Services/SelectionService.cs`
- `Editor/Services/EditorContextMenuService.cs`

Observed dependency pattern:
- views and viewmodels can reach directly into services
- mutations are not yet funneled through a command bus
- selection truth is partially centralized in `SelectionService`, but still UI-driven

### Bridge/value to preserve under new architecture
The current engine/editor bridge already covers major editor operations that should be preserved behind future command handlers:
- remote viewport lifecycle and input
- world/object mutation flows
- component mutation flows
- asset/content workflows
- selection and contextual editor actions

## First adaptation targets into the panel system

The first legacy views to wrap as registered shell panels in Phase 1:
- `HierarchyView` -> tool panel
- `InspectorView` -> tool panel
- `ContentFolderView` -> tool panel
- `ConsoleView` -> tool panel
- `SceneView` -> document-style panel (initially likely singleton in practice)
- `ToolbarView` -> shell surface, not a panel

Planned early shell/system panels without existing equivalent:
- `Settings`
- `AI`

## Proposed migration mapping to new architecture

### Shell
New namespace/folder:
- `Editor/Shell`

Responsibility:
- top-level shell host abstractions
- layout persistence entry points
- panel open/focus orchestration

Legacy sources expected to migrate here first:
- top-level composition responsibilities now in `MainPage`

### Panels
New namespace/folder:
- `Editor/Panels`

Responsibility:
- panel identifiers and descriptors
- singleton/multi-instance metadata
- persistence hooks and registry contracts

Legacy sources expected to plug into panel descriptors later:
- `HierarchyView`
- `InspectorView`
- `ContentFolderView`
- `ConsoleView`
- `SceneView`

### Layout
New namespace/folder:
- `Editor/Layout`

Responsibility:
- persistent shell layout tree
- split/tab/floating contracts

Legacy hotspots expected to be retired later:
- fixed XAML grid layout in `MainPage.xaml`
- ad hoc splitter wiring tightly coupled to page layout

### Commands
New namespace/folder:
- `Editor/Commands`

Responsibility:
- command abstraction
- origin metadata
- action context
- transaction scope

Legacy hotspots expected to be retired later:
- direct service mutation from views/viewmodels/context menus
- duplicate mutation entry points across hierarchy/inspector/scene/content flows

### History
New namespace/folder:
- `Editor/History`

Responsibility:
- undo/redo contracts
- merge/coalesce policy abstractions
- readable history entries

Legacy hotspots expected to be retired later:
- one-off reversible behavior embedded directly in UI handlers

### State
New namespace/folder:
- `Editor/State`

Responsibility:
- shell focus/activation state
- later shared projections for selection, hierarchy, inspector, content

Legacy hotspots expected to be retired later:
- fragmented focus/selection ownership spread across views

### Settings
New namespace/folder:
- `Editor/Settings`

Responsibility:
- unified settings scopes and validation contracts

Legacy hotspots expected to be retired later:
- scattered editor/runtime configuration entry points

### AI
New namespace/folder:
- `Editor/AI`

Responsibility:
- AI action classification and editor-context contracts

Legacy status:
- no native editor-shell AI integration yet
- future AI actions must route through the shared command layer

## Phase 0 constraints upheld
- no user-visible shell change introduced yet
- no view migration performed yet
- current startup path remains intact
- new files are additive architectural contracts only

## Follow-up hotspots for Phase 1
1. create `EditorShellPage` or equivalent shell host without breaking startup
2. introduce a runtime panel registry implementation
3. wrap core legacy views as panel content
4. move hardcoded layout ownership out of `MainPage.xaml`
5. begin layout persistence with safe fallback to default layout
