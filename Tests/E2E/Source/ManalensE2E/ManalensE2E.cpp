// Copyright (c) 2026 Manalens. MIT License.
//
// The game side of scripts/test-unreal-sdk-local.mjs in the Manalens monorepo.
// With -ManalensE2E on the command line, it records one custom event, waits for
// the send, writes the anonymous and session IDs to -ManalensResult=<file>, and
// quits. The plugin sends session_start and game_open at start and session_end on quit.

#include "ManalensE2E.h"

#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "ManalensSubsystem.h"

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, ManalensE2E, "ManalensE2E");

bool UManalensE2ESubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	return FParse::Param(FCommandLine::Get(), TEXT("ManalensE2E"));
}

void UManalensE2ESubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Manalens = Collection.InitializeDependency<UManalensSubsystem>();
	FParse::Value(FCommandLine::Get(), TEXT("ManalensRunId="), RunId);
	FParse::Value(FCommandLine::Get(), TEXT("ManalensResult="), ResultPath);
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UManalensE2ESubsystem::OnTick), 0.0f);
}

void UManalensE2ESubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	Super::Deinitialize();
}

bool UManalensE2ESubsystem::OnTick(float DeltaTime)
{
	if (bFinished)
	{
		return false;
	}
	Elapsed += DeltaTime;
	if (!Manalens || !Manalens->IsActive())
	{
		Finish(TEXT("{\"active\":false}"));
		return false;
	}
	if (!bTracked && Elapsed >= 0.5)
	{
		bTracked = true;
		Manalens->Track(TEXT("e2e_level_complete"), { { TEXT("run_id"), RunId } }, { { TEXT("level"), 3 }, { TEXT("score"), 1200 } }, { { TEXT("perfect"), true } });
		Manalens->Flush();
	}
	if (bTracked && Elapsed >= 3.0)
	{
		Finish(FString::Printf(TEXT("{\"active\":true,\"anonymous_id\":\"%s\",\"session_id\":\"%s\"}"), *Manalens->GetAnonymousId(), *Manalens->GetSessionId()));
		return false;
	}
	return true;
}

void UManalensE2ESubsystem::Finish(const FString& Result)
{
	bFinished = true;
	if (!ResultPath.IsEmpty())
	{
		FFileHelper::SaveStringToFile(Result, *ResultPath);
	}
	// A normal quit: the plugin sends session_end and the rest of the queue.
	FPlatformMisc::RequestExit(false, TEXT("ManalensE2E"));
}
