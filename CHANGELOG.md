# Changelog

## 0.1.2 (2026-11-26)

- Fixed: a focus event after `Shutdown()` started a new, empty session. Now a focus event keeps only an open session alive; the next event starts a new one. Same rule as the flush timer in 0.1.1. Found in the `with-unity` example with 0.1.1.

## 0.1.1 (2026-11-26)

- Fixed: a quit added an empty session. While `Shutdown()` waited for the last send, the flush timer could fire and start a new session after `session_end`. So each quit counted one extra session with no events. Found in the `with-unity` example run (P-2676).
- Fixed: an install could be lost. `game_open` (with `is_first_open`) and `app_unclean_exit` stayed only in memory until the next save of the queue (a full batch, the flush timer, focus loss, or quit). A first launch with no network that was killed before then lost the install, and the next launch sent `is_first_open` false. Now the queue is written to disk as soon as these events are queued, before any send (P-2773).
- Play In Editor: errors of the editor itself are no longer sent as game errors. The editor logs in the same process as the game, so a real Play In Editor session sent `error` events for `LogBlueprint`, `LogEditor`, `LogShaderCompilers`, `LogDerivedDataCache`, `LogSourceControl` and other editor categories. In the editor, the log capture now skips the categories of editor modules (`FManalensClient::IsEditorLogCategory`). A packaged game is not changed (P-2792).

## 0.1.0 (2026-11-24)

First version. Same behavior and wire format as the Godot SDK and the Unity SDK 0.1.0:

- A game instance subsystem (`UManalensSubsystem`) that starts from **Project Settings > Plugins > Manalens**. No init call.
- Blueprint nodes: Track, Identify, Set Consent, Flush, Track Error, Reset, Link URL, Get Anonymous Id, and Get Session Id. C++ also has `TrackJson`, `IdentifyJson`, `TrackErrorJson`, and `SetBeforeSend`.
- Persistent anonymous ID, sessions that rotate after 30 minutes without focus or events and when they are 24 hours old, and automatic `game_open`, `session_start`, and `session_end`.
- Offline queue on disk in `Saved/Manalens` (1000 events, 7 days), batches of up to 50, and retries with backoff and jitter. A `Retry-After` header is honoured. A refused batch is dropped and the reason is logged. Refused events of an accepted batch (`202` with `rejected` above 0, P-2787) are dropped, logged with their reason codes, and reported by On Send Failed.
- Track Error and automatic capture of `Error` lines of the engine log, cleaned of home folders, URL credentials and queries, emails, and tokens. Online, platform, and Slate log categories are skipped. `app_unclean_exit` after a crash.
- On in the editor by default (Enabled In Editor), so Play in Editor sends events like a live game. Off on dedicated servers.
- Editor Build (default `dev`): the build label of Play in Editor runs, so they can be filtered out without a change to Build.
- On Flushed and On Send Failed events.
- Refuses Secret keys and API keys.
- Consent fails closed: an opt-out marker file, atomic writes, and an unreadable state keeps the SDK off.
- Event properties and traits: at most 50 keys and 8 KB.
