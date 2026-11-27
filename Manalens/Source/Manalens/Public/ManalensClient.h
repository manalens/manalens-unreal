// Copyright (c) 2026 Manalens. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ManalensTypes.h"
#include "Templates/Function.h"
#include "Templates/SharedPointer.h"

MANALENS_API DECLARE_LOG_CATEGORY_EXTERN(LogManalens, Log, All);

/** Options of the client. UManalensSubsystem fills them from the project settings. */
struct MANALENS_API FManalensConfig
{
	/** The project Publishable key (mana_pk_...). A Secret key or an API key is refused. */
	FString PublishableKey;
	/** The Manalens ingest URL, without /v0/events. */
	FString Endpoint;
	/** The API URL of LinkSteamTicket(). Empty: the Endpoint with an events. host changed to api. (https://events.manalens.dev gives https://api.manalens.dev); another endpoint is used as it is. */
	FString ApiEndpoint;
	FString AppVersion;
	FString Build;
	/** steam, itch, epic, gog, app_store, play, psn, xbox, nintendo, other, or empty. */
	FString Storefront;
	/** true: no events and no stored ID until SetConsent(true). */
	bool bConsentRequired = false;
	bool bDebug = false;
	double FlushIntervalSeconds = 10.0;
	/** Send when this many events are queued. Also the batch size (at most 50). */
	int32 FlushAt = 20;
	int32 MaxQueue = 1000;
	int32 MaxAutoErrorsPerSession = 50;
	/** Folder for the state, the queue, and the opt-out marker. Empty: Saved/Manalens. */
	FString StorageDir;
	/** Empty: the platform of this build (PLATFORM_* macros). Else one of Platforms(). */
	FString Platform;
	/** Empty: the current culture, for example en-US. */
	FString Locale;
	/** Minutes from UTC (480 for UTC+8). MAX_int32: read the system clock. Out of -720 to 840: not sent. */
	int32 UtcOffsetMinutes = MAX_int32;
	/** true in a dedicated server (UManalensSubsystem: IsRunningDedicatedServer()). */
	bool bServerBuild = false;
	/**
	 * A dedicated server is not a player. false (default): on a server build the
	 * client sends no game_open, session_start, session_end, or app_crash,
	 * so the server does not count as an install or a session. Track() and
	 * Identify() still work. true: a server build sends them as a client does.
	 */
	bool bTrackServerBuilds = false;
	/**
	 * Crash stacks (sdks/SPEC.md, P-2831). At the next launch on Windows, macOS,
	 * and Linux, add the crash data that the engine wrote for the crashed run
	 * (CrashContext.runtime-xml in a crash folder) to app_crash.
	 */
	bool bCaptureCrashStacks = true;
	/**
	 * Folders that hold crash folders (each with CrashContext.runtime-xml).
	 * Empty: none. UManalensSubsystem sets Saved/Crashes of the project and the
	 * engine user folder (macOS writes there).
	 */
	TArray<FString> CrashDirs;
	/** The project name in CrashContext GameName (UE-<name>). Empty: any. */
	FString CrashGameName;
};

/** The result of one HTTP request. Status 0 is a network error. */
struct FManalensHttpResult
{
	int32 Status = 0;
	FString RetryAfter;
	FString Body;
};

using FManalensSendDone = TFunction<void(const FManalensHttpResult&)>;
using FManalensTransport = TFunction<void(const FString& Url, const TArray<TPair<FString, FString>>& Headers, const FString& Body, FManalensSendDone Done)>;

/** What to do with a batch after a response. */
enum class EManalensSendOutcome : uint8
{
	/** 2xx: the batch is stored. */
	Sent,
	/** Network error, 5xx, 408, or 429: keep the batch, back off, and send it again with the same message_id values. */
	Retry,
	/** Another 4xx: the same batch would be refused again, so drop it. */
	Drop,
	/**
	 * 413: the batch is too large. With more than 1 event, keep the events and
	 * send again at once with half the batch (no backoff, not a failure). With
	 * 1 event, drop it as Drop does.
	 */
	Split,
};

/**
 * The Manalens client: identity, sessions, the offline queue, consent, errors,
 * and sending. It has no UObject and no engine state, so the automation tests
 * drive it with a fake clock, fake IDs, and a fake transport. Use it on the
 * game thread only. Games use UManalensSubsystem.
 */
