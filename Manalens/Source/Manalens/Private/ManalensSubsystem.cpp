// Copyright (c) 2026 Manalens. MIT License.

#include "ManalensSubsystem.h"

#include "Async/Async.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "ManalensClient.h"
#include "ManalensSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"

/** Collects Error (and optionally Warning) lines of the engine log from any thread. The subsystem sends them on the game thread. */
class FManalensLogCapture : public FOutputDevice
{
public:
	struct FItem
	{
		FString Category;
		FString Message;
		bool bWarning = false;
	};

	explicit FManalensLogCapture(bool bInWarnings)
		: bWarnings(bInWarnings)
	{
	}

	virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override
	{
		const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		const bool bWarning = Level == ELogVerbosity::Warning;
		if (Level != ELogVerbosity::Error && !(bWarnings && bWarning))
		{
			return;
		}
		// Skip the SDK's own lines (a loop), failed sends, and online and platform services.
		if (FManalensClient::IsSkippedLogCategory(Category.ToString()))
		{
			return;
		}
		FScopeLock Lock(&Mutex);
		if (Pending.Num() >= 100)
		{
			return;
		}
		Pending.Add({ Category.ToString(), FString(Text), bWarning });
	}

	virtual bool CanBeUsedOnAnyThread() const override { return true; }
	virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

	/** At most Max items, oldest first. */
	TArray<FItem> Take(int32 Max)
	{
		FScopeLock Lock(&Mutex);
		const int32 Count = FMath::Min(Max, Pending.Num());
		TArray<FItem> Out(Pending.GetData(), Count);
		Pending.RemoveAt(0, Count);
		return Out;
	}

private:
	bool bWarnings = false;
	FCriticalSection Mutex;
	TArray<FItem> Pending;
};

void UManalensSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const UManalensSettings* Settings = GetDefault<UManalensSettings>();
	if (!Settings->bEnabled)
	{
		return; // Every call is a no-op.
	}
	if (GIsEditor && !Settings->bEnabledInEditor)
	{
		if (Settings->bDebug)
		{
			UE_LOG(LogManalens, Log, TEXT("Manalens: off in the editor (Enabled In Editor is false); no events are sent"));
		}
		return;
	}
	if (IsRunningDedicatedServer() || IsRunningCommandlet())
	{
		return; // Player analytics only: a dedicated server is not a player.
	}

	FManalensConfig Config;
	Config.PublishableKey = Settings->PublishableKey.TrimStartAndEnd();
	Config.Endpoint = Settings->Endpoint.TrimStartAndEnd();
	Config.AppVersion = Settings->AppVersion;
	if (Config.AppVersion.IsEmpty() && GConfig)
	{
		GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), Config.AppVersion, GGameIni);
	}
	Config.Build = GIsEditor && !Settings->EditorBuild.IsEmpty() ? Settings->EditorBuild : Settings->Build;
	Config.Storefront = UManalensSettings::StorefrontValue(Settings->Storefront);
	Config.bConsentRequired = Settings->bConsentRequired;
	Config.bDebug = Settings->bDebug;
	Config.FlushIntervalSeconds = Settings->FlushIntervalSeconds;
	Config.FlushAt = Settings->FlushAt;
	Config.MaxQueue = Settings->MaxQueue;
	Config.MaxAutoErrorsPerSession = Settings->MaxAutoErrorsPerSession;
	QuitTimeoutSeconds = FMath::Clamp(Settings->QuitTimeoutSeconds, 0.0f, 10.0f);

	Client = MakeShared<FManalensClient>();
	TWeakObjectPtr<UManalensSubsystem> WeakThis(this);
	Client->OnSendFailed = [WeakThis](int32 Status, int32 Dropped)
	{
		if (WeakThis.IsValid())
		{
			WeakThis->OnSendFailed.Broadcast(Status, Dropped);
		}
	};
	Client->OnFlushed = [WeakThis](int32 Sent, bool bOk)
	{
		if (WeakThis.IsValid())
		{
			WeakThis->OnFlushed.Broadcast(Sent, bOk);
		}
	};
	if (!Client->Init(Config))
	{
		Client.Reset();
		return;
	}

	if (Settings->bCaptureLogErrors && GLog)
	{
		LogCapture = MakeShared<FManalensLogCapture>(Settings->bCaptureLogWarnings);
		GLog->AddOutputDevice(LogCapture.Get());
	}
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UManalensSubsystem::OnTick), 0.0f);
	FCoreDelegates::ApplicationWillDeactivateDelegate.AddUObject(this, &UManalensSubsystem::OnDeactivate);
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &UManalensSubsystem::OnDeactivate);
	FCoreDelegates::ApplicationHasReactivatedDelegate.AddUObject(this, &UManalensSubsystem::OnReactivate);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &UManalensSubsystem::OnReactivate);
	FCoreDelegates::GetApplicationWillTerminateDelegate().AddUObject(this, &UManalensSubsystem::OnTerminate);
}

void UManalensSubsystem::Deinitialize()
{
	Shutdown();
	if (TickHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
		TickHandle.Reset();
	}
	FCoreDelegates::ApplicationWillDeactivateDelegate.RemoveAll(this);
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.RemoveAll(this);
	FCoreDelegates::ApplicationHasReactivatedDelegate.RemoveAll(this);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.RemoveAll(this);
	FCoreDelegates::GetApplicationWillTerminateDelegate().RemoveAll(this);
	if (LogCapture.IsValid())
	{
		if (GLog)
		{
			GLog->RemoveOutputDevice(LogCapture.Get());
		}
		LogCapture.Reset();
	}
	Client.Reset();
	Super::Deinitialize();
}

