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
	 * game_open, sessions, or unclean exits, and is not counted as an install.
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