class MANALENS_API FManalensClient
{
public:
	static constexpr const TCHAR* LibraryName = TEXT("Manalens Unreal SDK");
	static constexpr const TCHAR* LibraryVersion = TEXT("0.5.1");
	/** The source of an error: a Track Error call, or an automatic capture. */
	static constexpr const TCHAR* SourceManual = TEXT("manual");
	static constexpr const TCHAR* SourceAuto = TEXT("auto");
	static constexpr double SessionTimeoutSeconds = 30.0 * 60.0;
	static constexpr double SessionMaxAgeSeconds = 24.0 * 60.0 * 60.0;
	/** The collector accepts app events up to 7 days old. Keep 1 hour of margin for clock skew. */
	static constexpr double MaxEventAgeSeconds = 7.0 * 24.0 * 3600.0 - 3600.0;
	static constexpr double MaxBackoffSeconds = 300.0;
	static constexpr int32 MaxBatch = 50;
	static constexpr int32 MaxPropertyKeys = 50;
	static constexpr int32 MaxPropertyBytes = 8 * 1024;
	static constexpr int32 MaxString = 1024;
	static constexpr int32 Breadcrumbs = 20;
	/** Error limits (as the Godot and Unity SDKs): one fingerprint at most once a second, at most 20 errors in 10 seconds. */
	static constexpr double ErrorDedupeSeconds = 1.0;
	static constexpr double ErrorWindowSeconds = 10.0;
	static constexpr int32 ErrorWindowMax = 20;
	static constexpr const TCHAR* HandoffParam = TEXT("mana_hid");
	static constexpr const TCHAR* StateFileName = TEXT("manalens_state.json");
	static constexpr const TCHAR* QueueFileName = TEXT("manalens_queue.txt");
	static constexpr const TCHAR* OptOutFileName = TEXT("manalens_optout");
	/** LinkSteamTicket(): the identity of the Steam Web API ticket. The server accepts only this one. */
	static constexpr const TCHAR* SteamTicketIdentity = TEXT("manalens");
	/** LinkSteamTicket() waits for a 429 Retry-After of at most this many seconds, once. */
	static constexpr double LinkMaxRetryAfterSeconds = 60.0;
	/** Protected traits (P-2814): only a server with the secret key may set them. Traits() does not send them. */
	static const TArray<FString>& ProtectedTraits();
	/**
	 * FPlatformMisc::GetNetworkConnectionType() is None or AirplaneMode. Only
	 * those values count: Unknown (a platform without a network monitor, or an
	 * older engine) and every connected type do not stop a send.
	 */
	static bool EngineReportsNoNetwork();

	FManalensClient();
	~FManalensClient();

	/** Unix seconds. Tests replace it. */
	TFunction<double()> Now;
	/** A new UUID v4. Tests replace it. */
	TFunction<FString()> NewId;
	/** A number in [0, 1) for the backoff jitter. Tests replace it. */
	TFunction<double()> Random;
	/**
	 * True when the engine reliably reports that the device has no network
	 * (P-2853). Empty (default) or false: the client sends as usual.
	 * UManalensSubsystem sets it to EngineReportsNoNetwork(). Called on the game
	 * thread before each send. While it is true the client starts no request,
	 * keeps the events, and does not change the failure counter or the backoff.
	 * When it is false again, the client clears the backoff and sends at the
	 * next tick.
	 */
	TFunction<bool()> IsOffline;
	/** Sends one request. The default uses the engine HTTP module. */
	FManalensTransport Transport;
	/** Events were dropped and will never be sent. Status is the HTTP status of a refusal, or 0 when the SDK dropped them itself. */
	TFunction<void(int32 /*Status*/, int32 /*Dropped*/)> OnSendFailed;
	/** After each response. bOk is false for a network error or a retried status. */
	TFunction<void(int32 /*Sent*/, bool /*bOk*/)> OnFlushed;
	/**
	 * The answer to LinkSteamTicket() (P-2815): bLinked, the HTTP status (0 when no
	 * request was made or the network failed), and the API error code (for example
	 * STEAM_TICKET_INVALID or STEAM_NOT_CONNECTED) or NETWORK_ERROR,
	 * CONSENT_REQUIRED, or IDENTITY_CHANGED. Empty on success.
	 */
	TFunction<void(bool /*bLinked*/, int32 /*Status*/, const FString& /*Code*/)> OnSteamLinked;
	/**
	 * Gets a copy of each event before it is queued: track events (also error and the
	 * automatic events such as session_start), identify, and traits. Fields: type, event,
	 * properties, traits, user_id, message_id, anonymous_id, session_id, context, ...
	 * Return the event to send (changed or not), or nullptr to drop it. The SDK takes
	 * event, properties, traits, and user_id from the result and keeps the other fields.
	 * When the result changes or removes type, message_id, anonymous_id, session_id, or
	 * context.app, the original event is sent. Errors pass the error limits first.
	 * Events recorded inside the callback are dropped.
	 */
	TFunction<TSharedPtr<FJsonObject>(TSharedPtr<FJsonObject>)> BeforeSend;

