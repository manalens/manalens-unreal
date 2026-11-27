// Copyright (c) 2026 Manalens. MIT License.

#include "ManalensHangWatchdog.h"

#include "HAL/PlatformProcess.h"
#include "HAL/RunnableThread.h"
#include "ManalensClient.h"
#include "Misc/ScopeLock.h"

FManalensHangWatchdog::FManalensHangWatchdog(double InThresholdSeconds, TFunction<double()> InMonotonic, TFunction<double()> InWall, TFunction<void(const FHang&)> InOnStuck)
	: FManalensHangWatchdog(FExactThreshold(), FMath::Clamp(InThresholdSeconds, MinThresholdSeconds, MaxThresholdSeconds), MoveTemp(InMonotonic), MoveTemp(InWall), MoveTemp(InOnStuck))
{
}

TUniquePtr<FManalensHangWatchdog> FManalensHangWatchdog::MakeForTests(double InThresholdSeconds, TFunction<double()> InMonotonic, TFunction<double()> InWall, TFunction<void(const FHang&)> InOnStuck)
{
	return TUniquePtr<FManalensHangWatchdog>(new FManalensHangWatchdog(FExactThreshold(), InThresholdSeconds, MoveTemp(InMonotonic), MoveTemp(InWall), MoveTemp(InOnStuck)));
}

FManalensHangWatchdog::FManalensHangWatchdog(FExactThreshold, double InThresholdSeconds, TFunction<double()> InMonotonic, TFunction<double()> InWall, TFunction<void(const FHang&)> InOnStuck)
	: ThresholdSeconds(FMath::Max(0.001, InThresholdSeconds))
	, Poll(FMath::Clamp(ThresholdSeconds / 4.0, 0.01, PollSeconds))
	, Monotonic(MoveTemp(InMonotonic))
	, Wall(MoveTemp(InWall))
	, OnStuck(MoveTemp(InOnStuck))
{
	LastBeat = Monotonic();
}

FManalensHangWatchdog::~FManalensHangWatchdog()
{
	StopThread();
}

bool FManalensHangWatchdog::Beat(bool bInCounting, const FString& SessionId, FHang& OutEnded)
{
	FScopeLock Lock(&Mutex);
	const double At = Monotonic();
	const bool bEnded = bHasCurrent;
	if (bEnded)
	{
		OutEnded = Current;
		OutEnded.DurationSeconds = At - LastBeat;
	}
	bHasCurrent = false;
	LastBeat = At;
	if (Frames <= WarmupFrames)
	{
		++Frames;
	}
	bCounting = bInCounting && Frames > WarmupFrames;
	Session = SessionId;
	// A map load without PostLoadMap (a failed load): turn detection back on.
	if ((PausedBy & static_cast<uint8>(EPauseCause::MapLoad)) != 0 && At - MapLoadPausedAt >= MapLoadPauseTimeoutSeconds)
	{
		PausedBy &= ~static_cast<uint8>(EPauseCause::MapLoad);
	}
	return bEnded;
}

void FManalensHangWatchdog::SetPaused(EPauseCause Cause, bool bInPaused)
{
	FScopeLock Lock(&Mutex);
	const uint8 Flag = static_cast<uint8>(Cause);
	PausedBy = bInPaused ? (PausedBy | Flag) : (PausedBy & ~Flag);
	LastBeat = Monotonic();
	if (bInPaused && Cause == EPauseCause::MapLoad)
	{
		MapLoadPausedAt = LastBeat;
	}
}

bool FManalensHangWatchdog::IsPaused() const
{
	FScopeLock Lock(&Mutex);
	return PausedBy != 0;
}

void FManalensHangWatchdog::SetBreadcrumbs(const FString& Breadcrumbs)
{
	FScopeLock Lock(&Mutex);
	Crumbs = Breadcrumbs;
}

bool FManalensHangWatchdog::HasCurrent() const
{
	FScopeLock Lock(&Mutex);
	return bHasCurrent;
}

void FManalensHangWatchdog::ClearSaved(TFunctionRef<void()> Delete, bool bForget)
{
	FScopeLock Lock(&Mutex);
	if (bForget)
	{
		bHasCurrent = false;
		bCounting = false;
		Session.Reset();
		Crumbs.Reset();
	}
	if (!bHasCurrent)
	{
		Delete();
	}
}

void FManalensHangWatchdog::Check()
{
	FString Id;
	TArray<uint64> Addresses;
	{
		FScopeLock Lock(&Mutex);
		const double At = Monotonic();
		// This thread itself slept much longer than asked: the whole process was
		// stopped (system sleep, a mobile app in the background). Not a hang.
		if (LastCheck >= 0.0 && At - LastCheck > Poll * 4.0 + 1.0)
		{
			LastBeat = At;
		}
		LastCheck = At;
		if (!bCounting || PausedBy != 0 || bHasCurrent || At - LastBeat < ThresholdSeconds)
		{
			return;
		}
		if (IsDebuggerPaused && IsDebuggerPaused())
		{
			LastBeat = At;
			return;
		}
		bHasCurrent = true;
		Current = FHang();
		Current.MessageId = FManalensClient::NewUuidV4();
		Current.SessionId = Session;
		Current.Breadcrumbs = Crumbs;
		Current.StartedAt = Wall() - (At - LastBeat);
		Current.ThresholdSeconds = ThresholdSeconds;
		// Under the lock: the game thread cannot take the hang back (Beat) before
		// it is on disk. Write it first, then read the stack addresses (fast).
		if (OnStuck)
		{
			OnStuck(Current);
		}
		if (!CaptureStackAddresses || !SymbolizeStack)
		{
			return;
		}
		Addresses = CaptureStackAddresses();
		Id = Current.MessageId;
	}
	// Outside the lock: symbol lookup can take a long time, and the game thread
	// must not wait for it in Beat() when it comes back.
	const FString Stack = Addresses.Num() > 0 ? SymbolizeStack(Addresses) : FString();
	if (Stack.IsEmpty())
	{
		return;
	}
	FScopeLock Lock(&Mutex);
	// Only for the same hang: the game thread may be back (Beat), or the hang
	// forgotten (consent withdrawal, reset) while the stack was symbolized.
	if (bHasCurrent && Current.MessageId == Id)
	{
		Current.Stack = Stack;
		if (OnStuck)
		{
			OnStuck(Current);
		}
	}
}

void FManalensHangWatchdog::StartThread()
{
	if (Thread == nullptr)
	{
		bStopping = false;
		Thread = FRunnableThread::Create(this, TEXT("ManalensHangWatchdog"), 256 * 1024, TPri_BelowNormal);
	}
}

void FManalensHangWatchdog::StopThread()
{
	if (Thread != nullptr)
	{
		bStopping = true;
		Thread->WaitForCompletion();
		delete Thread;
		Thread = nullptr;
	}
}

uint32 FManalensHangWatchdog::Run()
{
	while (!bStopping)
	{
		FPlatformProcess::SleepNoStats(static_cast<float>(Poll));
		if (bStopping)
		{
			break;
		}
		Check();
	}
	return 0;
}

void FManalensHangWatchdog::Stop()
{
	bStopping = true;
}
