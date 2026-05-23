# Projects

This directory contains and index projects, each identified by a folder named
with the project's slug.

## Project Folder

Each projects folder contains context documents specific to the respective
project. The types of documents that may exist within the project folder
and their naming conventions are as follows.

| File Name | Description |
| --------- | ----------- |
| prd.md | Production Requirements Document - outlining the high level concept of the project, why it is valuable, what success looks like, and what the user stories are. |
| ard.md | Architecture Review Document - outlining how this project will fit into the existing system & application architecture or need to evolve the current system and application architecture. |
| impl.md | Implementation Plan - outlining the detailed tasks that need to be completed for the goals of this project to be completed. |

## Project Index

| Folder | Description |
| ------ | ----------- |
| project-setup | Bootstrap: plain Make build, hello-world C entry point, a `make test` smoke harness, and the project README. |
| app-shell | The themed GUI application entry point: a single GLFW + OpenGL 3 + Dear ImGui (docking) window, ~60 FPS render loop, full cyberpunk theme, centered ASCII wordmark, diagnostics footer, and clean shutdown. Empty docking host, no panels yet. |
| connection | The Connect phase: the BREACH connection overlay and thin connection bar, libssh2 linked in, ssh-agent / password auth, TOFU host-key verification against `~/.ssh/known_hosts`, off-thread cancelable connect, an exec-channel liveness probe, distinct failure reporting, keepalive-driven auto-reconnect, saved connections (KNOWN HOSTS) with opt-in remembered password, and a multi-channel-ready session. Stops before discovery/run-config/play/logs. |
