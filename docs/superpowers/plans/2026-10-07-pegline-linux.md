# Pegline Linux Implementation Plan

> **For agentic workers:** Use superpowers:executing-plans to implement this plan inline, task by task. Steps use checkbox syntax for tracking.

**Goal:** Build, install, and run a native Linux screenshot line on the test machine and publish the port to the user's GitHub fork.

**Architecture:** A Rust executable owns screenshot discovery and persistent history. A small Qt6 C++ interface linked through C ABI owns native desktop interactions and layer-shell surfaces. All callbacks run on the GUI thread.

**Tech stack:** Rust, serde/serde_json, Qt6 Widgets/DBus, LayerShellQt, KF6GlobalAccel, Spectacle.

**Spec:** `docs/superpowers/specs/2026-10-07-pegline-linux-design.md`.

## Global constraints

- The modified app is called **Pegline**, with its own simple line-and-peg icon.
- Show up to eight recent screenshots.
- A 500 ms timer samples file metadata; a file must remain stable across samples before decode/add.
- The application never moves or removes watched originals on dismissal, overflow, or exit.
- No network access or telemetry is needed at runtime.
- No audio configuration changes.
- Build for the target desktop's CachyOS/KDE Wayland desktop using its installed native libraries.

## Review focus

- Interrupted screenshot writes: wait for stability and retry decoding rather than permanently skipping them.
- Dismissed or evicted files on restart: do not rediscover them until their contents change.
- Failed folder scan or corrupt state: preserve screenshot files and report errors without silently wiping history.
- Copy filename collisions: preserve both the original and every existing destination.
- Drag cancellation and Qt nested event loops: retain source files and keep widgets alive through drag completion.

## Task 1: Rust screenshot state

**Files:** `linux/Cargo.toml`, `linux/src/store.rs`, Rust unit tests in `store.rs`.

**Interfaces:** `Store::open(watch: PathBuf, state: PathBuf) -> io::Result<Store>`, `Store::scan() -> io::Result<bool>`, `Store::dismiss(path: &Path) -> io::Result<()>`, `Store::save_copy(path: &Path, destination: &Path) -> io::Result<PathBuf>`, and a serializable snapshot with `watch`, `items`, and `error` fields. Each item has `path` and `modified` fields. Keep settings discovery in this module.

- [ ] Write failing tests using real temporary files: incomplete/stable discovery, filtering video/hidden/symlink files, eight-item eviction, dismissal persistence, pruning, Spectacle file-URL decoding with spaces, corrupt-state preservation, and destination collisions.
- [ ] Run the tests and observe their failures before implementing the behavior.
- [ ] Implement metadata sampling, extension filtering, bounded history, dismissed fingerprints, atomic state save, and exclusive destination creation.
- [ ] Run `cargo test --manifest-path linux/Cargo.toml`; all tests pass.
- [ ] Commit the tested Rust state implementation.

## Task 2: Native Qt interface and Rust executable

**Files:** `linux/build.rs`, `linux/src/main.rs`, `linux/ui/pegline.cpp`, native UI smoke checks.

**Interfaces:** C ABI `pg_snapshot() -> *mut c_char`, `pg_action(operation: *const c_char, path: *const c_char) -> *mut c_char`, `pg_string_free(value: *mut c_char)`, and `pg_run(argc: c_int, argv: *mut *mut c_char) -> c_int`. Qt owns widgets; Rust owns returned strings until the matching free. A session D-Bus service `org.choppy.Pegline` exposes controls under `/Pegline`.

- [ ] Write failing native smoke checks for reveal/hide, card click producing image and URI clipboard MIME, dismissal retaining its file, and a render containing a card. Include cancelled drag ownership coverage.
- [ ] Implement the C ABI and a Cargo build script invoking CMake for Qt native compilation.
- [ ] Implement one top-anchored layer surface per monitor, an input mask limited to cards and a top-center sensor, animated reveal, card painting, gestures, clipboard, file drag/drop, menus, tray, KDE shortcut, and Spectacle actions.
- [ ] Add D-Bus single-instance controls and CLI flags from the spec. Report action failures through feedback and stderr.
- [ ] Run Rust tests and native smoke checks, then `cargo fmt --manifest-path linux/Cargo.toml --check`, `cargo clippy --manifest-path linux/Cargo.toml --all-targets -- -D warnings`, and `cargo build --manifest-path linux/Cargo.toml --release`; all pass.
- [ ] Commit the working interface and executable.

## Task 3: Install, validate on KDE, publish fork

**Files:** `linux/data/pegline.svg`, `linux/data/org.choppy.Pegline.desktop`, `linux/data/pegline.service`, `linux/scripts/install.sh`, root `README.md`.

**Interfaces:** Installed command `~/.local/bin/pegline`; desktop entry `org.choppy.Pegline.desktop`; systemd user service `pegline.service`, tied to the graphical session.

- [ ] Add the independent icon, desktop entry, user service, and rootless installation script. Set no Spectacle settings and replace no existing screenshot shortcuts.
- [ ] Rewrite the README for Pegline Linux, preserving upstream attribution and distinguishing supported Linux behavior from macOS features.
- [ ] Validate the desktop entry with `desktop-file-validate` and service with `systemd-analyze --user verify`.
- [ ] Install and start it. Check the running process, tray registration, shortcut registration, D-Bus status, and hover sensor geometry on the real KDE Wayland session.
- [ ] Create a temporary image in the watched folder, wait for ingestion, copy it through the app and verify clipboard image MIME, restart and verify persistence, then remove the fixture. Capture and inspect the revealed line.
- [ ] Perform one whole-branch code review using superpowers:requesting-code-review, fixing material findings and rerunning relevant checks.
- [ ] Create or reuse `pnaaberi/tendedero`, publish the port on `linux-pegline`, and verify remote commit and fork metadata. Keep the upstream source history.
- [ ] Record the final verification evidence and give the user the fork link, installed command, and practical usage instructions.
