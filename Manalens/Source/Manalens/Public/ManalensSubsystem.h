// Copyright (c) 2026 Manalens. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "ManalensTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ManalensSubsystem.generated.h"

class FManalensClient;
class FManalensLogCapture;

/**
 * Manalens for Unreal Engine. The engine creates it with the game instance and
 * starts it from Project Settings > Plugins > Manalens, so there is no init
 * call. It sends session_start and game_open, keeps an offline queue on disk,
 * and sends session_end on quit.
 *
 *   UManalensSubsystem* Manalens = GetGameInstance()->GetSubsystem<UManalensSubsystem>();
 *   Manalens->Track(TEXT("level_complete"), {}, { { TEXT("level"), 3 }, { TEXT("score"), 1200 } });
 *
 * Call it on the game thread. Track and Track Error from another thread are
 * moved to the game thread.
 */
UCLASS()
class MANALENS_API UManalensSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** The subsystem of the game instance of this object, or nullptr. */
	static UManalensSubsystem* Get(const UObject* WorldContextObject);

	/** Record a game event. Use flat snake_case names, for example level_complete. */
	UFUNCTION(BlueprintCallable, Category = "Manalens", meta = (AutoCreateRefTerm = "Strings,Numbers,Flags"))
	void Track(const FString& EventName, const TMap<FString, FString>& Strings, const TMap<FString, double>& Numbers, const TMap<FString, bool>& Flags);

	/** Record a game event with a JSON object of scalar properties (C++). */
	void TrackJson(const FString& EventName, const TSharedPtr<FJsonObject>& Properties);

	/** Attach your player ID (1 to 255 characters) and traits to this install, for example after sign-in. */
	UFUNCTION(BlueprintCallable, Category = "Manalens", meta = (AutoCreateRefTerm = "Strings,Numbers,Flags"))
	void Identify(const FString& UserId, const TMap<FString, FString>& Strings, const TMap<FString, double>& Numbers, const TMap<FString, bool>& Flags);

	/** Identify with a JSON object of scalar traits (C++). */
	void IdentifyJson(const FString& UserId, const TSharedPtr<FJsonObject>& Traits);

	/** true starts tracking and sends game_open. false deletes the player ID, the session, and every unsent event. The choice is saved. */
	UFUNCTION(BlueprintCallable, Category = "Manalens")
	void SetConsent(bool bGranted);

	/** Send queued events now. */
	UFUNCTION(BlueprintCallable, Category = "Manalens")
	void Flush();

	/** Record an error as an error event. */
	UFUNCTION(BlueprintCallable, Category = "Manalens", meta = (AutoCreateRefTerm = "Options"))
	void TrackError(const FString& Message, const FManalensErrorOptions& Options);

	/** Record an error with more scalar properties (C++). */
	void TrackErrorJson(const FString& Message, const FManalensErrorOptions& Options, const TSharedPtr<FJsonObject>& Properties);

	/** A function that gets the properties of each error event before it is queued. Return nullptr to drop the error (C++). */
	void SetBeforeSend(TFunction<TSharedPtr<FJsonObject>(TSharedPtr<FJsonObject>)> Callback);

	/** Forget the player on sign-out: a new anonymous ID and a new session. */
	UFUNCTION(BlueprintCallable, Category = "Manalens")
	void Reset();

	/** Add this install's handoff parameter (mana_hid) to a website URL before the game opens it. */
	UFUNCTION(BlueprintPure, Category = "Manalens")
	FString LinkUrl(const FString& Url) const;

	UFUNCTION(BlueprintPure, Category = "Manalens")
	FString GetAnonymousId() const;

	UFUNCTION(BlueprintPure, Category = "Manalens")
	FString GetSessionId() const;

	/** true when the SDK started (enabled, with a valid key and endpoint). */
	UFUNCTION(BlueprintPure, Category = "Manalens")
	bool IsActive() const;

	/** Events were dropped and will never be sent. Status is the HTTP status of a refusal, or 0 when the SDK dropped them itself. */
	UPROPERTY(BlueprintAssignable, Category = "Manalens")
	FManalensSendFailed OnSendFailed;

	/** After each send. bOk is false for a network error or a 5xx, 408, or 429 response; those events stay queued. */
	UPROPERTY(BlueprintAssignable, Category = "Manalens")
	FManalensFlushed OnFlushed;

	/** The client, for tests and advanced C++ use. Null when the SDK is off. */
	TSharedPtr<FManalensClient> GetClient() const { return Client; }

private:
	bool OnTick(float DeltaTime);
	void OnDeactivate();
	void OnReactivate();
	void OnTerminate();
	void Shutdown();
	static TSharedPtr<FJsonObject> ToJson(const TMap<FString, FString>& Strings, const TMap<FString, double>& Numbers, const TMap<FString, bool>& Flags);

	TSharedPtr<FManalensClient> Client;
	TSharedPtr<FManalensLogCapture> LogCapture;
	FTSTicker::FDelegateHandle TickHandle;
	TArray<FDelegateHandle> DelegateHandles;
	float QuitTimeoutSeconds = 2.0f;
	bool bShutDown = false;
};
