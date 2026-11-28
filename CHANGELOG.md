# Changelog

## 0.8.0 (2026-11-28)

- Breaking: the crash event is now `game_crash` (was `app_crash`), the hang `error_type` is now `game_hang` (was `app_hang`), and `hang_duration_ms` is now `hang_duration_s` in seconds (one decimal, for example `6.2`). The plugin does not send or accept the old names. Events that an older version queued on disk keep their old name.

## 0.7.0 (2026-11-28)

- Fix (P-2865): an Endpoint written by hand as `Endpoint=https://events.manalens.dev` in `DefaultGame.ini` made the plugin log "the endpoint must start with https://" and stay off. Unreal reads an unquoted `//` in a `.ini` value as the start of a comment, so the value arrived as `https:`. The plugin now names the cause: "Unreal reads // in a .ini value as a comment. Put the URL in quotes: Endpoint=\"https://events.manalens.dev\"". An **Api Endpoint** cut the same way logs a warning and uses the default API host. Project Settings always wrote the quotes. Test: `Manalens.Config.EndpointCutAtComment`.
- Docs: a macOS Steam build must not use the App Sandbox. UE 5 signs a packaged Mac game with `Sandbox.Server.entitlements` (Development) or `Sandbox.NoNet.entitlements` (Shipping) by default. In the sandbox the Steam API cannot find the Steam client, so **Steam Link** gets no ticket, and `Sandbox.NoNet` blocks all events. Set `PremadeMacEntitlements` and `ShippingSpecificMacEntitlements` to an entitlements file without the sandbox (README, Verify Steam ownership). Tested end to end in a packaged Development Mac build: events first, link `200`, the install trusted in Manalens.
- Hang detection (P-2833): a watchdog thread sends an `error` with `error_type` `app_hang` (severity `error`, `source` `auto`) when the game thread is stuck for longer than **Hang Threshold Seconds** (new option in Project Settings > Plugins > Manalens > Errors, `HangThresholdSeconds`, default `5`; `0` turns it off; other values are clamped to 1 to 60 s). Every hang has `handled` `false` and the message "The main thread was blocked for N s", the same text as the web SDK. The hang has the game thread `stack`: the watchdog thread reads the addresses with `FPlatformStackWalk::CaptureThreadStackBackTrace` and looks up the symbols outside its lock, so the game thread never waits for the lookup. The watchdog writes the hang to disk while the game is stuck, so a game that the player kills keeps it: the next launch sends it in the old session (N is the threshold), and `app_crash` gets the new property `hang_message_id`. A hang that ends is sent once, with `hang_duration_ms` (N is that duration, one decimal). `SetConsent(false)` and `Reset()` also stop the count and clear the session and the breadcrumbs that the watchdog keeps. Focus loss and a map load pause the watchdog with separate flags; a map load with no PostLoadMap (a failed load) turns detection back on at the first frame 120 s after PreLoadMap. Not counted while the game is in the background, during a map load, at a debugger or Blueprint breakpoint, while the editor runs a slow task, in the first 10 frames, or on a quiet dedicated server. The usual error limits and `BeforeSend` apply.
- Tests: `Manalens.Hang.*`.

## 0.6.0 (2026-11-27)

- New option **Steam Link** (Project Settings > Plugins > Manalens > Steam, `bSteamLink`, off by default), as in the Godot and Unity SDKs. Once per launch, after consent, the plugin asks the Steam Online Subsystem for a Web API ticket with the identity `manalens` and links the Steam account (`POST /v0/link`). It waits until the first event batch of the launch was accepted (a 2xx answer), plus 20 seconds, because Manalens merges the installs of one Steam account only when the install has its own events before a link. If Steam gives no ticket (not running yet), it asks again every 10 seconds, at most 6 times. Needs the OnlineSubsystemSteam plugin and the Steam client; without them nothing happens. The plugin has no Steam dependency: it depends on the generic OnlineSubsystem plugin (on by default in the engine). `Link Steam Ticket` from your code does not wait, as before, and stops the option for that launch.
## 0.5.2 (2026-11-27)

- Fix: a crash stack longer than 1024 characters made the collector refuse the whole `app_crash` (`INVALID_EVENT`), so the crash was lost. The stack now keeps the top frames, at most 1024 characters, the collector limit for a property value. Found in a production test of the Godot SDK, which had the same limit.

## 0.5.1 (2026-11-27)

- Crash stacks: the crash message gets the full log cleaning again, so a long token or a `password=` value in an assertion text is masked. Only Windows exception names (`EXCEPTION_ACCESS_VIOLATION` and others) are kept as they are.

## 0.5.0 (2026-11-27)

- Crash stacks (P-2831): on Windows, macOS, and Linux, `app_crash` now says where the game crashed. At the next launch, the plugin reads the `CrashContext.runtime-xml` that the engine wrote for the crashed run (`Saved/Crashes`, and the engine user folder on macOS) and adds `stack`, `error_type`, `message`, `fingerprint`, `crash_source` (`unreal_crash_folder`), and `symbolicated`. No crash handler; at most 256 KB read; no minidump or log sent; each folder once; nothing before consent; ensures and stalls are skipped; folders of other games are skipped. New setting **Capture Crash Stacks** (default on) and `FManalensConfig::bCaptureCrashStacks`, `CrashDirs`, `CrashGameName`. macOS writes the crash folder only when the build includes the crash reporter.

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
