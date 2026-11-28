# Manalens for Unreal Engine

Game analytics for Unreal Engine 5, as a C++ plugin with Blueprint nodes. The plugin sends sessions, `game_open`, your game events, and errors to [Manalens](https://manalens.dev) with your project Publishable key. It keeps an offline queue on disk and has a consent mode.

It uses only engine modules (Core, CoreUObject, Engine, HTTP, Json, and DeveloperSettings) and supports Windows, macOS, Linux, Android, and iOS.

## Install

1. Copy the `Manalens` folder into the `Plugins/` folder of your project.
2. Open the project and let the editor build the plugin. A Blueprint-only project needs one C++ class first (**Tools > New C++ Class**).
3. Open **Edit > Project Settings > Plugins > Manalens** and set the **Publishable Key** and the **Endpoint**.

If you write the settings into `Config/DefaultGame.ini` by hand, put the URL in quotes: `Endpoint="https://events.manalens.dev"`. Unreal reads an unquoted `//` in a value as a comment, so the plugin would get `https:` and stay off.

The engine starts the SDK with the game instance. It also sends from Play in Editor, so you can watch your events in Activity and Live View while you play. Dev and live data go to the same project. Play in Editor runs send **Editor Build** (default `dev`) in place of **Build**, so you can filter them out. To tell other test builds apart, set **Build** (for example `dev`) and filter by it. To send nothing from the editor, turn off **Enabled In Editor**.

When the game quits normally (window close, Cmd+Q, the `Quit` command, or SIGINT or SIGTERM, which the engine turns into a normal quit), the plugin ends the session and sends `session_end` and the rest of the queue before the game exits. It waits at most **Quit Timeout Seconds** (Project Settings > Plugins > Manalens > Advanced, default 2 s, 0 to 10). Events it cannot send in that time, or with no network, stay on disk and go out at the next launch. Without consent nothing is sent.

## Track events

In Blueprint, use **Get ManalensSubsystem**, then **Track**. In C++:

```cpp
#include "ManalensSubsystem.h"

if (UManalensSubsystem* Manalens = GetGameInstance()->GetSubsystem<UManalensSubsystem>())
{
    Manalens->Track(TEXT("level_complete"), {}, { { TEXT("level"), 3 }, { TEXT("score"), 1200 } }, {});
}
```

Add `"Manalens"` to the dependencies in the `Build.cs` file of your game module.

The other nodes are Identify, Traits, Link Steam Ticket, Set Consent, Flush, Track Error, Reset, and Link URL. The full guide, with every setting, is in the [Unreal SDK docs](https://docs.manalens.dev/sdks/unreal).

## Traits

**Traits** sets traits on this player. It needs no player ID: before **Identify**, the traits belong to this install's anonymous ID; after it, to the player.

```cpp
Manalens->Traits({ { TEXT("difficulty"), TEXT("hard") } }, { { TEXT("level"), 3 } }, {});
```

- At most 50 keys and 8 KB of JSON. `TraitsJson()` (C++) takes a JSON object, where a null value clears a trait.
- The protected traits `email`, `itch_user_id`, `stripe_customer_id`, and `paddle_customer_id` are not sent (one warning). Only a server with the secret key can set them.
- Like Identify, it waits for consent. Calls before consent are merged and sent then.

## Verify Steam ownership

**Link Steam Ticket** links the Steam account that runs the game to this install. Manalens checks a Steam ticket with Steam and reads which of your apps the account owns. The player does not sign in. See [Steam ownership](https://docs.manalens.dev/features/steam-ownership) and the [Link API](https://docs.manalens.dev/api/link).

**Steam Link** (the easy way): with the Steam Online Subsystem (the OnlineSubsystemSteam plugin enabled and `DefaultPlatformService=Steam`), turn on **Steam Link** in Project Settings > Plugins > Manalens > Steam. Once per launch, after consent, the plugin gets a Web API ticket with the identity `manalens` and links it. It waits until the first event batch of the launch was accepted, plus 20 seconds: Manalens merges the installs of one Steam account only when the install has its own events before a link. See [Link Steam in 2 minutes](https://docs.manalens.dev/features/link-steam).

The plugin has no Steam dependency (it uses the generic OnlineSubsystem plugin, which the engine turns on by default). You can also get the ticket yourself and pass its bytes. Do it after your first events were sent. With the Steamworks SDK (the `Steamworks` module of the engine):

```cpp
// After SteamAPI_Init(). Keep the callback object alive until it fires.
STEAM_CALLBACK(AMyGameMode, OnWebApiTicket, GetTicketForWebApiResponse_t);

SteamUser()->GetAuthTicketForWebApi("manalens");

void AMyGameMode::OnWebApiTicket(GetTicketForWebApiResponse_t* Response)
{
    if (Response->m_eResult == k_EResultOK)
    {
        TArray<uint8> Ticket(Response->m_rgubTicket, Response->m_cubTicket);
        GetGameInstance()->GetSubsystem<UManalensSubsystem>()->LinkSteamTicket(Ticket);
    }
}
```

With the Steam Online Subsystem, get a Web API ticket for the identity `manalens` from its auth interface, and pass the bytes (or the hex string to **Link Steam Ticket Hex**).

- The request goes to `<Api Endpoint>/v0/link` after consent. **Api Endpoint** (Advanced) defaults to the Endpoint with `events.` changed to `api.`.
- **On Steam Linked** (bLinked, Status, Code) gives the answer. `Code` is the API error code (for example `STEAM_TICKET_INVALID` or `STEAM_NOT_CONNECTED`), or `NETWORK_ERROR`, `CONSENT_REQUIRED`, or `IDENTITY_CHANGED`.
- A refused ticket is not sent again. After a `429` with `Retry-After` (60 s or less), the plugin tries once more, but not after Set Consent (false) or Reset during the wait.
- Say in your privacy notice that your game sends a Steam ticket to Manalens to confirm ownership.

**macOS: no App Sandbox for a Steam build.** By default, UE 5 signs a packaged Mac game with an App Sandbox (`Sandbox.Server.entitlements` for Development, `Sandbox.NoNet.entitlements` for Shipping). In the sandbox, the Steam API cannot find the Steam client ("Could not determine Steam client install directory"), and `Sandbox.NoNet` blocks all network, so a Shipping build sends no events. For a Steam build, add `Build/Mac/Resources/Unrestricted.entitlements` (a plist with an empty `<dict/>`) and set it in `Config/DefaultEngine.ini`:

```ini
[/Script/MacTargetPlatform.XcodeProjectSettings]
PremadeMacEntitlements=(FilePath="/Game/Build/Mac/Resources/Unrestricted.entitlements")
ShippingSpecificMacEntitlements=(FilePath="/Game/Build/Mac/Resources/Unrestricted.entitlements")
```

A Mac App Store build keeps the sandbox and needs `com.apple.security.network.client` to send events. See [macOS builds and the App Sandbox](https://docs.manalens.dev/sdks/unreal#macos-builds-and-the-app-sandbox).

## No network

The plugin keeps events on disk and sends them in batches. Before a send, it asks the engine for the network type (`FPlatformMisc::GetNetworkConnectionType()`). When the type is `None` or `AirplaneMode`, the plugin starts no request: the events stay in the queue, and the wait after failed sends does not grow. When the device has a network again, the next tick (**Flush Interval Seconds**) sends at once. `Unknown` and every connected type do not stop a send, so a real network error still waits and tries again (2 s, 4 s, and so on, at most 300 s).

The engine reports `None` from its own network monitor: Network List Manager on Windows, the network interfaces on Linux, `NWPathMonitor` on macOS and iOS, and `ConnectivityManager` on Android (UE 5.8). An engine or platform without a monitor reports `Unknown`, and the plugin sends as before.

## Crash stacks

After a crash, the next launch sends `game_crash`. On Windows, macOS, and Linux it also says where the game crashed. The plugin installs no crash handler: when the game crashes, the engine itself writes a crash folder with `CrashContext.runtime-xml`. At the next launch, the plugin reads the call stack and the error message from that file and adds `stack`, `error_type` (for example `EXCEPTION_ACCESS_VIOLATION` or `SIGSEGV`), `message`, `fingerprint`, `crash_source` (`unreal_crash_folder`), and `symbolicated` to `game_crash`.

- Windows and Linux: the engine writes the folder to `Saved/Crashes`.
- macOS: the engine writes the folder only when the build includes the crash reporter (Project Settings > Packaging > **Include Crash Reporter**).
- A shipping build without debug symbols (`.pdb` on Windows) shows `UnknownFunction` frames. The plugin then sends `symbolicated` = `false`.
- The plugin reads at most 256 KB, never sends the minidump or the log, never sends the same folder twice, and reads nothing before consent. Home folders, URLs, and emails are removed from the frames.
- Turn it off with **Capture Crash Stacks** (Project Settings > Plugins > Manalens > Errors).

## Hangs

A frozen game that the player kills is an `game_crash` with no cause. The plugin also watches the game thread. A watchdog thread checks a heartbeat that the game thread writes once per frame. When one frame takes longer than **Hang Threshold Seconds** (default `5`; other values than `0` are clamped to 1 to 60), the game thread is stuck: the plugin sends an `error` with `error_type` `game_hang`, severity `error`, and `source` `auto`, once per stuck period.

- The watchdog writes the hang to `Saved/Manalens/manalens_hang.json` while the game is stuck, so it is not lost when the player kills the game.
- It reads the game thread stack addresses from the watchdog thread with `FPlatformStackWalk::CaptureThreadStackBackTrace` (as the engine's own hitch detector does), looks up the symbols after that (the game thread never waits for the lookup), and sends it as `stack`: `Module!Function [File:Line]` frames, at most 1024 characters. Home folders are removed. A build without symbols shows `UnknownFunction` frames.
- Every hang has `handled` `false` and the message "The main thread was blocked for N s" (the same text as the web SDK). When the game recovers, the hang is sent with `hang_duration_s` (seconds, one decimal, for example `6.2`), and N is that duration. When the game was killed while stuck, the next launch sends the hang in the old session, N is the threshold, and `game_crash` gets `hang_message_id` (the `message_id` of the last hang of that session).
- `SetConsent(false)` and `Reset()` also clear the session and the breadcrumbs that the watchdog keeps, so no hang with old data is saved after them.
- No hang is counted while the game is in the background, during a map load (a load that never ends, with no PostLoadMap, turns detection back on after 120 s), at a debugger or Blueprint breakpoint, while the editor runs a slow task, in the first 10 frames, or on a dedicated server without **Track Server Builds**.
- Hangs use the same error limits as other automatic errors, and `BeforeSend` can drop them.
- Set **Hang Threshold Seconds** (Project Settings > Plugins > Manalens > Errors) to `0` to turn it off.

## BeforeSend

`SetBeforeSend()` (C++) gets a copy of each event (track, identify, traits, error, automatic events) before it is queued. Return it (changed or not), or `nullptr` to drop it. The SDK takes `event`, `properties`, `traits`, and `user_id` from the result. If the result changes or removes `type`, `message_id`, `anonymous_id`, `session_id`, or `context.app`, the SDK sends the original event.

```cpp
// Keep 10% of player_jump events (sampling).
Manalens->SetBeforeSend([](TSharedPtr<FJsonObject> Event) -> TSharedPtr<FJsonObject>
{
    FString Name;
    if (Event->TryGetStringField(TEXT("event"), Name) && Name == TEXT("player_jump") && FMath::FRand() >= 0.1f)
    {
        return nullptr; // drop
    }
    return Event;
});

// Remove personal data from every event (PII scrub).
Manalens->SetBeforeSend([](TSharedPtr<FJsonObject> Event) -> TSharedPtr<FJsonObject>
{
    Event->GetObjectField(TEXT("properties"))->RemoveField(TEXT("email"));
    const TSharedPtr<FJsonObject>* Traits = nullptr;
    if (Event->TryGetObjectField(TEXT("traits"), Traits))
    {
        (*Traits)->RemoveField(TEXT("email"));
    }
    return Event;
});
```

## Repository layout

| Path | What it is |
| --- | --- |
| `Manalens/` | The plugin. This is the only folder your game needs. |
| `Manalens/Source/Manalens/` | The runtime module: the client, the subsystem, and the settings. |
| `Manalens/Source/ManalensTests/` | Automation tests (an editor module). |
| `HostProject/` | A small project that runs the automation tests. |
| `Tests/run-tests.sh` | Builds the plugin with `RunUAT BuildPlugin` and runs the tests headless. |
| `Tests/fixtures/` | The request bodies of the test scenario. They match the Unity SDK fixtures. |
| `Tests/E2E/` | A small game module for the end-to-end test against a real collector. |
| `.github/workflows/` | CI (`ci.yml`) checks `Manalens.uplugin`, the version, and that no binaries are committed. GitHub-hosted runners have no Unreal Engine, so the build and the tests run with `Tests/run-tests.sh` before each release. `release.yml` makes a release from a `v*` tag. |

## Releases

Each [release](https://github.com/manalens/manalens-unreal/releases) has `manalens-unreal-<version>.zip` (the `Manalens` plugin folder, source only), `SHA256SUMS`, and the changes from `CHANGELOG.md`. GitHub Actions builds the zip from the tagged commit and attests its build provenance. To check a download:

```bash
sha256sum --check --ignore-missing SHA256SUMS
gh attestation verify manalens-unreal-<version>.zip --repo manalens/manalens-unreal
```

## Test

```bash
UE_ROOT="/Users/Shared/Epic Games/UE_5.8" Tests/run-tests.sh
```

The script builds the plugin, copies it into `HostProject/Plugins`, and runs `Automation RunTests Manalens` with `UnrealEditor-Cmd`. Set `UPDATE_FIXTURES=1` to write the request bodies to `Tests/fixtures`.

To change the SDK, see [CONTRIBUTING.md](CONTRIBUTING.md): the test setup, how to release, and the commit rules.

## Links

- Docs: [docs.manalens.dev/sdks/unreal](https://docs.manalens.dev/sdks/unreal)
- Support: [manalens.dev/support](https://manalens.dev/support)

## License

MIT. See [LICENSE](LICENSE).
