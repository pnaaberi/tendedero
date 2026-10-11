# FileShare Implementation Plan

**Goal:** Drop files onto Pegline and copy an owner-private download link/path.
**Architecture:** Qt upload queue plus shelf panel card; exact-ID owner
download endpoint in the existing Python FileShare server.
**Spec:** ../specs/2026-10-11-fileshare-design.md
**Execution:** Inline, following the user's instruction to implement.

## Global constraints

- All transfers require configured HTTPS; preserve owner/Serve/Origin/CSRF gates.
- Local configuration and upload history must not enter Git.
- Keep screenshot capacity and publication catalogue independent of uploads.
- No automatic retry after ambiguous reception; no source deletion.

## Tasks

- [x] Server: extend tests/test_upload.py with owner-only GET/HEAD and Range,
  unauthorized/traversal/missing/symlink denial. Observe failure. Implement in
  serve.py/intake.py, then run upload/server tests. Update service docs and
  inventory; snapshot/record rollback and deploy after checks.
- [x] Native uploader: add fileshare.hpp/.cpp with sequential QFile POST,
  fresh CSRF, complete receipt validation, progress/cancel and bounded private
  receipt persistence. Test with a local HTTP fixture (success, malformed
  receipt, lost connection, cancellation, invalid paths). Add Qt6::Network
  to both CMake and Rust linkage. Extend clipboard helper for plain text.
- [x] Card: add drop handling/render/input region in Panel, shared uploader
  in Desktop, fixed utility slot, capture suppression and tray configuration.
  Extend native smoke coverage for hidden/revealed/suppressed cards, copy-only
  drops and screen-shared state. Run existing relevant UI regressions.
- [x] Delivery: document configuration and actions; build/install locally,
  verify a synthetic upload/download hash through the actual Tailnet endpoint,
  Wayland clipboard and drop behavior. Review diff and commit scoped files.

## Review focus

Cancellation can occur during CSRF fetch or POST; downloaded bytes must never
be served to another Tailnet identity; a drop may contain directories or
nonlocal URLs; the overlay must remain click-through outside visible controls;
capture suppression must recover on both cancellation and process failure.

## Progress

Baseline: Pegline 84f5c37; isolated `fileshare-upload` branch.
Implementation and deployment completed 2026-10-11. Server source and private
release/rollback receipts are maintained on the FileShare host.

Verification: 12 Rust store tests; all five native modes (existing core,
motion, capture arrival, FileShare card and HTTP client); formatting, Clippy
and release build. FileShare: 16 upload tests and 13 server boundary tests.
The real owner endpoint passed synthetic POST, exact download bytes/hash,
and both Wayland clipboard actions. A synthetic external native window also
completed a real compositor-delivered file drag to the installed app.
Exact synthetic uploads and their receipts were removed afterward.

Independent review found one important issue: copy actions could reference
the preceding successful file after a new failure. Fixed by retaining an
explicit current confirmed receipt; failure/cancellation regression checks
and the complete suite passed. Older receipts remain in the named menu.

Follow-up request: hide FileShare by default with the normal shelf and conceal
the hostname. Use the shared slide animation and a dashed visual drop target;
show selection size/count and sequential batch progress, and copy all confirmed
batch links/paths. Add native multi-file drop, duplicate, partial-failure,
cancel, hidden-background completion and batch-copy regression checks.
