---
name: write-a-impl
description: Create an Implementation Plan through user interview, codebase exploration and then write the impl.md to the project. Use when user wants to write an IMPL, create a implementation plan for a new feature.
---

## Process

1. Read context/README.md and context/projects/README.md if you have not read it this session, to understand the structure of projects and what projects exist.

2. Ask the user for the current project-slug if you don't have it in this session.

3. Read all the files within `repo_root/context/projects/<project-slug>/` to gain context about the PRD and ARD if you don't already have them in this session. If the project directory doesn't exist simply operate off the context in the current session.

4. If unfamiliar with the codebase, launch an Explore agent to map architecture, existing patterns, and the integration layers relevant to this feature.

4a. Read context/coding_standards.md and ensure the implementation plan conforms to it (memory via arenas, modules as compiled libraries, the C/C++ seam, interface error-handling, and testing). Address its ARD/IMPL conformance checklist when scoping tasks.

5. Decompose requirements into tasks. Each task is scoped to produce a working, verifiable increment, minimizing integration risk and enabling continuous delivery.
	a. Break the requirements into layers appropriate to the architecture (e.g. UI → service → domain → infrastructure for web; protocol → handler → domain for backends).
	b. Plan tasks based on the layers, bottom-up (infrastructure/domain first, UI last) so each task lands on already committed foundations. Each task should be a single commit/PR that proves a checkpoint toward the goal.
	c. Ensure each step/task is independently buildable, testable, and releasable. If this is impossible, present the issue to the user and ask for guidance. 
		- buildable - it must successfully compile & build without issue
		- testable - all of the tests in the test suite must pass, including tests that were added for it
		- releasable - the current state of the repository, including tasks that came before the current task, must be **safe to be shipped** without having negative impacts on user experience or breaking backwards compatibility. If a task can't be safely user-facing yet, it's still releasable when gated behind a flag/disabled config

6. Review task breakdown with the user. Present the proposed tasks as a numbered list of tasks, shown as follows:
	- **Title**: short descriptive name
	- **Blocked by**: which other tasks (if any) must complete first
	- **User stories covered**: which user stories from the PRD this addresses if a PRD is present

7. Ask the user the following and iterate between step 6 and 7 until the user approves the breakdown:

	- Does the granularity feel right? (too coarse / too fine)
	- Are the dependency relationships correct?
	- Should any tasks be merged or split further?

8. Write the implementation plan in the project at `context/projects/<project-slug>/impl.md` with hard wraping at a maxmimum width of 72 using the following template:

<impl-template>
# Implementation Plan - <project-title>

## Summary of Tasks

This section contains one line summary description of the tasks in the sequence they should be tackled.

## Task Dependency Relationships

This section outlines the dependencies between tasks as a visual ascii art diagram.

## Detailed Tasks

This section has all of the tasks in the sequence they should be tackled in using the task template below for each task.

<task-template>
### Task <task-number> - <task-short-descriptive-name>

- **Status**: pending / complete
- **Blocked by**: which other tasks (if any) must complete first
- **User stories covered**: which user stories from the PRD this addresses if a PRD is present

#### What to build

A concise description of this task. Describe the end-to-end behavior. Reference specific sections of the parent PRD rather than duplicating content.

#### Technical Details

A concise description of related technical details (architectural changes, etc.). Reference specific sections of the parent ARD rather than duplicating content.

#### Acceptance criteria

- [ ] Criterion 1
- [ ] Criterion 2
- [ ] Criterion 3
</task-template>
</impl-template>