	/** Checks the options and starts. Returns false and logs why when an option is wrong. */
	bool Init(const FManalensConfig& InConfig);

	/** Record a named event with scalar properties (strings, numbers, booleans, or null). */
	void Track(const FString& Event, const TSharedPtr<FJsonObject>& Properties = nullptr);
	/** Attach a known player ID (1 to 255 characters) and traits to this install. */
	void Identify(const FString& UserId, const TSharedPtr<FJsonObject>& Traits = nullptr);
	/**
	 * Set traits on this player, with or without a user ID (P-2814). Before Identify()
	 * they belong to this install's anonymous ID; after it, to the user. Scalars only;
	 * null clears a trait. At most 50 keys and 8 KB. Waits for consent: calls before
	 * consent are merged and sent then. Protected traits (email, itch_user_id,
	 * stripe_customer_id, paddle_customer_id) are not sent (one warning).
	 */
	void Traits(const TSharedPtr<FJsonObject>& Traits);
	/**
	 * Verify the Steam account of this player (P-2815). Pass the bytes of a Steam Web API
	 * ticket made with the identity "manalens" (GetAuthTicketForWebApi("manalens"), 16 to
	 * 2048 bytes). The request goes out from Update() after consent, to ApiEndpoint
	 * /v0/link. The answer comes in OnSteamLinked. A refused ticket is not sent again; a
	 * 429 with Retry-After (60 s or less) is tried once more, but not after
	 * SetConsent(false) or Reset() during the wait.
	 */
	void LinkSteamTicket(const TArray<uint8>& Ticket);
	/** LinkSteamTicket() with the ticket as a hex string (32 to 4096 hex characters). */
	void LinkSteamTicketHex(const FString& TicketHex);
	/** The URL of LinkSteamTicket(). */
	const FString& GetLinkUrl() const { return LinkUrlValue; }
	/** true while a link waits for Update() or consent, runs, or waits for its retry. */
	bool IsLinking() const { return bLinkInFlight || !PendingLinkTicket.IsEmpty(); }
	/** Record an error event. Extra holds more scalar properties. */
	void TrackError(const FString& Message, const FManalensErrorOptions& Options = FManalensErrorOptions(), const TSharedPtr<FJsonObject>& Extra = nullptr);
	/** An error from the engine log (UE_LOG with Error verbosity). Counts toward MaxAutoErrorsPerSession. */
	void TrackLogError(const FString& Category, const FString& Message, bool bWarning);
	/** true starts the identity and sends game_open. false deletes the ID, the session, and every queued event. */
	void SetConsent(bool bGranted);
	/** Ends the session, clears the user ID, makes a new anonymous ID, and starts a new session. */
	void Reset();
	/** Sends one batch now, unless a request is in flight or the backoff wait is not over. */
	void Flush();
	/** Call often (each frame). Sends on the flush interval and when a send was requested. */
	void Update();
	/** The game lost or got focus (or went to the background). */
	void SetFocused(bool bFocused);
	/** Quit: ends the session, saves, clears the backoff, and starts a send. Then wait for IsSending() to become false. */
	void BeginShutdown();
	/** A normal exit: marks the saved session as exited (not a crash) and saves the queue. */
	void FinishShutdown();
	/** Adds mana_hid=<anonymous ID>.<unix seconds> to a website URL. Before consent the URL is unchanged. */
	FString LinkUrl(const FString& Url) const;
	/** Change the collector URL. */
	void SetEndpoint(const FString& InEndpoint);

