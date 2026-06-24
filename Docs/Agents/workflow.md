# Agent Workflow

This document defines the sub-agent operating model for Sailor development work. The workflow is centered on GitHub issues, pull requests, labels, and a small GitHub Project board. The Manager owns ticket movement and coordination; specialist agents own planning, implementation, architecture review, and QA validation.

## Roles

### Manager

The Manager is the orchestrator for the ticket lifecycle. The Manager owns GitHub state, stage transitions, and concise status reporting.

Responsibilities:

- Triage new or updated GitHub issues.
- Keep the GitHub Project status accurate.
- Assign the next responsible agent for each stage.
- Ensure each stage posts a short, actionable result to the issue or PR.
- Track blockers, unresolved questions, and requested changes.
- Move the ticket to human review only after planning, implementation, tech review, and QA validation are complete.

The Manager does not make architecture decisions or perform the main implementation unless explicitly assigned. The Manager may ask clarifying questions and may stop the workflow when product intent, scope, or risk is unclear.

### Team Lead

The Team Lead turns a ticket into an implementation plan. The Team Lead understands the feature request, repository structure, likely code paths, and acceptance criteria.

Responsibilities:

- Read the GitHub issue and related PRs or comments.
- Inspect relevant code before proposing a plan.
- Answer brainstorming questions.
- Define scope and non-goals.
- Identify affected modules, risks, dependencies, and expected tests.
- Produce a plan that an Implementer can execute without guessing.

### Implementer

The Implementer is the primary coding agent. The Implementer executes the Team Lead plan, keeps changes scoped, and prepares the PR for review.

Responsibilities:

- Create or update a branch and PR for the ticket.
- Implement the planned code changes.
- Add or update focused tests when appropriate.
- Follow repository conventions from `AGENTS.md`.
- Record deviations from the plan and explain why they were needed.
- Hand the PR to the Tech Lead for review.

### Tech Lead

The Tech Lead is the architecture reviewer. The Tech Lead checks whether the implementation fits the engine and editor architecture, not just whether the code compiles.

Responsibilities:

- Review the PR diff and relevant surrounding code.
- Check ownership boundaries, lifetimes, threading, runtime/editor separation, platform assumptions, and public contracts.
- Identify regressions, brittle shortcuts, hidden coupling, or missing tests.
- Approve the architecture or request concrete changes.

### Lead QA

The Lead QA validates behavior after Tech Lead approval. The Lead QA selects and runs the test set that matches the change risk.

Responsibilities:

- Run relevant automated tests.
- Run functional world tests when runtime behavior is affected.
- Run editor tests when editor workflow or C# code is affected.
- Compare output, logs, screenshots, and performance with expected behavior or baselines.
- Approve QA or return the ticket with clear reproduction details.

## GitHub Project Columns

Use only coarse workflow columns in GitHub Projects:

- `Backlog`
- `To Do`
- `In Progress`
- `Done`

The `Done` column must include a reason in the ticket or PR summary:

- `Done: fixed`
- `Done: rejected`
- `Done: duplicate`
- `Done: implemented`
- `Done: cannot-reproduce`

Do not create separate project columns for agent stages. Agent stage, ownership, blockers, and review state are labels and comments.

## Labels

Use labels to represent agent workflow state. Only one stage label should be active at a time:

- `needs-planning`
- `planning`
- `ready-for-implementation`
- `implementation`
- `needs-tech-review`
- `tech-review`
- `needs-qa`
- `qa-validation`
- `ready-for-human-review`

Use state labels when work cannot advance normally:

- `changes-requested`
- `blocked`

Use owner labels when a specific agent owns the next action. Only one owner label should be active at a time:

- `agent-manager`
- `agent-team-lead`
- `agent-implementer`
- `agent-tech-lead`
- `agent-lead-qa`

Use done-reason labels when a ticket enters `Done`:

- `done-rejected`
- `done-fixed`
- `done-duplicate`
- `done-implemented`
- `done-cannot-reproduce`

Use optional triage labels when they help filtering and reporting:

- Type: `type-bug`, `type-feature`, `type-refactor`, `type-documentation`, `type-test`, `type-build`
- Area: `area-runtime`, `area-editor`, `area-renderer`, `area-rhi`, `area-assets`, `area-platform`, `area-ci`
- Priority: `priority-low`, `priority-medium`, `priority-high`
- Risk: `risk-low`, `risk-medium`, `risk-high`

## Label Hygiene

The Manager must keep labels mutually consistent:

- When applying a new stage label, remove the previous stage label.
- When applying a new owner label, remove the previous owner label.
- `blocked` may coexist with the current stage and owner labels.
- `changes-requested` may coexist with `agent-implementer`, `agent-team-lead`, or `agent-manager`, depending on who owns the next action.
- `ready-for-human-review` must not coexist with `changes-requested` or `blocked`.
- `done-*` labels are only valid after the ticket enters `Done`.
- Closed tickets must have exactly one `done-*` label.
- If a PR is merged, prefer `done-implemented` for feature work and `done-fixed` for bug fixes.

## Lifecycle