UManalensSubsystem* UManalensSubsystem::Get(const UObject* WorldContextObject)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	const UGameInstance* GameInstance = World ? World->GetGameInstance() : nullptr;
	return GameInstance ? GameInstance->GetSubsystem<UManalensSubsystem>() : nullptr;
}

bool UManalensSubsystem::OnTick(float DeltaTime)
{
	if (!Client.IsValid())
	{
		return true;
	}
	if (LogCapture.IsValid())
	{
		// At most 5 log errors in one frame; the rest wait for the next frames.
		for (const FManalensLogCapture::FItem& Item : LogCapture->Take(5))
		{
			Client->TrackLogError(Item.Category, Item.Message, Item.bWarning);
		}
	}
	Client->Update();
	return true;
}

void UManalensSubsystem::OnDeactivate()
{
	if (Client.IsValid())
	{
		Client->SetFocused(false);
	}
}

void UManalensSubsystem::OnReactivate()
{
	if (Client.IsValid())
	{
		Client->SetFocused(true);
	}
}

void UManalensSubsystem::OnTerminate()
{
	Shutdown();
}

void UManalensSubsystem::Shutdown()
{
	if (bShutDown || !Client.IsValid())
	{
		return;
	}
	bShutDown = true;
	// Send session_end and the queue, for at most QuitTimeoutSeconds. Unsent events stay on disk.
	Client->BeginShutdown();
	if (FModuleManager::Get().IsModuleLoaded(TEXT("HTTP")))
	{
		const double Deadline = FPlatformTime::Seconds() + QuitTimeoutSeconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			if (!Client->IsSending())
			{
				if (Client->GetQueueSize() == 0)
				{
					break;
				}
				Client->Flush();
				if (!Client->IsSending())
				{
					break; // Backoff after a failure: keep the events for the next launch.
				}
			}
			FHttpModule::Get().GetHttpManager().Tick(0.0f);
			FPlatformProcess::Sleep(0.01f);
		}
	}
	Client->FinishShutdown();
}

TSharedPtr<FJsonObject> UManalensSubsystem::ToJson(const TMap<FString, FString>& Strings, const TMap<FString, double>& Numbers, const TMap<FString, bool>& Flags)
{
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Strings)
	{
		Out->SetStringField(Pair.Key, Pair.Value);
	}
	for (const TPair<FString, double>& Pair : Numbers)
	{
		Out->SetNumberField(Pair.Key, Pair.Value);
	}
	for (const TPair<FString, bool>& Pair : Flags)
	{
		Out->SetBoolField(Pair.Key, Pair.Value);
	}
	return Out;
}

void UManalensSubsystem::Track(const FString& EventName, const TMap<FString, FString>& Strings, const TMap<FString, double>& Numbers, const TMap<FString, bool>& Flags)
{
	TrackJson(EventName, ToJson(Strings, Numbers, Flags));
}

void UManalensSubsystem::TrackJson(const FString& EventName, const TSharedPtr<FJsonObject>& Properties)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UManalensSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, EventName, Properties]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->TrackJson(EventName, Properties);
			}
		});
		return;
	}
	if (Client.IsValid())
	{
		Client->Track(EventName, Properties);
	}
}

void UManalensSubsystem::Identify(const FString& UserId, const TMap<FString, FString>& Strings, const TMap<FString, double>& Numbers, const TMap<FString, bool>& Flags)
{
	IdentifyJson(UserId, ToJson(Strings, Numbers, Flags));
}

void UManalensSubsystem::IdentifyJson(const FString& UserId, const TSharedPtr<FJsonObject>& Traits)
{
	if (Client.IsValid())
	{
		Client->Identify(UserId, Traits);
	}
}

void UManalensSubsystem::SetConsent(bool bGranted)
{
	if (Client.IsValid())
	{
		Client->SetConsent(bGranted);
	}
}

void UManalensSubsystem::Flush()
{
	if (Client.IsValid())
	{
		Client->Flush();
	}
}

void UManalensSubsystem::TrackError(const FString& Message, const FManalensErrorOptions& Options)
{
	TrackErrorJson(Message, Options, nullptr);
}

void UManalensSubsystem::TrackErrorJson(const FString& Message, const FManalensErrorOptions& Options, const TSharedPtr<FJsonObject>& Properties)
{
	if (!IsInGameThread())
	{
		TWeakObjectPtr<UManalensSubsystem> WeakThis(this);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Message, Options, Properties]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->TrackErrorJson(Message, Options, Properties);
			}
		});
		return;
	}
	if (Client.IsValid())
	{
		Client->TrackError(Message, Options, Properties);
	}
}

void UManalensSubsystem::SetBeforeSend(TFunction<TSharedPtr<FJsonObject>(TSharedPtr<FJsonObject>)> Callback)
{
	if (Client.IsValid())
	{
		Client->BeforeSend = MoveTemp(Callback);
	}
}

void UManalensSubsystem::Reset()
{
	if (Client.IsValid())
	{
		Client->Reset();
	}
}

FString UManalensSubsystem::LinkUrl(const FString& Url) const
{
	return Client.IsValid() ? Client->LinkUrl(Url) : Url;
}

FString UManalensSubsystem::GetAnonymousId() const
{
	return Client.IsValid() ? Client->GetAnonymousId() : FString();
}

FString UManalensSubsystem::GetSessionId() const
{
	return Client.IsValid() ? Client->GetSessionId() : FString();
}

bool UManalensSubsystem::IsActive() const
{
	return Client.IsValid() && Client->IsInitialized();
}