	bool IsInitialized() const { return bInitialized; }
	bool IsIdentityActive() const { return bIdentityActive; }
	bool IsSending() const { return bSending; }
	int32 GetQueueSize() const { return Queue.Num(); }
	double GetBackoffUntil() const { return BackoffUntil; }
	const FString& GetAnonymousId() const { return AnonymousId; }
	const FString& GetSessionId() const { return SessionId; }
	const FString& GetUserId() const { return UserId; }
	const FManalensConfig& GetConfig() const { return Config; }
	/** The JSON of each queued event, oldest first. */
	TArray<FString> GetQueuedEvents() const;

	// Pure helpers (public for the tests) ------------------------------------

	/** A random RFC 4122 version 4 UUID, lower case. */
	static FString NewUuidV4();
	static bool IsUuidV4(const FString& Value);
	/** Why the value cannot be the Publishable key, or an empty string when it may be one. */
	static FString PublishableKeyRefusal(const FString& Key);
	/** The 8-character base62 CRC32 checksum of prefix and body (packages/shared/src/keys.ts). */
	static FString KeyChecksum(const FString& PrefixAndBody);
	static EManalensSendOutcome ClassifyStatus(int32 Status);
	/** Seconds to wait from a Retry-After header (seconds or an HTTP date). 0 when absent or not valid. */
	static double ParseRetryAfter(const FString& Header, double NowUnix);
	/**
	 * The refused events of an accepted batch (P-2787): the "rejected" count, and
	 * the reason codes of "errors" in OutCodes. 0 when the body has no count. The
	 * count is read from the text, so it works on a body cut at 4096 characters.
	 */
	static int32 ParseRejected(const FString& Body, FString& OutCodes);
	/** Groups errors as the Godot and Unity SDKs do: 16 hex characters of SHA-1. */
	static FString Fingerprint(const FString& ErrorType, const FString& Message, const FString& File, const FString& Function);
	/** ISO 8601 UTC with milliseconds and Z, for example 2027-01-15T08:00:00.000Z. */
	static FString FormatTimestamp(double Unix);
	/** A JSON number: integers without a fraction, other values with the shortest exact form. */
	static FString FormatNumber(double Value);
	/** The platform of this build: windows, macos, linux, android, ios, ps5, xbox_series, xbox_one, switch, switch2, or other. */
	static FString BuildPlatform();
	/** The platform values that the collector accepts. */
	static const TArray<FString>& Platforms();
	static const TArray<FString>& Storefronts();
	static const TCHAR* SeverityName(EManalensSeverity Severity);
	/**
	 * A captured log line without personal data: home folders become ~, URLs lose
	 * user:pass@, the query, and the fragment, emails become [email], and values
	 * after key=, token=, secret=, password=, authorization:, or bearer, and runs
	 * of 24 or more key characters, become [redacted].
	 */
	static FString CleanLogMessage(const FString& Message);
	static FString CleanLogMessage(const FString& Message, const TArray<FString>& HomeDirs);
	/**
	 * A crash stack frame without personal data: home folders become ~, URLs lose
	 * user:pass@, the query, and the fragment, and emails become [email]. Long
	 * names stay: a C++ symbol is code, not a token.
	 */
	static FString CleanStackFrame(const FString& Frame, const TArray<FString>& HomeDirs);
	/**
	 * The app_crash properties from a CrashContext.runtime-xml (crash stacks):
	 * stack, error_type, message, fingerprint, crash_source, symbolicated.
	 * nullptr when the file is not a crash of this game (another GameName, an
	 * ensure, or a stall). GameName empty: any game.
	 */
	static TSharedPtr<FJsonObject> ParseCrashContext(const FString& Xml, const FString& GameName, const TArray<FString>& HomeDirs);
	static constexpr int32 MaxCrashStack = 4096;
	static constexpr int64 MaxCrashFileBytes = 256 * 1024;
	/** Log categories that are never captured: LogManalens, LogHttp, LogSlate, LogSteamShared, LogOnline*, and LogEOS*. */
	static bool IsSkippedLogCategory(const FString& Category);
	/**
	 * Log categories of editor modules (Blueprint compiler, shader compiler, DDC, source
	 * control, editor tools). The editor runs them in the same process as Play In Editor,
	 * so the log capture skips them in the editor (P-2792). A packaged game has none of them.
	 */
	static bool IsEditorLogCategory(const FString& Category);

private:
	struct FQueueEntry
	{
		double At = 0.0;
		FString MessageId;
		FString Json;
	};

