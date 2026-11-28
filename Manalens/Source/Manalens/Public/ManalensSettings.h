// Copyright (c) 2026 Manalens. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "ManalensTypes.h"
#include "ManalensSettings.generated.h"

/**
 * Project Settings > Plugins > Manalens. Saved in Config/DefaultGame.ini under
 * [/Script/Manalens.ManalensSettings].
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Manalens"))
class MANALENS_API UManalensSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UManalensSettings();

	/** The project Publishable key (mana_pk_...), from your Manalens project settings. Never a Secret key or an API key. */
	UPROPERTY(config, EditAnywhere, Category = "Project")
	FString PublishableKey;

	/** The Manalens ingest URL, without /v0/events. */
	UPROPERTY(config, EditAnywhere, Category = "Project")
	FString Endpoint;

	/** The API URL of Link Steam Ticket. Empty: the Endpoint with events. changed to api. */
	UPROPERTY(config, EditAnywhere, Category = "Advanced")
	FString ApiEndpoint;

	/** false: the SDK sends nothing, writes no files, and keeps no ID. */
	UPROPERTY(config, EditAnywhere, Category = "Project")
	bool bEnabled = true;

	/** Send events from the editor (Play in Editor). On by default: editor and live data go to the same project, so set Build (for example "dev") to tell them apart. */
	UPROPERTY(config, EditAnywhere, Category = "Project")
	bool bEnabledInEditor = true;

	/** Up to 32 characters, for example 1.4.0. Empty: the Project Version in Project Settings > Description. */
	UPROPERTY(config, EditAnywhere, Category = "Build")
	FString AppVersion;

	/** Up to 64 characters, for example a build number. */
	UPROPERTY(config, EditAnywhere, Category = "Build")
	FString Build;

	/** The build label of Play in Editor runs, so you can filter them out. Empty: use Build. A packaged build and a -game run use Build. */
	UPROPERTY(config, EditAnywhere, Category = "Build")
	FString EditorBuild = TEXT("dev");

	/** The store this build is sold on. */
	UPROPERTY(config, EditAnywhere, Category = "Build")
	EManalensStorefront Storefront = EManalensStorefront::None;

	/**
	 * Empty: the platform of this build (windows, macos, linux, android, ios,
	 * ps5, xbox_series, xbox_one, switch, switch2, or other). Set it when the
	 * plugin cannot name the console, for example switch2, in the platform's
	 * own Game.ini so that it applies to that platform only.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Build")
	FString Platform;

	/**
	 * Steam Link: verify the player's Steam account once per launch, after
	 * consent, so Manalens shows Steam ownership and joins the player's installs.
	 * Needs the Steam Online Subsystem (the OnlineSubsystemSteam plugin enabled
	 * and the Steam client running); without it nothing happens. The plugin asks
	 * Steam for a Web API ticket with the identity "manalens" 20 seconds after
	 * the first accepted event batch of the launch. Link Steam Ticket from your
	 * own code does not wait. Off by default.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Steam", meta = (DisplayName = "Steam Link"))
	bool bSteamLink = false;

	/** No events and no stored ID until Set Consent (true). */
	UPROPERTY(config, EditAnywhere, Category = "Privacy")
	bool bConsentRequired = false;

	/** Send Error lines of the engine log (UE_LOG) as error events. */
	UPROPERTY(config, EditAnywhere, Category = "Errors")
	bool bCaptureLogErrors = true;

	/** Also send Warning lines of the engine log. Often noisy. */
	UPROPERTY(config, EditAnywhere, Category = "Errors")
	bool bCaptureLogWarnings = false;

	/** Most captured log errors in one session. Track Error calls do not count. */
	UPROPERTY(config, EditAnywhere, Category = "Errors", meta = (ClampMin = "0"))
	int32 MaxAutoErrorsPerSession = 50;

	/**
	 * Windows, macOS, Linux: at the next launch, add the crash stack that the engine
	 * wrote for the crashed run (Saved/Crashes, CrashContext.runtime-xml) to game_crash.
	 * macOS writes it only when the build includes the crash reporter.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Errors")
	bool bCaptureCrashStacks = true;

	/**
	 * Hang detection: send an error with error_type game_hang when one frame takes
	 * longer than this many seconds (the game thread is stuck), with the game
	 * thread stack. A watchdog thread writes it to disk while the game is stuck,
	 * so a game that the player kills keeps it, and game_crash links to it. 0 turns
	 * it off; other values are clamped to 1 to 60. Not counted while the game is in
	 * the background, during a map load,
	 * at a debugger or Blueprint breakpoint, or in the first frames.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Errors", meta = (ClampMin = "0", ClampMax = "60"))
	float HangThresholdSeconds = 5.0f;

	/** Seconds between sends. */
	UPROPERTY(config, EditAnywhere, Category = "Advanced", meta = (ClampMin = "1"))
	float FlushIntervalSeconds = 10.0f;

	/** Send when this many events are queued. Also the batch size (at most 50). */
	UPROPERTY(config, EditAnywhere, Category = "Advanced", meta = (ClampMin = "1", ClampMax = "50"))
	int32 FlushAt = 20;

	/** Most events kept on disk. The oldest are dropped first. */
	UPROPERTY(config, EditAnywhere, Category = "Advanced", meta = (ClampMin = "1"))
	int32 MaxQueue = 1000;

	/** Longest wait on quit to send the last events. */
	UPROPERTY(config, EditAnywhere, Category = "Advanced", meta = (ClampMin = "0", ClampMax = "10"))
	float QuitTimeoutSeconds = 2.0f;

	/**
	 * A dedicated server is not a player. Off: a dedicated server sends no
	 * game_open, sessions, or game_crash events, and is not counted as an install.
	 * Track and Identify still work. On: it sends them as a client does.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Advanced")
	bool bTrackServerBuilds = false;

	/** Log each queued event and each failed send. */
	UPROPERTY(config, EditAnywhere, Category = "Advanced")
	bool bDebug = false;

	/** The storefront as the collector value: steam, itch, epic, gog, app_store, play, psn, xbox, nintendo, other, or empty. */
	static FString StorefrontValue(EManalensStorefront Value);
};
