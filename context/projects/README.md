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
| discovery | Recon: query the Mac over the existing session for build inputs and assemble a run configuration from selectable, discovered values. SCAN HOST finds buildable projects under a pointed-at root (curated, workspace-preferred); picking one prefills scheme/config/bundle-id; SWEEP FOR TARGETS lists devices+simulators. Full named-preset CRUD per connection, last-active restore, a session-sticky target kept out of the preset, off-thread/concurrent/cancelable queries, best-effort graceful-degrade failure handling. Stops at READY; builds no Play/run-state/logs. |
| logging | Developer-facing debug observability (infra, impl-only): a compile-gated (`OSTRICH_DEBUG`, off by default) global ambient logger writing a plain-text columnar file at `~/.local/state/ostrich/`, plus instrumentation of the connection lifecycle, remote command exec (the ops-first core — command string, exit code, timing, capped raw output), and discovery parse failures. The truth channel beneath the themed UI line. In-app panel deferred. |
| keybinds | Application-wide keyboard shortcuts (UI-only, impl-only): Ctrl+Enter EXECUTE from anywhere, Ctrl+Escape close connection, Ctrl+Backspace clear the Device Log; KEYCHAIN modal Enter=submit / Escape=skip; `v` toggles the project dropdown when no textbox is focused. No new intents/libraries — all in `src/ui/ui.cpp` reusing existing intents; global chords suppressed while the KEYCHAIN modal is open; verified manually. impl.md written 2026-06-08 (3 tasks). |
| xcode-project-build-and-deploy | The Play/Observe core loop: one ▶ EXECUTE action drives `xcodebuild` → install (`devicectl`/`simctl`) → launch over the existing session, with build-only COMPILE, a run-state machine, and universal ABORT. The Build Log streams the whole chain's raw tooling output (cleared per build); the Device Log streams the launched app's own stdout/stderr via process-console (`--console`), preserved with a `NEW PAYLOAD` demarcation per run. Re-EXECUTE is terminate-first (clean per-build data); COMPILE-while-running keeps both streams live and flags a stale deployed build. Simulators auto-boot (headless, log-only). Distinct build vs deploy failures, bounded ephemeral log buffers, off-thread/concurrent over multiple SSH channels. Amends design.md #7. PRD + ARD written 2026-05-25 (ARD: dedicated run subsystem on the existing worker — `RunChain` + persistent `DevConsole`; new pure libs `librunstate`/`libbuilddeploy`/`liblogbuf`; one run-event ring with raw chunk records; step-by-step orchestration; two-pronged kill; output-progress watchdog). Stops at the closed loop (no error parsing, launch args, debugger, or system-log firehose). |
