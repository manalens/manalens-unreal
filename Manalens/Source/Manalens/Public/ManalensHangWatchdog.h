// Copyright (c) 2026 Manalens. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "HAL/ThreadSafeBool.h"
#include "Templates/Function.h"

class FRunnableThread;

/**
 * Hang detection (P-2833). The game thread writes a heartbeat once per frame
 * (Beat). A watchdog thread checks it (Check). When the heartbeat is older than
 * the threshold, the game thread is stuck: the watchdog calls OnStuck at once,
 * on its own thread, so the hang is written to disk before the player kills
 * the game. Then it captures the raw return addresses of the game thread
 * (CaptureStackAddresses, under the lock), symbolizes them outside the lock
 * (SymbolizeStack, which can be slow), and calls OnStuck again with the stack
 * only if the same hang is still current. One hang per stuck period. The next
 * Beat after it gives the hang back with its duration.
 *
 * Thread safety: every member takes one lock of its own. The watchdog never
 * calls FManalensClient, so a game thread that is stuck inside the SDK cannot
 * stop it.
 */
class MANALENS_API FManalensHangWatchdog : public FRunnable
{
public:
	/** A hang is not counted in the first frames after the start (a long first load). */
	static constexpr int32 WarmupFrames = 10;
	/** The watchdog checks the heartbeat this often (at most). */
	static constexpr double PollSeconds = 0.25;
	/** The smallest and the largest threshold (0 turns detection off before a watchdog is made). */
	static constexpr double MinThresholdSeconds = 1.0;
	static constexpr double MaxThresholdSeconds = 60.0;
	/**
	 * A map load that never ends (PreLoadMap without PostLoadMap, for example a
	 * failed load) does not turn detection off for the session: the first frame
	 * this long after PreLoadMap clears the map-load pause.
	 */
	static constexpr double MapLoadPauseTimeoutSeconds = 120.0;

	/** Why the watchdog does not count. Each cause has its own flag; any set flag pauses it. */
	enum class EPauseCause : uint8
	{
		/** The game lost focus or is in the background. */
		Focus = 1,
		/** A map load (PreLoadMap to PostLoadMap). */
		MapLoad = 2,
	};

	/** One stuck period of the game thread. */
	struct FHang
	{
		/** The message_id of the error event, made when the hang starts, so a crash can link to it. */
		FString MessageId;
		/** The session of the last heartbeat. */
		FString SessionId;
		/** The breadcrumbs of the last heartbeat. */
		FString Breadcrumbs;
		/** The game thread stack, when CaptureStack gave one. */
		FString Stack;
		/** Unix seconds of the last heartbeat before the hang. */
		double StartedAt = 0.0;
		double ThresholdSeconds = 0.0;
		/** Set by Beat() when the game thread is back: how long it was stuck. */
		double DurationSeconds = 0.0;
	};

	/**
	 * Monotonic: seconds of a clock that never jumps (FPlatformTime::Seconds).
	 * Wall: Unix seconds. OnStuck runs on the watchdog thread. The threshold is
	 * clamped to MinThresholdSeconds to MaxThresholdSeconds.
	 */
	FManalensHangWatchdog(double InThresholdSeconds, TFunction<double()> InMonotonic, TFunction<double()> InWall, TFunction<void(const FHang&)> InOnStuck);
	/** Tests only: a watchdog with a threshold under the minimum. */
	static TUniquePtr<FManalensHangWatchdog> MakeForTests(double InThresholdSeconds, TFunction<double()> InMonotonic, TFunction<double()> InWall, TFunction<void(const FHang&)> InOnStuck);
	virtual ~FManalensHangWatchdog() override;

	/**
	 * Watchdog thread: true while the game thread is stopped on purpose (a
	 * debugger breakpoint, a Blueprint breakpoint). Not a hang.
	 */
	TFunction<bool()> IsDebuggerPaused;
	/**
	 * Watchdog thread, under the lock: the raw return addresses of the stuck
	 * game thread (fast). Empty function: no stack.
	 */
	TFunction<TArray<uint64>()> CaptureStackAddresses;
	/** Watchdog thread, outside the lock: the stack text of the addresses (slow), or empty. */
	TFunction<FString(const TArray<uint64>&)> SymbolizeStack;

	/**
	 * Game thread, once per frame. bCounting: the game is focused and the SDK
	 * records. Returns true and the hang that just ended (with its duration) in
	 * OutEnded.
	 */
	bool Beat(bool bCounting, const FString& SessionId, FHang& OutEnded);
	/**
	 * Any thread: sets or clears one pause cause (focus loss, a map load). No
	 * hang while any cause is set. The time paused does not count.
	 */
	void SetPaused(EPauseCause Cause, bool bPaused);
	/** true while any pause cause is set. */
	bool IsPaused() const;
	/** The breadcrumbs that a hang carries (game thread, when they change). */
	void SetBreadcrumbs(const FString& Breadcrumbs);
	/** true while a stuck period runs (a hang was seen and the game thread is not back). */
	bool HasCurrent() const;
	/**
	 * Runs Delete (the saved hang) unless a new hang started since the last
	 * Beat(): its file must stay. bForget (consent withdrawn, reset): delete it
	 * in any case, forget the hang of this stuck period, stop counting until the
	 * next Beat(), and clear the session and the breadcrumbs, so no hang is
	 * written with the old player's data.
	 */
	void ClearSaved(TFunctionRef<void()> Delete, bool bForget = false);
	/** One check (the watchdog thread; the tests call it directly). */
	void Check();
	double GetThresholdSeconds() const { return ThresholdSeconds; }

	/** Starts the watchdog thread. */
	void StartThread();
	/** Stops the watchdog thread and waits for it. */
	void StopThread();

	// FRunnable
	virtual uint32 Run() override;
	virtual void Stop() override;

private:
	struct FExactThreshold {};
	FManalensHangWatchdog(FExactThreshold, double InThresholdSeconds, TFunction<double()> InMonotonic, TFunction<double()> InWall, TFunction<void(const FHang&)> InOnStuck);

	mutable FCriticalSection Mutex;
	const double ThresholdSeconds;
	const double Poll;
	TFunction<double()> Monotonic;
	TFunction<double()> Wall;
	TFunction<void(const FHang&)> OnStuck;
	double LastBeat = 0.0;
	double LastCheck = -1.0;
	int32 Frames = 0;
	bool bCounting = false;
	/** EPauseCause flags. */
	uint8 PausedBy = 0;
	/** Monotonic seconds of the last PreLoadMap (EPauseCause::MapLoad). */
	double MapLoadPausedAt = 0.0;
	bool bHasCurrent = false;
	FHang Current;
	FString Session;
	FString Crumbs;
	FThreadSafeBool bStopping;
	FRunnableThread* Thread = nullptr;
};