1. Manager triages a GitHub issue. If it is accepted, Manager moves it to `To Do` and applies `needs-planning` plus `agent-team-lead`.
2. Team Lead analyzes the ticket, answers brainstorming questions if needed, and posts an implementation plan.
3. Manager checks that the plan has scope, risks, acceptance criteria, and a test plan, then applies `ready-for-implementation` plus `agent-implementer`.
4. When active work starts, Manager moves the ticket to `In Progress`.
5. Implementer creates or updates a branch and PR, implements the plan, and posts an implementation summary.
6. Manager applies `needs-tech-review` plus `agent-tech-lead`.
7. Tech Lead reviews the PR. If changes are required, Manager applies `changes-requested` plus `agent-implementer`.
8. After Tech Lead approval, Manager applies `needs-qa` plus `agent-lead-qa`.
9. Lead QA runs the selected validation and posts a QA report. If validation fails, Manager applies `changes-requested` or `blocked`.
10. After QA approval, Manager posts the final workflow summary and applies `ready-for-human-review` plus `agent-manager`.
11. Human reviewer performs final code review and merge decision.
12. After merge or closure, Manager moves the ticket to `Done` and records the done reason.

## GitHub Integration Setup

Minimum repository setup:

- Create a GitHub Project with `Backlog`, `To Do`, `In Progress`, and `Done`.
- Add the project to the repository.
- Create the labels listed in this document and give each label a short description.
- Add issue templates for bugs, features, and technical tasks.
- Add a pull request template with links to the issue, implementation summary, tests, and risk.
- Enable built-in project automation for closed issues and merged PRs to move items to `Done`.
- Use GitHub Actions or the GitHub GraphQL API for any automation that updates Project fields beyond built-in workflows.
- Keep the Manager as the only actor that changes agent stage labels until automation is proven reliable.

Recommended Project views:

- Board grouped by `Status`.
- Table filtered to open items and showing labels, assignee, linked PR, and updated date.
- QA view filtered by `needs-qa` or `qa-validation`.
- Human review view filtered by `ready-for-human-review`.

MCP and CLI integration:

- Use the GitHub MCP/App for issues, PRs, comments, labels, and review metadata when available.
- Use `gh` or GraphQL for GitHub Project v2 field updates when MCP coverage is insufficient.
- Keep all agent decisions traceable by writing stage artifacts back to the issue or PR.

## Required Stage Artifacts

### Planning

```md
## Implementation Plan

Issue: #123
Labels: ready-for-implementation / blocked

### Scope
- ...

### Non-Goals
- ...

### Affected Areas
- ...

### Proposed Approach
- ...

### Risks
- ...

### Acceptance Criteria
- ...

### Test Plan
- ...
```

### Implementation

```md
## Implementation Summary

Issue: #123
PR: #456
Labels: needs-tech-review / blocked

### Changes
- ...

### Files and Modules
- ...

### Deviations from Plan
- ...

### Tests Added or Updated
- ...

### Known Limitations
- ...
```

### Tech Review

```md
## Tech Review

PR: #456
Review result: Approved / changes-requested

### Findings
- ...

### Architecture Assessment
- ...

### Required Changes
- ...

### Residual Risk
- ...
```

### QA Report

```md
## QA Report

PR: #456
Status: Approved / Failed / Blocked

### Tests Run
- ...

### Results
- ...

### Performance Notes
- ...

### Regressions
- ...

### Recommendation
- ...
```

### Manager Summary

```md
## Agent Workflow Summary

Issue: #123
PR: #456
Labels: ready-for-human-review / blocked

### Planning
Status: Approved / Blocked
Summary:
- ...

### Implementation
Status: Complete / Blocked
Summary:
- ...

### Tech Review
Status: Approved / changes-requested
Summary:
- ...

### QA
Status: Approved / Failed / Blocked
Summary:
- ...

### Manager Assessment
Risk: Low / Medium / High
Recommendation: Ready for human code review / Needs more work
```

## Sailor Validation Guide

The Lead QA selects tests based on the affected area. Common commands:

```bash
ctest --test-dir build-mac-vcpkg -C Debug --output-on-failure
dotnet test Editor.Tests/Editor.Contracts.Tests.csproj
python3 run_world_tests.py --config Release
```

Selection guidance:

- Runtime C++ contracts or remote viewport changes: run CTest remote viewport tests and relevant world tests.
- Editor C# workflow changes: run `dotnet test Editor.Tests/Editor.Contracts.Tests.csproj`.
- Renderer, frame graph, asset registry, or gameplay behavior: run `python3 run_world_tests.py --config Release`.
- Performance-sensitive changes: run benchmark or performance world tests and record timing deltas.
- Dependency, submodule, or toolchain updates: run `python update_deps.py` first, then rebuild and validate.

## Handoff Rules

- Every handoff must include a GitHub comment or PR comment with the required stage artifact.
- Agents must include blockers explicitly. Silent failure is not an acceptable state.
- The Manager is the only role that applies `ready-for-human-review`.
- A PR cannot receive `needs-qa` until Tech Lead review is approved.
- A PR cannot receive `ready-for-human-review` until QA is approved or the Manager documents an explicit human-approved exception.
- If any stage requests changes, the Manager routes the ticket back to the correct role and records the reason.
