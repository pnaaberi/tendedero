# Pegline FileShare

Approved in conversation: a persistent hanging upload card, immediate upload on
drop, progress/cancel, and copying either an owner-private download URL or the
exact receiving-machine path. Support local files and screenshot-card drags.

Keep one uploader in the Qt desktop controller, shared by all screen panels.
Use Qt Network to stream QFile data; fetch the existing owner-gated page for
its CSRF token and send the existing raw-body POST with Origin and filename.
Only a complete 201 received_untrusted receipt establishes success. Empty,
nonregular, symlink, remote-URL and >4 GiB inputs fail before transmission.
Interrupted transmissions remain uncertain and never retry automatically.
Keep a bounded recent receipt list in private local state; copy only on click.

The utility card remains visible with screenshots hidden, uses a fixed slot
outside their eight-item capacity, accepts only Copy drops, and suppresses
itself during Spectacle capture. Menus provide file selection, recent results,
cancel and the upload page. Share one queue across screens.

FileShare adds GET/HEAD /api/uploads/<exact-id>/download, restricted to the
same configured owner and trusted Serve boundary as its upload page. Validate
the exact receipt, open a regular nonsymlink file, send attachment/octet-stream
with no-store and Range support. Preserve the separate publication catalogue.
Add download_url to successful POST responses. No tokens in download URLs.

Endpoint configuration is local, outside Git; distributed Pegline stays
network-free until configured. Update documentation and native plain-text
Wayland clipboard support. Verify server auth/path/range boundaries, client
success/failure/cancellation, card input masks and real Wayland interaction.
