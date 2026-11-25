# Changelog

## 0.1.0 (unreleased)

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
