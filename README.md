# Manalens for Unreal Engine

Game analytics for Unreal Engine 5, as a C++ plugin with Blueprint nodes. The plugin sends sessions, `game_open`, your game events, and errors to [Manalens](https://manalens.dev) with your project Publishable key. It keeps an offline queue on disk and has a consent mode.

It uses only engine modules (Core, CoreUObject, Engine, HTTP, Json, and DeveloperSettings) and supports Windows, macOS, Linux, Android, and iOS.

## Install

1. Copy the `Manalens` folder into the `Plugins/` folder of your project.
2. Open the project and let the editor build the plugin. A Blueprint-only project needs one C++ class first (**Tools > New C++ Class**).
3. Open **Edit > Project Settings > Plugins > Manalens** and set the **Publishable Key** and the **Endpoint**.

The engine starts the SDK with the game instance. It also sends from Play in Editor, so you can watch your events in Activity and Live View while you play. Dev and live data go to the same project. Play in Editor runs send **Editor Build** (default `dev`) in place of **Build**, so you can filter them out. To tell other test builds apart, set **Build** (for example `dev`) and filter by it. To send nothing from the editor, turn off **Enabled In Editor**.

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

The other nodes are Identify, Set Consent, Flush, Track Error, Reset, and Link URL. The full guide, with every setting, is in the [Unreal SDK docs](https://docs.manalens.dev/sdks/unreal).

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

## Test

```bash
UE_ROOT="/Users/Shared/Epic Games/UE_5.8" Tests/run-tests.sh
```

The script builds the plugin, copies it into `HostProject/Plugins`, and runs `Automation RunTests Manalens` with `UnrealEditor-Cmd`. Set `UPDATE_FIXTURES=1` to write the request bodies to `Tests/fixtures`.

## Links

- Docs: [docs.manalens.dev/sdks/unreal](https://docs.manalens.dev/sdks/unreal)
- Support: [manalens.dev/support](https://manalens.dev/support)

## License

MIT. See [LICENSE](LICENSE).