	bool IdentityAllowed() const;
	void StartIdentity();
	void StartSession(double At);
	void EndSession(double At);
	void Touch();
	/** The crash data of the session that started at StartedAt, or nullptr (crash stacks). */
	TSharedPtr<FJsonObject> CrashData(double StartedAt);
	/** Returns false when BeforeSend dropped the event. */
	bool Enqueue(const FString& Type, const FString& Event, const TSharedPtr<FJsonObject>& Properties, double At = -1.0, const TSharedPtr<FJsonObject>& Traits = nullptr);
	/** Runs BeforeSend on a copy of the envelope. nullptr: drop the event. */
	TSharedPtr<FJsonObject> ApplyBeforeSend(const TSharedRef<FJsonObject>& Envelope);
	void Prune();
	void TrySend();
	/** Asks IsOffline(). On the change back to online, clears the backoff and the failure counter. */
	bool CheckOffline();
	void OnResponse(const FManalensHttpResult& Result, const TSet<FString>& Ids);
	void RecordError(const FString& Message, const FManalensErrorOptions& Options, const FString& Source, const TSharedPtr<FJsonObject>& Extra);
	bool ErrorAllowed(const FString& FingerprintValue, bool bAuto);
	void ClearErrorHistory();
	TSharedRef<FJsonObject> BuildContext() const;
	TSharedRef<FJsonObject> Fit(const TSharedPtr<FJsonObject>& Values, const TCHAR* Field) const;
	TSharedRef<FJsonObject> Scalars(const TSharedPtr<FJsonObject>& Values) const;
	void RaiseSendFailed(int32 Status, int32 Dropped);
	void SendTraits(const TSharedRef<FJsonObject>& Values);
	void UpdateLink();
	void SendLink();
	void OnLinkResponse(const FManalensHttpResult& Result);
	void FinishLink(bool bLinked, int32 Status, const FString& Code);

	FString PathOf(const TCHAR* Name) const;
	void LoadState();
	void SaveState();
	void SaveSession();
	void LoadQueue();
	void SaveQueue();
	void WriteOptOut();
	bool WriteAtomically(const FString& Path, const FString& Text) const;
	void DefaultTransport(const FString& Url, const TArray<TPair<FString, FString>>& Headers, const FString& Body, FManalensSendDone Done);

	FManalensConfig Config;
	FString Url;
	bool bInitialized = false;
	bool bIdentityActive = false;
	/** A server build without bTrackServerBuilds: no automatic client events. */
	bool bServerQuiet = false;
	/** The state file exists but could not be read: record nothing and never overwrite it (until SetConsent(true)). */
	bool bStateUnreadable = false;
	/** SetConsent(false) could not write the opt-out marker: stay off and retry. */
	bool bOptOutPending = false;
	bool bSending = false;
	bool bFlushRequested = false;
	bool bQueueDirty = false;
	bool bFocused = true;
	TSharedPtr<FJsonObject> State;
	TArray<FQueueEntry> Queue;
	FString AnonymousId;
	FString UserId;
	FString SessionId;
	double SessionStartedAt = 0.0;
	double LastActivityAt = 0.0;
	double NextTickAt = 0.0;
	double BackoffUntil = 0.0;
	int32 Failures = 0;
	/** The last IsOffline() result that TrySend() saw. */
	bool bOffline = false;
	/** The batch limit after a 413: half the last batch. Back to no limit after any 2xx. */
	int32 BatchLimit = MAX_int32;
	TOptional<TPair<FString, TSharedPtr<FJsonObject>>> DeferredIdentify;
	/** Traits() before consent, merged. */
	TSharedPtr<FJsonObject> HeldTraits;
	bool bWarnedProtectedTraits = false;
	FString LinkUrlValue;
	/** A Steam ticket (hex) that waits for consent or Update(). */
	FString PendingLinkTicket;
	/** The body of the link request in flight or waiting for its retry, and its anonymous ID. */
	FString LinkBody;
	FString LinkAnonymousId;
	bool bLinkInFlight = false;
	int32 LinkAttempt = 0;
	double LinkRetryAt = -1.0;
	TArray<FString> RecentEvents;
	bool bInBeforeSend = false;
	TMap<FString, double> ErrorSeen;
	TArray<double> ErrorTimes;
	int32 AutoErrors = 0;
	int32 ErrorsDropped = 0;
	/** Set to false on destruction, so a late HTTP callback does nothing. */
	TSharedRef<bool, ESPMode::ThreadSafe> Alive;
};
