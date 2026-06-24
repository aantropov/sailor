This repository uses git submodules for dependencies (`External/renderdoc` and `External/vcpkg`).

The project is a game engine written in C++ and built with CMake. It targets Windows x64 and can be compiled with either the MSVC or clang toolchains.

Dependencies are managed through vcpkg. Whenever you update the repository (for example, pull new commits or fetch submodule changes) run `python update_deps.py` from the repository root to update and install the required packages.

The editor application is written in C# using .NET MAUI. Its source code lives in the `Editor` directory.

For C++ code use tabs. For C# code use spaces.

Use branch names only in English.

## Agent Workflow Infrastructure

The agent workflow documentation lives in `Docs/Agents/`.

- `Docs/Agents/workflow.md` defines the full GitHub issue/PR lifecycle, labels, handoff rules, stage artifacts, and validation guidance.
- `Docs/Agents/manager.md` defines the Manager agent, responsible for GitHub ticket state, labels, orchestration, and final workflow summaries.
- `Docs/Agents/team-lead.md` defines the Team Lead agent, responsible for ticket analysis, brainstorming, scope, risks, acceptance criteria, and implementation plans.
- `Docs/Agents/implementer.md` defines the Implementer agent, responsible for coding the approved plan, following surrounding code style, and producing the implementation summary.
- `Docs/Agents/tech-lead.md` defines the Tech Lead agent, responsible for architecture-focused code review.
- `Docs/Agents/lead-qa.md` defines the Lead QA agent, responsible for selecting and running validation before human review.

Use a small GitHub Project board with only these columns:

- `Backlog`
- `To Do`
- `In Progress`
- `Done`

Do not create separate GitHub Project columns for agent stages. Track agent stage, ownership, blocked state, and review state with labels and GitHub comments.

Required agent stage labels:

- `needs-planning`
- `planning`
- `ready-for-implementation`
- `implementation`
- `needs-tech-review`
- `tech-review`
- `needs-qa`
- `qa-validation`
- `ready-for-human-review`

State labels:

- `changes-requested`
- `blocked`

Owner labels:

- `agent-manager`
- `agent-team-lead`
- `agent-implementer`
- `agent-tech-lead`
- `agent-lead-qa`

Done-reason labels:

- `done-rejected`
- `done-fixed`
- `done-duplicate`
- `done-implemented`
- `done-cannot-reproduce`

The Manager must keep stage labels mutually exclusive, owner labels mutually exclusive, and closed tickets must have exactly one `done-*` label.

`Docs/Plans/` is intentionally ignored by Git. Use it for local, temporary, or developer-specific planning notes. Shared process documentation belongs in `Docs/Agents/`.
