# Working on Kobalt

Read [Building](doc/building.md) before running a build. This branch is rebuilding the development tooling incrementally. Do not restore retired runners wholesale.

## Build ownership

Use CMake and presets for application builds. Do not add Python tooling or an internal build-support framework. Keep platform build definitions, presets and toolchains under `platforms/`. Shared application source remains under `src/`. Shared asset inputs live under root `assets/`; stage runtime bundles inside each platform output. Do not read runtime assets from another checkout.

Kobo and Android builds use prepared `kobuild` images. Use native ARM64 Android containers on ARM64 hosts when the selected image supports them. The compiler, sysroot, Qt modules, Android SDK, NDK and JDK belong to kobuild. Fix missing SDK components in kobuild rather than building Qt modules or repairing SDKs in the application. Never compile Qt or any Qt module as part of a Kobalt build.

The Mac app uses an existing native Qt SDK. A Kobo cross SDK cannot replace the native Mac SDK. Application-specific dependencies such as CREngine remain application inputs, separate from Qt SDK production.

Mac development uses Qt 6.11.3. From `platforms/desktop/`, build `mac-tests` and run `ctest --preset mac` for native headless checks. Build `mac-screenshots` for the current captures in `build/screenshots/mac/`. Both paths use the same compiled desktop implementation as the app. Keep test profiles isolated and remove temporary fixture and capture state when a run ends. See [headless tests and screenshots](doc/building.md#headless-tests-and-screenshots) for scope and commands.

Restore portable regression assertions from `develop` with only necessary path, fixture and SDK adaptations. The `mac-tests` preset builds desktop, model, engine, session, resource and component checks; use `ctest --preset mac -L model` for the restored model group. Reuse compiled application objects. Native checks run through `platforms/desktop/run-test.cmake` so failure and timeout cleanup does not depend on test destructors. Keep the Fusion style used by the app and the original component suite. Await asynchronous UI completion before sending the next action. A page center can be covered by a panel; target exposed paper when dismissing it.

Use the checked-in Trevelyan's Type Tester fixture for general reading checks and screenshots. It includes the chapter-heading CSS fix documented in [Native validation](doc/validation.md#book-fixtures). Keep synthetic books for focused regressions, not as a replacement for the general book fixture. The full screenshot gallery also uses the checksum-pinned international chapters and Gutenberg editions documented in Native validation. Keep those fixtures, their provenance and their scenes when extending the tour. Gallery captures are frameless production reader or Library images at 300 PPI, in both themes at six-inch and seven-inch sizes. Each gallery card represents one screen; size, theme and page choose its raw file. Require both themes at both sizes for each screen, with matching light/dark pages at each size. Keep device frames in the HTML viewer only, and retain filenames and checksums in the manifest for comparisons.

Use [Native validation](doc/validation.md) for the test inventory, feature coverage and explicit gaps. The routine UI loop is `mac-light-tests` followed by `ctest --preset mac-light`; `mac-light-screenshots` captures the primary views. Keep the light lane focused on primary views and features. It does not replace full release validation. `mac-fast-tests` and `ctest --preset mac-fast` retain the broader fast checks; `mac-tests` and `ctest --preset mac` run the full native set. `mac-screenshots` produces the current image set, checksum manifest and local gallery. Use `ctest --preset mac -L lifecycle` for reader lifecycle and annotation-frame checks, `-L ui` for shared canvas and paint checks, `-L platform` for portable adapter checks and `-L corpus` for the representative literary corpus. All four groups belong to the integration lane. Keep them in the integration lane and link the existing production objects; do not restore the old test-host application or scroll-only canvas. Keep public validation independent of private runtime services. Public Nickel compatibility checks are deferred until the user chooses an approach.

Flag suboptimal approaches before extending them. Identify duplicated build logic, application-side SDK workarounds, dependence on an old worktree, unnecessary rebuilds and unclear ownership. Explain the issue and propose a simpler approach.

Do not record fixed test, case, assertion or screenshot totals in documentation or these instructions. Describe coverage and verification commands instead. Use `ctest --preset mac -N`, with optional `-L` or `-R` filters, for the current test inventory. Keep meaningful fixture sizes, limits and measured durations.

When adding a UI screen that needs screenshot coverage, include its scenes in the full gallery in the same change. When adding or removing screenshot scenes, update the full-gallery expected count in `tests/desktop.cpp` in that change. Run `cmake --build --preset mac-screenshots` from `platforms/desktop/` to verify that the complete collection matches that count.

## Execution and cleanup

Run native Qt commands outside the agent sandbox on the development Mac. Headless Qt tools still need CPU capabilities that the sandbox can hide. Container commands need access to the engine socket. Keep outputs and caches in the authorized workspace.

Do not open a visible app or run visible UI tests without authorization. Prefer native headless validation while editing, then validate Kobo and Android integrations last. Reuse prepared SDKs and verified dependencies.

Stop and reap run-owned helper processes and remove scratch profiles, staging directories and temporary containers when a run ends, including on failure or interruption. Preserve the user's app state, active runs, SDKs and incremental caches. Retain useful current results and unresolved failure evidence rather than every successful raw log indefinitely.

## Repository practice

Read [Storage compatibility](doc/storage.md) before changing settings, book state, bookmarks, annotations or rendering caches. This also applies to backup tools and test database inspection: follow the backup contract and never open the live exclusive profile through an external SQLite connection. Keep saved reading data separate from disposable caches. App releases must not reset either format merely because the release version changed. Compatible optional data fields keep the current format version and must load older records with explicit defaults. Preserve supported optional fields when editing records. An incompatible saved-data change requires a format-version bump, a migration that preserves existing user data, and regression tests before it ships. Reject unsupported saved-data versions without overwriting the original files. Do not assume older builds preserve unknown fields or can safely write newer schemas.

Bump the format version in `documentCacheChecksum` when parser or preprocessing changes make parsed-book caches incompatible. Bump the inspection-cache format separately when verdict rules change incompatibly. Cache invalidation must rebuild disposable data only; it must never clear settings, bookmarks or annotations.

Keep Markdown in `doc/`, except the root `README.md` and `AGENTS.md`. The user owns root README edits. Keep these instructions and build commands current in the same change as tooling updates.

Do not stage, commit, push or publish without explicit authorization. Signed operations require an advance warning for 1Password prompts. Never add an agent coauthor. Preserve unrelated changes and the original checkout.

Parallel workers implementing features must each use a separate Git worktree and a dedicated feature branch named `feature/name-of-feature`. The `feature/` prefix is required. Do not implement parallel features in the shared `develop` checkout. When authorized to integrate a feature into `develop`, prefer a fast-forward when possible. Otherwise, cherry-pick the feature commits. Create a merge commit only when the user explicitly requests one instead of cherry-picking.

After successful integration, normally remove the worker's worktree and feature branch unless the user asks to keep them. Before removal, verify that the worktree has no uncommitted changes and that all intended changes are integrated. Preserve any needed test evidence or other artifacts before removing the worktree.

Use lowercase release-stage words in commit messages, including `alpha` and `beta`, even when part of a release name. The abbreviation `RC` may remain uppercase.
