# Changelog

## 0.4.0 (2026-11-27)

- A `413` (batch too large) no longer drops the batch (P-2853). For a batch of more than 1 event, the plugin keeps the events and sends again at once with half the batch. It does not back off and does not count a failure. A `413` for 1 event drops that event and raises **On Send Failed** (`413`, `1`), as the other refused `4xx`. After a `2xx`, batches use the normal size (**Flush At**, at most 50) again. New value `EManalensSendOutcome::Split`, which `ClassifyStatus(413)` now returns.
- No send while the device has no network (P-2853). When `FPlatformMisc::GetNetworkConnectionType()` is `None` or `AirplaneMode`, the plugin starts no request, keeps the events, and does not count a failure or back off. When the device has a network again, the next tick sends at once (before, a long time offline made the plugin wait up to 300 s). A real network error during a send still backs off. `Unknown` (an engine or platform without a network monitor) counts as a network. New `FManalensClient::IsOffline` and `FManalensClient::EngineReportsNoNetwork()`.

## 0.3.0 (2026-11-27)

- Breaking: standard names for error and crash events (the same in the Unity, Godot, and Unreal SDKs).
  - The crash event `app_unclean_exit` is now `app_crash`. Its properties (`previous_session_id`, `previous_build`, `previous_app_version`) and the rule that sends it do not change.
  - The `source` property of an `error` event is now `manual` (a **Track Error** call) or `auto` (any automatic capture). The old value `unreal_log` is now `auto`. The engine is in `context.app.engine`.
  - New `log_type` on an error from the engine log: the verbosity of the line, `error` or `warning` (warnings only with **Capture Log Warnings** on). `log_category` does not change. Fatal lines are not captured, so there is no `fatal` value.
  - The per-session cap (`MaxAutoErrorsPerSession`) applies to `auto` errors only, as before.

## 0.2.0 (2026-11-27)

- New **Traits** (`Traits()`, `TraitsJson()`, and `FManalensClient::Traits()`) (P-2814): set traits on this player, with or without a player ID. Before **Identify** the traits belong to the anonymous ID of this install. The protected traits `email`, `itch_user_id`, `stripe_customer_id`, and `paddle_customer_id` are not sent (one warning). Calls before consent are merged and sent after consent.
- New **Link Steam Ticket** (`LinkSteamTicket(TArray<uint8>)`, `LinkSteamTicketHex(FString)`) and the **On Steam Linked** event (P-2815). The game passes a Steam Web API ticket with the identity `manalens` (from the Steamworks SDK or the Steam Online Subsystem); the plugin posts it to `/v0/link` on the API host (new **Api Endpoint** setting; default: the Endpoint with `events.` changed to `api.`) after consent, from the tick. No Steam module dependency. A refusal is not sent again; a `429` with `Retry-After` of 60 s or less is tried once more, but not after Set Consent (false) or Reset during the wait.
- The shared fixture scenario has a `traits` event.

## 0.1.6 (2026-11-26)

- Console values (P-2776). `context.app.platform` is now `ps5`, `xbox_series`, `xbox_one`, `switch`, or `switch2` in a console build (from `PLATFORM_PS5`, `PLATFORM_XSX`, `PLATFORM_XB1`, `PLATFORM_SWITCH`, and `PLATFORM_SWITCH2`), instead of `other`. New **Platform** setting (and `FManalensConfig::Platform` is now checked): empty detects the platform; set it when the plugin cannot name the console, in that platform's own `Game.ini`. A wrong value logs a warning and the build platform is used. The **Storefront** setting has PlayStation Store (`psn`), Microsoft Store on Xbox (`xbox`), and Nintendo eShop (`nintendo`).

## 0.1.5 (2026-11-26)

- Dedicated servers (G-2533): the SDK now starts on a dedicated server (`IsRunningDedicatedServer()`), but sends no `game_open`, `session_start`, `session_end`, or `app_unclean_exit`, and does not count as an install. Before, it stayed off there. It logs one line that says why. **Track** and **Identify** still work. The new setting **Track Server Builds** (default off, under Advanced) sends the automatic events from a dedicated server. New `FManalensConfig::bServerBuild` and `bTrackServerBuilds`. Commandlets still do not start the SDK. See the new docs page "Multiplayer and dedicated servers".

## 0.1.4 (2026-11-26)

- Each request sends `sent_at`, the device clock at the time of the send. The collector uses it to correct the event times, so a device clock that is fast or slow no longer gives wrong times or refused events (P-2799).
- Changed: `SetBeforeSend()` (and `FManalensClient::BeforeSend`) now runs on every event (track, identify, error, and the automatic events), not only on errors. The callback gets a copy of the whole event (`type`, `event`, `properties`, `traits`, `user_id`, ...) instead of the error properties; return it (changed or not), or `nullptr` to drop it. The SDK takes `event`, `properties`, `traits`, and `user_id` from the result. If the result changes or removes `type`, `message_id`, `anonymous_id`, `session_id`, or `context.app`, the original event is sent. Errors pass the error limits first. Same behavior as the Godot and Unity SDKs (G-2527).
- Privacy: the log message cleaning also redacts quoted values up to the closing quote (`password="a b"`), the JSON and header forms (`"token": "..."`, `"api_key":`, `token: `, `password: `, `"Authorization": "Bearer ..."`), JWTs (`eyJ...` with two dots), and base64 runs of 24 or more characters with `+` or a trailing `=`. Same rules as the Godot and Unity SDKs (P-2783).

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
