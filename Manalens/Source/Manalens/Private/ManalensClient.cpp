// Copyright (c) 2026 Manalens. MIT License.

#include "ManalensClient.h"

#include "CoreGlobals.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformStackWalk.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Internationalization/Regex.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY(LogManalens);

namespace ManalensPrivate
{
	const TCHAR* PublishablePrefix = TEXT("mana_pk_");
	const TCHAR* SecretPrefix = TEXT("mana_sk_");
	const TCHAR* ApiPrefix = TEXT("mana_api_");
	constexpr int32 KeyBodyLength = 43;
	constexpr int32 KeyChecksumLength = 8;
	const TCHAR* Base62 = TEXT("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");

	FString ToJson(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> FromJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Object))
		{
			return nullptr;
		}
		return Object;
	}

	int32 Utf8Length(const FString& Text)
	{
		return FTCHARToUTF8(*Text).Length();
	}

	TSharedRef<FJsonValue> Number(double Value)
	{
		return MakeShared<FJsonValueNumberString>(FManalensClient::FormatNumber(Value));
	}

	/** Letters, digits, spaces, and . _ + - (packages/shared appLabel). */
	bool IsLabel(const FString& Value, int32 MaxLength)
	{
		if (Value.Len() < 1 || Value.Len() > MaxLength)
		{
			return false;
		}
		for (const TCHAR Char : Value)
		{
			const bool bAllowed = (Char >= TEXT('a') && Char <= TEXT('z')) || (Char >= TEXT('A') && Char <= TEXT('Z')) || (Char >= TEXT('0') && Char <= TEXT('9'))
				|| Char == TEXT('.') || Char == TEXT('_') || Char == TEXT('+') || Char == TEXT('-') || Char == TEXT(' ');
			if (!bAllowed)
			{
				return false;
			}
		}
		return true;
	}

	uint32 Crc32(const FString& Text)
	{
		static uint32 Table[256];
		static bool bTable = false;
		if (!bTable)
		{
			for (uint32 N = 0; N < 256; ++N)
			{
				uint32 C = N;
				for (int32 K = 0; K < 8; ++K)
				{
					C = (C & 1) ? 0xedb88320u ^ (C >> 1) : C >> 1;
				}
				Table[N] = C;
			}
			bTable = true;
		}
		uint32 Crc = 0xffffffffu;
		for (const TCHAR Char : Text)
		{
			Crc = Table[(Crc ^ static_cast<uint32>(Char)) & 0xff] ^ (Crc >> 8);
		}
		return Crc ^ 0xffffffffu;
	}

	/** The message with quoted values as ? and numbers as N (as the Godot and Unity SDKs). */
	FString NormalizeMessage(const FString& Message)
	{
		FString Quoted;
		Quoted.Reserve(Message.Len());
		for (int32 Index = 0; Index < Message.Len(); ++Index)
		{
			const TCHAR Char = Message[Index];
			if (Char == TEXT('"') || Char == TEXT('\''))
			{
				int32 End = INDEX_NONE;
				for (int32 Next = Index + 1; Next < Message.Len(); ++Next)
				{
					if (Message[Next] == Char)
					{
						End = Next;
						break;
					}
				}
				if (End != INDEX_NONE)
				{
					Quoted.AppendChar(TEXT('?'));
					Index = End;
					continue;
				}
			}
			Quoted.AppendChar(Char);
		}
		FString Out;
		Out.Reserve(Quoted.Len());
		for (int32 Index = 0; Index < Quoted.Len(); ++Index)
		{
			const TCHAR Char = Quoted[Index];
			if (Char == TEXT('0') && Index + 2 < Quoted.Len() && Quoted[Index + 1] == TEXT('x') && FChar::IsHexDigit(Quoted[Index + 2]))
			{
				Index += 2;
				while (Index + 1 < Quoted.Len() && FChar::IsHexDigit(Quoted[Index + 1]))
				{
					++Index;
				}
				Out.AppendChar(TEXT('N'));
				continue;
			}
			if (Char >= TEXT('0') && Char <= TEXT('9'))
			{
				while (Index + 1 < Quoted.Len() && Quoted[Index + 1] >= TEXT('0') && Quoted[Index + 1] <= TEXT('9'))
				{
					++Index;
				}
				Out.AppendChar(TEXT('N'));
				continue;
			}
			Out.AppendChar(Char);
		}
		return Out;
	}
}

using namespace ManalensPrivate;

FManalensClient::FManalensClient()
	: Alive(MakeShared<bool, ESPMode::ThreadSafe>(true))
{
	Now = []() { return static_cast<double>((FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTicks()) / ETimespan::TicksPerSecond; };
	NewId = []() { return FManalensClient::NewUuidV4(); };
	Random = []() { return FMath::FRand() * 0.999999; };
	MonotonicNow = []() { return FPlatformTime::Seconds(); };
	Transport = [this](const FString& InUrl, const TArray<TPair<FString, FString>>& Headers, const FString& Body, FManalensSendDone Done)
	{
		DefaultTransport(InUrl, Headers, Body, MoveTemp(Done));
	};
}

FManalensClient::~FManalensClient()
{
	*Alive = false;
	Hang.Reset(); // Stops the watchdog thread: it writes to this client's folder.
}

// Setup ------------------------------------------------------------------------

bool FManalensClient::Init(const FManalensConfig& InConfig)
{
	if (bInitialized)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: Init() was already called"));
		return true;
	}
	Config = InConfig;
	if (Config.Endpoint.IsEmpty())
	{
		UE_LOG(LogManalens, Error, TEXT("Manalens: the endpoint is required"));
		return false;
	}
	if (Config.Endpoint.Equals(TEXT("https:"), ESearchCase::IgnoreCase) || Config.Endpoint.Equals(TEXT("http:"), ESearchCase::IgnoreCase))
	{
		// Unreal reads an unquoted // in a .ini value as the start of a comment, so
		// Endpoint=https://events.manalens.dev arrives as "https:".
		UE_LOG(LogManalens, Error, TEXT("Manalens: the endpoint is \"%s\": Unreal reads // in a .ini value as a comment. Put the URL in quotes: Endpoint=\"https://events.manalens.dev\""), *Config.Endpoint);
		return false;
	}
	if (!Config.Endpoint.StartsWith(TEXT("https://")) && !Config.Endpoint.StartsWith(TEXT("http://")))
	{
		UE_LOG(LogManalens, Error, TEXT("Manalens: the endpoint must start with https://"));
		return false;
	}
	// Only the publishable key (mana_pk_) may ship in a game build: never a secret key or an API key.
	const FString Refusal = PublishableKeyRefusal(Config.PublishableKey);
	if (!Refusal.IsEmpty())
	{
		UE_LOG(LogManalens, Error, TEXT("Manalens: Publishable key refused: %s"), *Refusal);
		return false;
	}
	Config.FlushAt = FMath::Clamp(Config.FlushAt, 1, MaxBatch);
	Config.MaxQueue = FMath::Max(Config.MaxQueue, Config.FlushAt);
	Config.FlushIntervalSeconds = FMath::Max(Config.FlushIntervalSeconds, 1.0);
	if (!Config.AppVersion.IsEmpty() && !IsLabel(Config.AppVersion, 32))
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: the app version must be 1 to 32 letters, digits, spaces, or . _ + -; ignored"));
		Config.AppVersion.Reset();
	}
	if (!Config.Build.IsEmpty() && !IsLabel(Config.Build, 64))
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: the build must be 1 to 64 letters, digits, spaces, or . _ + -; ignored"));
		Config.Build.Reset();
	}
	if (!Config.Storefront.IsEmpty() && !Storefronts().Contains(Config.Storefront))
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: the storefront must be one of %s; ignored"), *FString::Join(Storefronts(), TEXT(", ")));
		Config.Storefront.Reset();
	}
	if (!Config.Platform.IsEmpty() && !Platforms().Contains(Config.Platform))
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: the platform must be one of %s; the platform of this build is used"), *FString::Join(Platforms(), TEXT(", ")));
		Config.Platform.Reset();
	}
	if (Config.Platform.IsEmpty())
	{
		Config.Platform = BuildPlatform();
	}
	if (Config.Locale.IsEmpty())
	{
		Config.Locale = FInternationalization::Get().GetCurrentCulture()->GetName();
	}
	Config.Locale = Config.Locale.Replace(TEXT("_"), TEXT("-")).Left(32);
	if (Config.UtcOffsetMinutes == MAX_int32)
	{
		Config.UtcOffsetMinutes = FMath::RoundToInt((FDateTime::Now() - FDateTime::UtcNow()).GetTotalMinutes());
	}
	if (Config.StorageDir.IsEmpty())
	{
		Config.StorageDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Manalens"));
	}
	IFileManager::Get().MakeDirectory(*Config.StorageDir, true);
	SetEndpoint(Config.Endpoint);
	bInitialized = true;
	bServerQuiet = Config.bServerBuild && !Config.bTrackServerBuilds;
	if (bServerQuiet)
	{
		UE_LOG(LogManalens, Log, TEXT("Manalens: dedicated server; no game_open, session, or game_crash events. Turn on Track Server Builds to send them"));
	}
	LoadState();
	NextTickAt = Now() + Config.FlushIntervalSeconds;
	if (IdentityAllowed())
	{
		StartIdentity();
	}
	else if (Config.bDebug)
	{
		UE_LOG(LogManalens, Log, TEXT("Manalens: waiting for consent; no events and no ID"));
	}
	StartHangWatchdog();
	return true;
}

void FManalensClient::SetEndpoint(const FString& InEndpoint)
{
	FString Base = InEndpoint;
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}
	Config.Endpoint = Base;
	Url = Base + TEXT("/v0/events");
	// The API host of LinkSteamTicket(): ApiEndpoint, or the endpoint with an events. host changed to api.
	FString Api = Config.ApiEndpoint.TrimStartAndEnd();
	if (Api.Equals(TEXT("https:"), ESearchCase::IgnoreCase) || Api.Equals(TEXT("http:"), ESearchCase::IgnoreCase))
	{
		// Cut at an unquoted // in the .ini (see Init()): use the default API host.
		UE_LOG(LogManalens, Warning, TEXT("Manalens: the Api Endpoint is \"%s\": Unreal reads // in a .ini value as a comment. Put the URL in quotes. Using the default API host"), *Api);
		Api.Reset();
	}
	if (Api.IsEmpty())
	{
		Api = Base;
		const int32 Scheme = Api.Find(TEXT("://"));
		if (Scheme != INDEX_NONE && Api.Mid(Scheme + 3).StartsWith(TEXT("events."), ESearchCase::IgnoreCase))
		{
			Api = Api.Left(Scheme + 3) + TEXT("api.") + Api.Mid(Scheme + 3 + 7);
		}
	}
	while (Api.EndsWith(TEXT("/")))
	{
		Api.LeftChopInline(1);
	}
	LinkUrlValue = Api + TEXT("/v0/link");
	BackoffUntil = 0.0;
	Failures = 0;
}

// Public API -------------------------------------------------------------------

void FManalensClient::Track(const FString& Event, const TSharedPtr<FJsonObject>& Properties)
{
	if (!bInitialized || !bIdentityActive)
	{
		return; // No events at all before consent.
	}
	if (Event.Len() < 1 || Event.Len() > 128)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: the event name must be 1 to 128 characters"));
		return;
	}
	Touch();
	Enqueue(TEXT("track"), Event, Scalars(Properties));
}

void FManalensClient::Identify(const FString& InUserId, const TSharedPtr<FJsonObject>& Traits)
{
	if (!bInitialized)
	{
		return;
	}
	if (InUserId.Len() < 1 || InUserId.Len() > 255)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: the user ID must be 1 to 255 characters"));
		return;
	}
	if (!bIdentityActive)
	{
		// Sent after consent.
		DeferredIdentify = TPair<FString, TSharedPtr<FJsonObject>>(InUserId, Traits);
		return;
	}
	UserId = InUserId;
	State->SetStringField(TEXT("user_id"), UserId);
	SaveState();
	Touch();
	Enqueue(TEXT("identify"), FString(), nullptr, -1.0, Fit(Scalars(Traits), TEXT("traits")));
	bFlushRequested = true;
}

const TArray<FString>& FManalensClient::ProtectedTraits()
{
	static const TArray<FString> Keys = { TEXT("email"), TEXT("itch_user_id"), TEXT("stripe_customer_id"), TEXT("paddle_customer_id") };
	return Keys;
}

void FManalensClient::Traits(const TSharedPtr<FJsonObject>& InTraits)
{
	if (!bInitialized)
	{
		return;
	}
	TSharedPtr<FJsonObject> Kept = MakeShared<FJsonObject>();
	TArray<FString> Dropped;
	if (InTraits.IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : InTraits->Values)
		{
			if (ProtectedTraits().Contains(Pair.Key))
			{
				Dropped.Add(Pair.Key);
			}
			else
			{
				Kept->SetField(Pair.Key, Pair.Value);
			}
		}
	}
	if (Dropped.Num() > 0 && !bWarnedProtectedTraits)
	{
		bWarnedProtectedTraits = true;
		UE_LOG(LogManalens, Warning, TEXT("Manalens: Traits() does not send %s. Only a server with the secret key can set protected traits"), *FString::Join(Dropped, TEXT(", ")));
	}
	TSharedRef<FJsonObject> Values = Fit(Scalars(Kept), TEXT("traits"));
	if (Values->Values.Num() == 0)
	{
		if (Dropped.Num() == 0)
		{
			UE_LOG(LogManalens, Warning, TEXT("Manalens: Traits() needs at least one trait"));
		}
		return;
	}
	if (!bIdentityActive)
	{
		// Sent after consent.
		if (!HeldTraits.IsValid())
		{
			HeldTraits = MakeShared<FJsonObject>();
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Values->Values)
		{
			HeldTraits->SetField(Pair.Key, Pair.Value);
		}
		return;
	}
	SendTraits(Values);
}

void FManalensClient::SendTraits(const TSharedRef<FJsonObject>& Values)
{
	Touch();
	Enqueue(TEXT("traits"), FString(), nullptr, -1.0, Values);
}

void FManalensClient::LinkSteamTicket(const TArray<uint8>& Ticket)
{
	if (!bInitialized)
	{
		return;
	}
	if (Ticket.Num() < 16 || Ticket.Num() > 2048)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: LinkSteamTicket() needs a ticket of 16 to 2048 bytes"));
		return;
	}
	if (bLinkInFlight)
	{
		return;
	}
	bSteamLinkDone = true; // A call from the game replaces bSteamLink in this launch.
	PendingLinkTicket = BytesToHex(Ticket.GetData(), Ticket.Num()).ToLower();
}

void FManalensClient::LinkSteamTicketHex(const FString& TicketHex)
{
	const FString Hex = TicketHex.TrimStartAndEnd();
	bool bHex = Hex.Len() >= 32 && Hex.Len() <= 4096 && Hex.Len() % 2 == 0;
	for (int32 Index = 0; bHex && Index < Hex.Len(); ++Index)
	{
		bHex = FChar::IsHexDigit(Hex[Index]);
	}
	if (!bHex)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: LinkSteamTicketHex() needs 32 to 4096 hex characters, an even number"));
		return;
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Hex.Len() / 2);
	HexToBytes(Hex, Bytes.GetData());
	LinkSteamTicket(Bytes);
}

void FManalensClient::UpdateLink()
{
	if (LinkRetryAt >= 0.0 && Now() >= LinkRetryAt)
	{
		LinkRetryAt = -1.0;
		// The same gates as the first send: after SetConsent(false), an opt-out, or
		// Reset(), the old ticket and ID are not sent again.
		if (!bIdentityActive)
		{
			FinishLink(false, 0, TEXT("CONSENT_REQUIRED"));
			return;
		}
		if (AnonymousId != LinkAnonymousId)
		{
			FinishLink(false, 0, TEXT("IDENTITY_CHANGED"));
			return;
		}
		SendLink();
		return;
	}
	if (PendingLinkTicket.IsEmpty() || bLinkInFlight || !bIdentityActive || AnonymousId.IsEmpty())
	{
		return;
	}
	LinkBody = FString::Printf(TEXT("{\"provider\":\"steam\",\"ticket\":\"%s\",\"anonymous_id\":\"%s\"}"), *PendingLinkTicket, *AnonymousId);
	PendingLinkTicket.Reset();
	LinkAnonymousId = AnonymousId;
	LinkAttempt = 0;
	bLinkInFlight = true;
	SendLink();
}

void FManalensClient::SendLink()
{
	TArray<TPair<FString, FString>> Headers;
	Headers.Emplace(TEXT("Content-Type"), TEXT("application/json"));
	Headers.Emplace(TEXT("x-publishable-key"), Config.PublishableKey);
	TWeakPtr<bool, ESPMode::ThreadSafe> WeakAlive = Alive;
	TSharedRef<bool> bDone = MakeShared<bool>(false);
	Transport(LinkUrlValue, Headers, LinkBody, [this, WeakAlive, bDone](const FManalensHttpResult& Result)
	{
		const TSharedPtr<bool, ESPMode::ThreadSafe> Pinned = WeakAlive.Pin();
		if (!Pinned.IsValid() || !*Pinned || *bDone)
		{
			return;
		}
		*bDone = true;
		OnLinkResponse(Result);
	});
}

void FManalensClient::OnLinkResponse(const FManalensHttpResult& Result)
{
	if (Result.Status >= 200 && Result.Status < 300)
	{
		if (Config.bDebug)
		{
			UE_LOG(LogManalens, Log, TEXT("Manalens: Steam account linked"));
		}
		FinishLink(true, Result.Status, FString());
		return;
	}
	if (Result.Status <= 0)
	{
		FinishLink(false, 0, TEXT("NETWORK_ERROR"));
		return;
	}
	const double Wait = Result.RetryAfter.IsEmpty() ? -1.0 : ParseRetryAfter(Result.RetryAfter, Now());
	if (Result.Status == 429 && LinkAttempt == 0 && Wait >= 0.0 && Wait <= LinkMaxRetryAfterSeconds)
	{
		LinkAttempt = 1;
		LinkRetryAt = Now() + Wait;
		return;
	}
	FString Code;
	TSharedPtr<FJsonObject> Parsed;
	const TSharedPtr<FJsonObject>* Error = nullptr;
	if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Result.Body), Parsed) && Parsed.IsValid() && Parsed->TryGetObjectField(TEXT("error"), Error))
	{
		(*Error)->TryGetStringField(TEXT("code"), Code);
	}
	UE_LOG(LogManalens, Warning, TEXT("Manalens: LinkSteamTicket() failed (HTTP %d%s%s)"), Result.Status, Code.IsEmpty() ? TEXT("") : TEXT(" "), *Code);
	FinishLink(false, Result.Status, Code);
}

void FManalensClient::FinishLink(bool bLinked, int32 Status, const FString& Code)
{
	bLinkInFlight = false;
	LinkBody.Reset();
	if (OnSteamLinked)
	{
		OnSteamLinked(bLinked, Status, Code);
	}
}

void FManalensClient::TrackError(const FString& Message, const FManalensErrorOptions& Options, const TSharedPtr<FJsonObject>& Extra)
{
	RecordError(Message, Options, SourceManual, Extra);
}

void FManalensClient::TrackLogError(const FString& Category, const FString& Message, bool bWarning)
{
	if (IsSkippedLogCategory(Category))
	{
		return;
	}
	FManalensErrorOptions Options;
	Options.Severity = bWarning ? EManalensSeverity::Warning : EManalensSeverity::Error;
	Options.ErrorType = Category;
	Options.bHandled = false;
	TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	Extra->SetStringField(TEXT("log_category"), Category);
	// The engine log level of the line (ELogVerbosity): only Error and Warning lines are captured.
	Extra->SetStringField(TEXT("log_type"), bWarning ? TEXT("warning") : TEXT("error"));
	// A log line can hold paths, URLs, emails, and tokens: clean it before it is
	// queued. The fingerprint is computed from the cleaned text, so the same error
	// on two machines with different user names groups together.
	RecordError(CleanLogMessage(Message), Options, SourceAuto, Extra);
}

bool FManalensClient::IsSkippedLogCategory(const FString& Category)
{
	// Our own lines (a loop), failed sends, online and platform services (account
	// IDs and tokens), and Slate (UI text).
	return Category == TEXT("LogManalens") || Category == TEXT("LogHttp") || Category == TEXT("LogSlate") || Category == TEXT("LogSteamShared")
		|| Category.StartsWith(TEXT("LogOnline"), ESearchCase::CaseSensitive) || Category.StartsWith(TEXT("LogEOS"), ESearchCase::CaseSensitive);
}

bool FManalensClient::IsEditorLogCategory(const FString& Category)
{
	// Exact names: LogBlueprint is the compiler; LogBlueprintUserMessages (Print String) and
	// LogScript (Accessed None) are the game's own Blueprint messages and stay captured.
	static const TCHAR* Names[] = {
		TEXT("LogBlueprint"), TEXT("LogK2Compiler"), TEXT("LogKismetCompiler"), TEXT("LogUnrealEd"),
		TEXT("LogAssetTools"), TEXT("LogContentBrowser"), TEXT("LogDerivedDataCache"), TEXT("LogSourceControl"),
		TEXT("LogShaderCompilers"), TEXT("LogPython"), TEXT("LogLiveCoding"), TEXT("LogDirectoryWatcher"),
		TEXT("LogSavePackage"), TEXT("LogMaterialEditor"), TEXT("LogPlayLevel"),
	};
	for (const TCHAR* Name : Names)
	{
		if (Category == Name)
		{
			return true;
		}
	}
	return Category.StartsWith(TEXT("LogEditor"), ESearchCase::CaseSensitive)
		|| Category.StartsWith(TEXT("LogCook"), ESearchCase::CaseSensitive)
		|| Category.StartsWith(TEXT("LogAutomation"), ESearchCase::CaseSensitive);
}

namespace ManalensClean
{
	bool IsAsciiAlnum(TCHAR C) { return (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('0') && C <= TEXT('9')); }
	bool IsSeparator(TCHAR C) { return C == TEXT('/') || C == TEXT('\\'); }
	bool IsTokenChar(TCHAR C) { return IsAsciiAlnum(C) || C == TEXT('_') || C == TEXT('-'); }
	bool IsEnd(TCHAR C) { return FChar::IsWhitespace(C) || C == TEXT('"') || C == TEXT('\'') || C == TEXT('<') || C == TEXT('>') || C == TEXT('(') || C == TEXT(')'); }
	bool IsEmailLocal(TCHAR C) { return IsAsciiAlnum(C) || C == TEXT('.') || C == TEXT('_') || C == TEXT('%') || C == TEXT('+') || C == TEXT('-'); }
	bool IsEmailDomain(TCHAR C) { return IsAsciiAlnum(C) || C == TEXT('.') || C == TEXT('-'); }

	/** Replaces each occurrence of a directory (and so the path under it) with ~. */
	void ReplaceDir(FString& Text, FString Dir)
	{
		Dir.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Dir.Len() > 0 && IsSeparator(Dir[Dir.Len() - 1]))
		{
			Dir.LeftChopInline(1);
		}
		if (Dir.Len() < 3)
		{
			return;
		}
		const FString Back = Dir.Replace(TEXT("/"), TEXT("\\"));
		for (const FString& Form : { Dir, Back })
		{
			int32 From = 0;
			int32 At = INDEX_NONE;
			while ((At = Text.Find(Form, ESearchCase::IgnoreCase, ESearchDir::FromStart, From)) != INDEX_NONE)
			{
				const int32 End = At + Form.Len();
				if (End < Text.Len() && !IsSeparator(Text[End]) && !IsEnd(Text[End]))
				{
					From = End; // A longer name, for example /Users/annabel for /Users/anna.
					continue;
				}
				Text = Text.Left(At) + TEXT("~") + Text.Mid(End);
				From = At + 1;
			}
		}
	}

	/** /Users/<name>, /home/<name>, and X:\Users\<name> become ~. */
	void ReplaceHomePatterns(FString& Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		int32 Index = 0;
		while (Index < Text.Len())
		{
			int32 NameAt = INDEX_NONE;
			const FString Rest = Text.Mid(Index, 9);
			const bool bBoundary = Index == 0 || !(IsAsciiAlnum(Text[Index - 1]) || Text[Index - 1] == TEXT('_'));
			if (Rest.StartsWith(TEXT("/Users/"), ESearchCase::IgnoreCase))
			{
				NameAt = Index + 7;
			}
			else if (Rest.StartsWith(TEXT("/home/"), ESearchCase::IgnoreCase))
			{
				NameAt = Index + 6;
			}
			else if (bBoundary && Rest.Len() == 9 && FChar::IsAlpha(Rest[0]) && Rest[1] == TEXT(':') && IsSeparator(Rest[2])
				&& Rest.Mid(3, 5).Equals(TEXT("Users"), ESearchCase::IgnoreCase) && IsSeparator(Rest[8]))
			{
				NameAt = Index + 9;
			}
			if (NameAt != INDEX_NONE)
			{
				int32 End = NameAt;
				while (End < Text.Len() && !IsSeparator(Text[End]) && !IsEnd(Text[End]))
				{
					++End;
				}
				if (End > NameAt)
				{
					Out += TEXT("~");
					Index = End;
					continue;
				}
			}
			Out.AppendChar(Text[Index]);
			++Index;
		}
		Text = MoveTemp(Out);
	}

	/** scheme://user:pass@host/path?query#fragment becomes scheme://host/path. */
	void CleanUrls(FString& Text)
	{
		int32 From = 0;
		int32 At = INDEX_NONE;
		while ((At = Text.Find(TEXT("://"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From)) != INDEX_NONE)
		{
			int32 End = At + 3;
			while (End < Text.Len() && !IsEnd(Text[End]))
			{
				++End;
			}
			FString Rest = Text.Mid(At + 3, End - At - 3);
			for (int32 Index = 0; Index < Rest.Len(); ++Index)
			{
				if (Rest[Index] == TEXT('?') || Rest[Index] == TEXT('#'))
				{
					Rest.LeftInline(Index);
					break;
				}
			}
			int32 Slash = INDEX_NONE;
			Rest.FindChar(TEXT('/'), Slash);
			FString Authority = Slash == INDEX_NONE ? Rest : Rest.Left(Slash);
			const FString Path = Slash == INDEX_NONE ? FString() : Rest.Mid(Slash);
			int32 AtSign = INDEX_NONE;
			if (Authority.FindLastChar(TEXT('@'), AtSign))
			{
				Authority.RightChopInline(AtSign + 1);
			}
			const FString Clean = Authority + Path;
			Text = Text.Left(At + 3) + Clean + Text.Mid(End);
			From = At + 3 + Clean.Len();
		}
	}

	void MaskEmails(FString& Text)
	{
		for (int32 At = 0; At < Text.Len(); ++At)
		{
			if (Text[At] != TEXT('@'))
			{
				continue;
			}
			int32 Start = At;
			while (Start > 0 && IsEmailLocal(Text[Start - 1]))
			{
				--Start;
			}
			int32 End = At + 1;
			while (End < Text.Len() && IsEmailDomain(Text[End]))
			{
				++End;
			}
			while (End > At + 1 && (Text[End - 1] == TEXT('.') || Text[End - 1] == TEXT('-')))
			{
				--End; // A full stop after the address.
			}
			const FString Domain = Text.Mid(At + 1, End - At - 1);
			if (Start < At && Domain.Contains(TEXT(".")))
			{
				Text = Text.Left(Start) + TEXT("[email]") + Text.Mid(End);
				At = Start + 6;
			}
		}
	}

	/**
	 * The value after key, token, secret, password, or authorization with = or :
	 * (also the JSON form "token": "...") and after bearer. A quoted value is
	 * redacted up to its closing quote.
	 */
	void MaskKeyValues(FString& Text)
	{
		// bearer first, so "token: Bearer abc" loses both words.
		const TCHAR* Names[] = { TEXT("bearer"), TEXT("authorization"), TEXT("key"), TEXT("token"), TEXT("secret"), TEXT("password") };
		const FString Redacted = TEXT("[redacted]");
		for (const TCHAR* Name : Names)
		{
			const bool bBearer = FCString::Strcmp(Name, TEXT("bearer")) == 0;
			const bool bAuthorization = FCString::Strcmp(Name, TEXT("authorization")) == 0;
			int32 From = 0;
			int32 At = INDEX_NONE;
			while ((At = Text.Find(Name, ESearchCase::IgnoreCase, ESearchDir::FromStart, From)) != INDEX_NONE)
			{
				int32 Start = At + FCString::Strlen(Name);
				if (bBearer)
				{
					if (Start >= Text.Len() || Text[Start] != TEXT(' '))
					{
						From = At + 1;
						continue;
					}
				}
				else
				{
					if (Start < Text.Len() && (Text[Start] == TEXT('"') || Text[Start] == TEXT('\'')))
					{
						++Start; // The closing quote of a JSON key.
					}
					while (Start < Text.Len() && Text[Start] == TEXT(' '))
					{
						++Start;
					}
					if (Start >= Text.Len() || (Text[Start] != TEXT(':') && Text[Start] != TEXT('=')))
					{
						From = At + 1;
						continue;
					}
					++Start;
				}
				while (Start < Text.Len() && Text[Start] == TEXT(' '))
				{
					++Start;
				}
				int32 End = Start;
				if (Start < Text.Len() && (Text[Start] == TEXT('"') || Text[Start] == TEXT('\'')))
				{
					// A quoted value: up to the closing quote (or the end of the text).
					const TCHAR Quote = Text[Start];
					++Start;
					End = Start;
					while (End < Text.Len() && Text[End] != Quote)
					{
						++End;
					}
				}
				else if (bAuthorization)
				{
					// Authorization: <scheme> <credentials>, to the end of the line.
					while (End < Text.Len() && Text[End] != TEXT('\n') && Text[End] != TEXT('\r') && Text[End] != TEXT('"') && Text[End] != TEXT('\''))
					{
						++End;
					}
				}
				else
				{
					while (End < Text.Len() && !IsEnd(Text[End]) && Text[End] != TEXT('&') && Text[End] != TEXT(',') && Text[End] != TEXT(';') && Text[End] != TEXT(']') && Text[End] != TEXT('}'))
					{
						++End;
					}
				}
				if (End > Start && Text.Mid(Start, End - Start) != Redacted)
				{
					Text = Text.Left(Start) + Redacted + Text.Mid(End);
					End = Start + Redacted.Len();
				}
				From = FMath::Max(End, At + 1);
			}
		}
	}

	bool IsBase64Char(TCHAR C) { return IsTokenChar(C) || C == TEXT('+') || C == TEXT('/') || C == TEXT('='); }

	/**
	 * Keys, tokens, and IDs: a JWT (eyJ... with two dots), a base64 run of 24 or
	 * more characters with + or a trailing =, and runs of 24 or more characters
	 * from A-Z, a-z, 0-9, _, and -. File paths (with / but no + or =) stay.
	 */
	void MaskLongTokens(FString& Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		int32 Index = 0;
		while (Index < Text.Len())
		{
			int32 End = Index;
			while (End < Text.Len() && IsTokenChar(Text[End]))
			{
				++End;
			}
			int32 RedactTo = INDEX_NONE;
			if (Index + 2 < Text.Len() && Text[Index] == TEXT('e') && Text[Index + 1] == TEXT('y') && Text[Index + 2] == TEXT('J'))
			{
				int32 JwtEnd = Index;
				int32 Dots = 0;
				while (JwtEnd < Text.Len() && (IsTokenChar(Text[JwtEnd]) || Text[JwtEnd] == TEXT('.')))
				{
					Dots += Text[JwtEnd] == TEXT('.') ? 1 : 0;
					++JwtEnd;
				}
				while (JwtEnd > Index && Text[JwtEnd - 1] == TEXT('.'))
				{
					--JwtEnd; // A full stop after the token.
					--Dots;
				}
				if (Dots >= 2)
				{
					RedactTo = JwtEnd;
				}
			}
			if (RedactTo == INDEX_NONE)
			{
				int32 B64End = Index;
				bool bPlus = false;
				while (B64End < Text.Len() && IsBase64Char(Text[B64End]))
				{
					bPlus |= Text[B64End] == TEXT('+');
					++B64End;
				}
				if (B64End - Index >= 24 && (bPlus || Text[B64End - 1] == TEXT('=')))
				{
					RedactTo = B64End;
				}
			}
			if (RedactTo == INDEX_NONE && End - Index >= 24)
			{
				RedactTo = End;
			}
			if (RedactTo != INDEX_NONE)
			{
				Out += TEXT("[redacted]");
				Index = RedactTo;
			}
			else if (End > Index)
			{
				Out += Text.Mid(Index, End - Index);
				Index = End;
			}
			else
			{
				Out.AppendChar(Text[Index]);
				++Index;
			}
		}
		Text = MoveTemp(Out);
	}
}

FString FManalensClient::CleanLogMessage(const FString& Message, const TArray<FString>& HomeDirs)
{
	FString Text = Message;
	for (const FString& Dir : HomeDirs)
	{
		ManalensClean::ReplaceDir(Text, Dir);
	}
	ManalensClean::ReplaceHomePatterns(Text);
	ManalensClean::CleanUrls(Text);
	ManalensClean::MaskEmails(Text);
	ManalensClean::MaskKeyValues(Text);
	ManalensClean::MaskLongTokens(Text);
	return Text;
}

FString FManalensClient::CleanStackFrame(const FString& Frame, const TArray<FString>& HomeDirs)
{
	FString Text = Frame;
	for (const FString& Dir : HomeDirs)
	{
		ManalensClean::ReplaceDir(Text, Dir);
	}
	ManalensClean::ReplaceHomePatterns(Text);
	ManalensClean::CleanUrls(Text);
	ManalensClean::MaskEmails(Text);
	return Text;
}

FString FManalensClient::CleanLogMessage(const FString& Message)
{
	// The home folder first, so a path under it keeps the rest of its name.
	return CleanLogMessage(Message, { FPlatformProcess::UserHomeDir(), FPlatformProcess::UserDir() });
}

void FManalensClient::SetConsent(bool bGranted)
{
	if (!bInitialized)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: SetConsent() before the SDK started; ignored"));
		return;
	}
	if (bGranted)
	{
		const FString OptOut = PathOf(OptOutFileName);
		if (IFileManager::Get().FileExists(*OptOut) && !IFileManager::Get().Delete(*OptOut, false, true, true))
		{
			UE_LOG(LogManalens, Warning, TEXT("Manalens: could not remove the opt-out marker; tracking stays off"));
			return;
		}
		bOptOutPending = false;
		if (bStateUnreadable)
		{
			// An unreadable state file is replaced by a new one after an explicit opt-in.
			bStateUnreadable = false;
			State = MakeShared<FJsonObject>();
		}
	}
	else
	{
		WriteOptOut();
	}
	State->SetBoolField(TEXT("consent"), bGranted);
	if (bGranted)
	{
		SaveState();
		if (!bIdentityActive)
		{
			StartIdentity();
		}
		return;
	}
	// Consent withdrawn: forget the player ID, the session, and every unsent event.
	bIdentityActive = false;
	DeferredIdentify.Reset();
	HeldTraits.Reset();
	PendingLinkTicket.Reset();
	AnonymousId.Reset();
	UserId.Reset();
	SessionId.Reset();
	for (const TCHAR* Field : { TEXT("anonymous_id"), TEXT("user_id"), TEXT("first_open_at"), TEXT("session") })
	{
		State->RemoveField(Field);
	}
	SaveState();
	Queue.Reset();
	bQueueDirty = false;
	bFlushRequested = false;
	IFileManager::Get().Delete(*PathOf(QueueFileName), false, true, true);
	if (Hang)
	{
		Hang->ClearSaved([this]() { IFileManager::Get().Delete(*PathOf(HangFileName), false, true, true); }, true);
	}
	SessionHangId.Reset();
	ClearErrorHistory();
}

void FManalensClient::ClearErrorHistory()
{
	// Event names and error counts of the old player must never reach a later error.
	RecentEvents.Reset();
	ErrorSeen.Reset();
	ErrorTimes.Reset();
	ErrorsDropped = 0;
}

void FManalensClient::Reset()
{
	HeldTraits.Reset();
	PendingLinkTicket.Reset();
	if (!bIdentityActive)
	{
		UserId.Reset();
		DeferredIdentify.Reset();
		ClearErrorHistory();
		return;
	}
	const double At = Now();
	EndSession(At); // Sends errors_dropped of the old session.
	ClearErrorHistory();
	if (Hang)
	{
		// No hang of the new player with the old session or breadcrumbs.
		Hang->ClearSaved([this]() { IFileManager::Get().Delete(*PathOf(HangFileName), false, true, true); }, true);
	}
	UserId.Reset();
	AnonymousId = NewId();
	State->SetStringField(TEXT("user_id"), FString());
	State->SetStringField(TEXT("anonymous_id"), AnonymousId);
	StartSession(At);
	SaveState();
}

void FManalensClient::Flush()
{
	if (!bIdentityActive)
	{
		return;
	}
	bFlushRequested = true;
	TrySend();
}

void FManalensClient::Update()
{
	if (!bInitialized)
	{
		return;
	}
	if (bOptOutPending)
	{
		WriteOptOut();
	}
	if (Hang)
	{
		FManalensHangWatchdog::FHang Ended;
		if (Hang->Beat(bIdentityActive && bFocused, SessionId, Ended))
		{
			RecordRecoveredHang(Ended);
		}
	}
	const double At = Now();
	if (At >= NextTickAt)
	{
		NextTickAt = At + Config.FlushIntervalSeconds;
		if (bIdentityActive)
		{
			// While the game has focus the player is active: keep the session alive.
			// Only an open one: after BeginShutdown() a tick must not start a new session.
			if (bFocused && !SessionId.IsEmpty())
			{
				Touch();
			}
			SaveSession();
			SaveQueue();
			bFlushRequested = true;
		}
	}
	if (bFlushRequested && bIdentityActive)
	{
		TrySend();
	}
	UpdateSteamLink();
	UpdateLink();
}

void FManalensClient::UpdateSteamLink()
{
	// Once per launch, after consent, and SteamLinkDelaySeconds after the first accepted
	// batch: Manalens trusts the link of an install that has its own events before it.
	if (!Config.bSteamLink || bSteamLinkDone || bSteamTicketPending || bServerQuiet || !bIdentityActive || !RequestSteamTicket || IsLinking() || !IsSteamLinkReady())
	{
		return;
	}
	const double At = Now();
	if (At < NextSteamLinkAt)
	{
		return;
	}
	if (SteamLinkTriesLeft <= 0)
	{
		bSteamLinkDone = true;
		return;
	}
	--SteamLinkTriesLeft;
	NextSteamLinkAt = At + SteamLinkRetrySeconds;
	bSteamTicketPending = true;
	TWeakPtr<bool, ESPMode::ThreadSafe> WeakAlive = Alive;
	RequestSteamTicket([this, WeakAlive](const FString& TicketHex)
	{
		const TSharedPtr<bool, ESPMode::ThreadSafe> Pinned = WeakAlive.Pin();
		if (!Pinned.IsValid() || !*Pinned || !bSteamTicketPending)
		{
			return;
		}
		bSteamTicketPending = false;
		if (TicketHex.IsEmpty())
		{
			if (Config.bDebug)
			{
				UE_LOG(LogManalens, Log, TEXT("Manalens: Steam gave no Web API ticket; trying again later"));
			}
			return;
		}
		bSteamLinkDone = true;
		LinkSteamTicketHex(TicketHex);
	});
}

void FManalensClient::SetFocused(bool bInFocused)
{
	// No hang while the game is in the background: the OS may stop its frames.
	if (Hang)
	{
		Hang->SetPaused(FManalensHangWatchdog::EPauseCause::Focus, !bInFocused);
	}
	if (!bIdentityActive)
	{
		return;
	}
	if (!bInFocused)
	{
		bFocused = false;
		LastActivityAt = Now();
		SaveSession();
		bQueueDirty = true;
		SaveQueue();
		Flush();
	}
	else
	{
		bFocused = true;
		// Only an open session: after BeginShutdown() the next event starts a new one.
		if (!SessionId.IsEmpty())
		{
			Touch();
		}
	}
}

void FManalensClient::BeginShutdown()
{
	StopHangWatchdog(); // The game quits: a slow quit is not a hang.
	if (!bIdentityActive)
	{
		return;
	}
	EndSession(Now());
	bQueueDirty = true;
	SaveQueue();
	BackoffUntil = 0.0;
	Flush();
}

bool FManalensClient::FlushBeforeExit(double TimeoutSeconds, const TFunction<void()>& Pump)
{
	if (!bInitialized || !bIdentityActive)
	{
		return Queue.Num() == 0; // No consent or an opt-out: nothing is sent.
	}
	const double Deadline = MonotonicNow() + FMath::Max(0.0, TimeoutSeconds);
	while (MonotonicNow() < Deadline)
	{
		if (!bSending)
		{
			if (Queue.Num() == 0)
			{
				break;
			}
			const int32 Before = Queue.Num();
			Flush();
			if (!bSending && Queue.Num() >= Before)
			{
				break; // A failure (backoff) or no network: keep the events for the next launch.
			}
			if (!bSending)
			{
				continue; // The transport answered at once: send the next batch.
			}
		}
		if (Pump)
		{
			Pump();
		}
	}
	return Queue.Num() == 0;
}

void FManalensClient::FinishShutdown()
{
	if (!bInitialized)
	{
		return;
	}
	// The game quit normally: the next launch must not count a crash.
	if (!SessionId.IsEmpty())
	{
		SaveSession();
	}
	const TSharedPtr<FJsonObject>* Session = nullptr;
	if (State.IsValid() && State->TryGetObjectField(TEXT("session"), Session))
	{
		(*Session)->SetBoolField(TEXT("exited"), true);
		SaveState();
	}
	bQueueDirty = true;
	SaveQueue();
}

FString FManalensClient::LinkUrl(const FString& InUrl) const
{
	if (!bIdentityActive || AnonymousId.IsEmpty())
	{
		return InUrl;
	}
	FString Base = InUrl;
	FString Fragment;
	int32 HashAt = INDEX_NONE;
	if (Base.FindChar(TEXT('#'), HashAt))
	{
		Fragment = Base.Mid(HashAt);
		Base = Base.Left(HashAt);
	}
	FString Separator = Base.Contains(TEXT("?")) ? TEXT("&") : TEXT("?");
	if (Base.EndsWith(TEXT("?")) || Base.EndsWith(TEXT("&")))
	{
		Separator.Reset();
	}
	return FString::Printf(TEXT("%s%s%s=%s.%lld%s"), *Base, *Separator, HandoffParam, *AnonymousId, static_cast<long long>(FMath::FloorToDouble(Now())), *Fragment);
}

TArray<FString> FManalensClient::GetQueuedEvents() const
{
	TArray<FString> Out;
	for (const FQueueEntry& Entry : Queue)
	{
		Out.Add(Entry.Json);
	}
	return Out;
}

// Identity and sessions --------------------------------------------------------

bool FManalensClient::IdentityAllowed() const
{
	if (bStateUnreadable || bOptOutPending || IFileManager::Get().FileExists(*PathOf(OptOutFileName)))
	{
		return false;
	}
	bool bConsent = false;
	if (State.IsValid() && State->TryGetBoolField(TEXT("consent"), bConsent))
	{
		return bConsent;
	}
	return !Config.bConsentRequired;
}

void FManalensClient::StartIdentity()
{
	bIdentityActive = true;
	const double At = Now();
	AnonymousId.Reset();
	State->TryGetStringField(TEXT("anonymous_id"), AnonymousId);
	const bool bFirstOpen = !IsUuidV4(AnonymousId);
	if (bFirstOpen)
	{
		AnonymousId = NewId();
		State->SetStringField(TEXT("anonymous_id"), AnonymousId);
		State->SetStringField(TEXT("first_open_at"), FormatTimestamp(At));
	}
	UserId.Reset();
	State->TryGetStringField(TEXT("user_id"), UserId);
	LoadQueue();
	// A session left open by a crash or a killed process ends at its last activity.
	const TSharedPtr<FJsonObject>* Previous = nullptr;
	FString PreviousId;
	if (State->TryGetObjectField(TEXT("session"), Previous) && (*Previous)->TryGetStringField(TEXT("id"), PreviousId) && !PreviousId.IsEmpty())
	{
		const TSharedPtr<FJsonObject> Session = *Previous;
		SessionId = PreviousId;
		double Started = At;
		Session->TryGetNumberField(TEXT("started_at"), Started);
		double Last = Started;
		Session->TryGetNumberField(TEXT("last_activity_at"), Last);
		SessionStartedAt = Started;
		LastActivityAt = Last;
		bool bExited = false;
		bool bWasFocused = true;
		Session->TryGetBoolField(TEXT("exited"), bExited);
		Session->TryGetBoolField(TEXT("focused"), bWasFocused);
		// A hang that the game never recovered from (it was killed while stuck),
		// else the last hang of the session that ended: game_crash links to it.
		FString HangId = SendSavedHang();
		if (HangId.IsEmpty())
		{
			Session->TryGetStringField(TEXT("hang_message_id"), HangId);
		}
		// No normal exit and the game had focus: count it as a crash. A game the
		// OS closed in the background is not a crash.
		if (!bServerQuiet && !bExited && bWasFocused)
		{
			TSharedPtr<FJsonObject> Crash = MakeShared<FJsonObject>();
			Crash->SetStringField(TEXT("previous_session_id"), PreviousId);
			if (!HangId.IsEmpty())
			{
				Crash->SetStringField(TEXT("hang_message_id"), HangId);
			}
			for (const TCHAR* Key : { TEXT("build"), TEXT("app_version") })
			{
				FString Value;
				if (Session->TryGetStringField(Key, Value) && !Value.IsEmpty())
				{
					Crash->SetStringField(FString(TEXT("previous_")) + Key, Value);
				}
			}
			if (const TSharedPtr<FJsonObject> Data = CrashData(SessionStartedAt))
			{
				for (const auto& Pair : Data->Values)
				{
					Crash->SetField(Pair.Key, Pair.Value);
				}
			}
			Enqueue(TEXT("track"), TEXT("game_crash"), Crash, LastActivityAt);
		}
		EndSession(LastActivityAt);
	}
	else
	{
		IFileManager::Get().Delete(*PathOf(HangFileName), false, true, true); // The hang of a session that is gone.
	}
	StartSession(At);
	TSharedPtr<FJsonObject> Open = MakeShared<FJsonObject>();
	Open->SetBoolField(TEXT("is_first_open"), bFirstOpen);
	FString FirstOpenAt = FormatTimestamp(At);
	State->TryGetStringField(TEXT("first_open_at"), FirstOpenAt);
	Open->SetStringField(TEXT("first_open_at"), FirstOpenAt);
	if (!bServerQuiet)
	{
		Enqueue(TEXT("track"), TEXT("game_open"), Open);
	}
	SaveState();
	// Write game_open (the install signal) and game_crash to disk before
	// any send: a process killed before the next save would lose them.
	SaveQueue();
	if (DeferredIdentify.IsSet())
	{
		const TPair<FString, TSharedPtr<FJsonObject>> Pending = DeferredIdentify.GetValue();
		DeferredIdentify.Reset();
		Identify(Pending.Key, Pending.Value);
	}
	if (HeldTraits.IsValid())
	{
		const TSharedRef<FJsonObject> Held = HeldTraits.ToSharedRef();
		HeldTraits.Reset();
		SendTraits(Held);
	}
	// Sent on the next Update(), so events of the first frame share the request.
	bFlushRequested = true;
}

void FManalensClient::StartSession(double At)
{
	SessionHangId.Reset();
	SessionId = NewId();
	SessionStartedAt = At;
	LastActivityAt = At;
	AutoErrors = 0;
	SaveSession();
	if (!bServerQuiet)
	{
		Enqueue(TEXT("track"), TEXT("session_start"), nullptr, At);
	}
}

void FManalensClient::EndSession(double At)
{
	if (SessionId.IsEmpty())
	{
		return;
	}
	TSharedPtr<FJsonObject> Ended = MakeShared<FJsonObject>();
	Ended->SetField(TEXT("duration_s"), Number(FMath::RoundToDouble(FMath::Max(0.0, At - SessionStartedAt))));
	if (ErrorsDropped > 0)
	{
		Ended->SetField(TEXT("errors_dropped"), Number(ErrorsDropped));
		ErrorsDropped = 0;
	}
	if (!bServerQuiet)
	{
		Enqueue(TEXT("track"), TEXT("session_end"), Ended, At);
	}
	SessionId.Reset();
	State->RemoveField(TEXT("session"));
	SaveState();
}

void FManalensClient::Touch()
{
	const double At = Now();
	if (SessionId.IsEmpty())
	{
		StartSession(At);
	}
	else if (At - LastActivityAt > SessionTimeoutSeconds)
	{
		EndSession(LastActivityAt);
		StartSession(At);
	}
	else if (At - SessionStartedAt >= SessionMaxAgeSeconds)
	{
		if (Config.bDebug)
		{
			UE_LOG(LogManalens, Log, TEXT("Manalens: the session is 24 hours old; a new session starts"));
		}
		EndSession(At);
		StartSession(At);
	}
	LastActivityAt = At;
}

// Events and queue -------------------------------------------------------------

TSharedRef<FJsonObject> FManalensClient::BuildContext() const
{
	TSharedRef<FJsonObject> App = MakeShared<FJsonObject>();
	App->SetStringField(TEXT("platform"), Config.Platform);
	App->SetStringField(TEXT("engine"), TEXT("unreal"));
	if (!Config.AppVersion.IsEmpty())
	{
		App->SetStringField(TEXT("app_version"), Config.AppVersion);
	}
	if (!Config.Build.IsEmpty())
	{
		App->SetStringField(TEXT("build"), Config.Build);
	}
	if (!Config.Storefront.IsEmpty())
	{
		App->SetStringField(TEXT("storefront"), Config.Storefront);
	}
	TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
	Context->SetObjectField(TEXT("app"), App);
	if (!Config.Locale.IsEmpty())
	{
		Context->SetStringField(TEXT("locale"), Config.Locale);
	}
	Context->SetStringField(TEXT("library_name"), LibraryName);
	Context->SetStringField(TEXT("library_version"), LibraryVersion);
	// The clock's offset from UTC in minutes. The collector uses it only to match
	// an install to a website visit, and accepts -720 to 840.
	if (Config.UtcOffsetMinutes >= -720 && Config.UtcOffsetMinutes <= 840)
	{
		Context->SetField(TEXT("utc_offset"), Number(Config.UtcOffsetMinutes));
	}
	return Context;
}

bool FManalensClient::Enqueue(const FString& Type, const FString& InEvent, const TSharedPtr<FJsonObject>& Properties, double At, const TSharedPtr<FJsonObject>& Traits, const FString& InMessageId)
{
	FString Event = InEvent;
	const double When = At < 0.0 ? Now() : At;
	TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	const FString MessageId = InMessageId.IsEmpty() ? NewId() : InMessageId;
	Envelope->SetStringField(TEXT("type"), Type);
	Envelope->SetStringField(TEXT("message_id"), MessageId);
	Envelope->SetStringField(TEXT("channel"), TEXT("app"));
	Envelope->SetStringField(TEXT("version"), LibraryVersion);
	Envelope->SetStringField(TEXT("original_timestamp"), FormatTimestamp(When));
	Envelope->SetObjectField(TEXT("context"), BuildContext());
	Envelope->SetObjectField(TEXT("properties"), Fit(Properties, TEXT("properties")));
	Envelope->SetStringField(TEXT("anonymous_id"), AnonymousId);
	if (!SessionId.IsEmpty())
	{
		Envelope->SetStringField(TEXT("session_id"), SessionId);
	}
	if (!Event.IsEmpty())
	{
		Envelope->SetStringField(TEXT("event"), Event);
	}
	if (!UserId.IsEmpty())
	{
		Envelope->SetStringField(TEXT("user_id"), UserId);
	}
	if (Traits.IsValid())
	{
		Envelope->SetObjectField(TEXT("traits"), Traits);
	}
	if (BeforeSend)
	{
		const TSharedPtr<FJsonObject> Final = ApplyBeforeSend(Envelope);
		if (!Final.IsValid())
		{
			if (Config.bDebug)
			{
				UE_LOG(LogManalens, Log, TEXT("Manalens: BeforeSend dropped %s %s"), *Type, *Event);
			}
			return false;
		}
		Envelope = Final.ToSharedRef();
		Envelope->TryGetStringField(TEXT("event"), Event);
	}
	if (Type == TEXT("track") && Event != TEXT("error"))
	{
		RecentEvents.Add(Event);
		if (RecentEvents.Num() > Breadcrumbs)
		{
			RecentEvents.RemoveAt(0);
		}
		if (Hang)
		{
			Hang->SetBreadcrumbs(FString::Join(RecentEvents, TEXT(",")).Right(MaxString));
		}
	}
	FQueueEntry& Entry = Queue.AddDefaulted_GetRef();
	Entry.At = When;
	Entry.MessageId = MessageId;
	Entry.Json = ToJson(Envelope);
	const int32 Overflow = Queue.Num() - Config.MaxQueue;
	if (Overflow > 0)
	{
		Queue.RemoveAt(0, Overflow);
		RaiseSendFailed(0, Overflow);
	}
	bQueueDirty = true;
	if (Config.bDebug)
	{
		UE_LOG(LogManalens, Log, TEXT("Manalens: queued %s %s"), *Type, *Event);
	}
	if (Queue.Num() >= Config.FlushAt)
	{
		SaveQueue();
		bFlushRequested = true;
	}
	return true;
}

TSharedPtr<FJsonObject> FManalensClient::ApplyBeforeSend(const TSharedRef<FJsonObject>& Envelope)
{
	if (bInBeforeSend)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: an event recorded inside BeforeSend is dropped"));
		return nullptr;
	}
	bInBeforeSend = true;
	const TSharedPtr<FJsonObject> Changed = BeforeSend(FromJson(ToJson(Envelope)));
	bInBeforeSend = false;
	if (!Changed.IsValid())
	{
		return nullptr;
	}
	auto Field = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Name) -> TSharedPtr<FJsonValue>
	{
		const TSharedPtr<FJsonValue>* Value = Object->Values.Find(Name);
		return Value && Value->IsValid() && (*Value)->Type != EJson::Null ? *Value : TSharedPtr<FJsonValue>();
	};
	bool bOk = true;
	for (const TCHAR* Key : { TEXT("type"), TEXT("message_id"), TEXT("anonymous_id"), TEXT("session_id") })
	{
		FString Original;
		const bool bHas = Envelope->TryGetStringField(Key, Original);
		const TSharedPtr<FJsonValue> Value = Field(Changed, Key);
		if (bHas != Value.IsValid() || (bHas && (Value->Type != EJson::String || Value->AsString() != Original)))
		{
			bOk = false;
		}
	}
	const TSharedPtr<FJsonObject> OriginalApp = Envelope->GetObjectField(TEXT("context"))->GetObjectField(TEXT("app"));
	const TSharedPtr<FJsonValue> Context = Field(Changed, TEXT("context"));
	const TSharedPtr<FJsonValue> App = Context.IsValid() && Context->Type == EJson::Object ? Field(Context->AsObject(), TEXT("app")) : TSharedPtr<FJsonValue>();
	if (!App.IsValid() || App->Type != EJson::Object || App->AsObject()->Values.Num() != OriginalApp->Values.Num())
	{
		bOk = false;
	}
	else
	{
		for (const auto& Pair : OriginalApp->Values)
		{
			FString Value;
			if (!App->AsObject()->TryGetStringField(FString(*Pair.Key), Value) || Value != Pair.Value->AsString())
			{
				bOk = false;
			}
		}
	}
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->Values = Envelope->Values;
	if (Envelope->HasField(TEXT("event")))
	{
		const TSharedPtr<FJsonValue> Name = Field(Changed, TEXT("event"));
		if (Name.IsValid() && Name->Type == EJson::String && Name->AsString().Len() >= 1 && Name->AsString().Len() <= 128)
		{
			Out->SetStringField(TEXT("event"), Name->AsString());
		}
		else
		{
			bOk = false;
		}
	}
	for (const TCHAR* Key : { TEXT("properties"), TEXT("traits") })
	{
		if (!Envelope->HasField(Key))
		{
			continue;
		}
		const TSharedPtr<FJsonValue> Value = Field(Changed, Key);
		if (!Value.IsValid())
		{
			Out->SetObjectField(Key, MakeShared<FJsonObject>());
		}
		else if (Value->Type == EJson::Object)
		{
			Out->SetObjectField(Key, Fit(Scalars(Value->AsObject()), Key));
		}
		else
		{
			bOk = false;
		}
	}
	const TSharedPtr<FJsonValue> User = Field(Changed, TEXT("user_id"));
	if (!User.IsValid() || (User->Type == EJson::String && User->AsString().IsEmpty()))
	{
		Out->RemoveField(TEXT("user_id"));
	}
	else if (User->Type == EJson::String && User->AsString().Len() <= 255)
	{
		Out->SetStringField(TEXT("user_id"), User->AsString());
	}
	else
	{
		bOk = false;
	}
	FString Type;
	Envelope->TryGetStringField(TEXT("type"), Type);
	if (Type == TEXT("identify") && !Out->HasField(TEXT("user_id")))
	{
		bOk = false;
	}
	if (!bOk)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: BeforeSend changed or removed type, message_id, anonymous_id, session_id, context.app, or gave a wrong event, properties, traits, or user_id; the original event is sent"));
		return Envelope;
	}
	return Out;
}

void FManalensClient::Prune()
{
	const double Oldest = Now() - MaxEventAgeSeconds;
	const int32 Removed = Queue.RemoveAll([Oldest](const FQueueEntry& Entry) { return Entry.At < Oldest; });
	if (Removed > 0)
	{
		bQueueDirty = true;
		RaiseSendFailed(0, Removed);
	}
}

bool FManalensClient::EngineReportsNoNetwork()
{
	const ENetworkConnectionType Type = FPlatformMisc::GetNetworkConnectionType();
	return Type == ENetworkConnectionType::None || Type == ENetworkConnectionType::AirplaneMode;
}

bool FManalensClient::CheckOffline()
{
	const bool bNow = IsOffline ? IsOffline() : false;
	if (bNow == bOffline)
	{
		return bNow;
	}
	bOffline = bNow;
	if (bNow)
	{
		UE_LOG(LogManalens, Verbose, TEXT("Manalens: no network: sends paused, %d events kept"), Queue.Num());
	}
	else
	{
		// The wait came from the time without a network: the first send after it goes at once.
		BackoffUntil = 0.0;
		Failures = 0;
		UE_LOG(LogManalens, Verbose, TEXT("Manalens: network back: sending now"));
	}
	return bNow;
}

void FManalensClient::TrySend()
{
	if (!bIdentityActive || bSending || Queue.Num() == 0)
	{
		if (Queue.Num() == 0)
		{
			bFlushRequested = false;
		}
		return;
	}
	if (CheckOffline())
	{
		// No network: start no request and keep the events. Not a failure: the
		// failure counter and the backoff do not change. The next tick asks again.
		bFlushRequested = false;
		return;
	}
	if (Now() < BackoffUntil)
	{
		return; // Keep the request: send when the wait is over.
	}
	Prune();
	if (Queue.Num() == 0)
	{
		bFlushRequested = false;
		return;
	}
	const int32 Count = FMath::Min3(Config.FlushAt, BatchLimit, Queue.Num());
	TSet<FString> Ids;
	FString Body = TEXT("{\"events\":[");
	for (int32 Index = 0; Index < Count; ++Index)
	{
		if (Index > 0)
		{
			Body.AppendChar(TEXT(','));
		}
		Body += Queue[Index].Json;
		Ids.Add(Queue[Index].MessageId);
	}
	// sent_at: the device clock now. The collector corrects the event times with it, so a wrong clock gives right times (P-2799).
	Body += FString::Printf(TEXT("],\"sent_at\":\"%s\"}"), *FormatTimestamp(Now()));
	TArray<TPair<FString, FString>> Headers;
	Headers.Emplace(TEXT("Content-Type"), TEXT("application/json"));
	Headers.Emplace(TEXT("x-publishable-key"), Config.PublishableKey);
	bSending = true;
	bFlushRequested = false;
	TWeakPtr<bool, ESPMode::ThreadSafe> WeakAlive = Alive;
	TSharedRef<bool> bDone = MakeShared<bool>(false);
	Transport(Url, Headers, Body, [this, WeakAlive, bDone, Ids](const FManalensHttpResult& Result)
	{
		const TSharedPtr<bool, ESPMode::ThreadSafe> Pinned = WeakAlive.Pin();
		if (!Pinned.IsValid() || !*Pinned || *bDone)
		{
			return; // The client is gone, or the transport called back twice.
		}
		*bDone = true;
		OnResponse(Result, Ids);
	});
}

EManalensSendOutcome FManalensClient::ClassifyStatus(int32 Status)
{
	if (Status >= 200 && Status < 300)
	{
		return EManalensSendOutcome::Sent;
	}
	// 413: the batch is too large. OnResponse() halves it (or drops a single event).
	if (Status == 413)
	{
		return EManalensSendOutcome::Split;
	}
	// A 4xx other than 408, 413, and 429 will fail again: drop the batch (as the
	// Godot, Unity, and web SDKs do). Network errors (0) and 5xx stay and back off.
	if (Status >= 400 && Status < 500 && Status != 408 && Status != 429)
	{
		return EManalensSendOutcome::Drop;
	}
	return EManalensSendOutcome::Retry;
}

double FManalensClient::ParseRetryAfter(const FString& Header, double NowUnix)
{
	const FString Value = Header.TrimStartAndEnd();
	if (Value.IsEmpty())
	{
		return 0.0;
	}
	if (Value.IsNumeric())
	{
		return FMath::Max(0.0, FCString::Atod(*Value));
	}
	FDateTime Date;
	if (FDateTime::ParseHttpDate(Value, Date))
	{
		return FMath::Max(0.0, static_cast<double>(Date.ToUnixTimestamp()) - NowUnix);
	}
	return 0.0;
}

int32 FManalensClient::ParseRejected(const FString& Body, FString& OutCodes)
{
	OutCodes.Reset();
	const int32 Key = Body.Find(TEXT("\"rejected\""), ESearchCase::CaseSensitive);
	if (Key == INDEX_NONE)
	{
		return 0;
	}
	int32 Index = Key + 10;
	while (Index < Body.Len() && (Body[Index] == TEXT(' ') || Body[Index] == TEXT(':')))
	{
		++Index;
	}
	int32 End = Index;
	while (End < Body.Len() && FChar::IsDigit(Body[End]))
	{
		++End;
	}
	if (End == Index || End - Index > 9)
	{
		return 0;
	}
	const int32 Rejected = FCString::Atoi(*Body.Mid(Index, End - Index));
	if (const TSharedPtr<FJsonObject> Parsed = FromJson(Body))
	{
		const TArray<TSharedPtr<FJsonValue>>* Errors = nullptr;
		if (Parsed->TryGetArrayField(TEXT("errors"), Errors))
		{
			TArray<FString> Codes;
			for (const TSharedPtr<FJsonValue>& Item : *Errors)
			{
				const TSharedPtr<FJsonObject>* ItemObject = nullptr;
				FString ItemCode;
				if (Item.IsValid() && Item->TryGetObject(ItemObject) && (*ItemObject)->TryGetStringField(TEXT("code"), ItemCode))
				{
					Codes.AddUnique(ItemCode);
				}
			}
			OutCodes = FString::Join(Codes, TEXT(", "));
		}
	}
	return Rejected;
}

void FManalensClient::OnResponse(const FManalensHttpResult& Result, const TSet<FString>& Ids)
{
	int32 Count = Ids.Num();
	EManalensSendOutcome Outcome = ClassifyStatus(Result.Status);
	if (Outcome == EManalensSendOutcome::Split && Count <= 1)
	{
		Outcome = EManalensSendOutcome::Drop; // A single event that is too large: drop it.
	}
	if (Outcome == EManalensSendOutcome::Sent)
	{
		BatchLimit = MAX_int32;
		if (FirstBatchAcceptedAt < 0.0)
		{
			FirstBatchAcceptedAt = Now();
		}
	}
	if (Outcome == EManalensSendOutcome::Split)
	{
		// Keep the events (same order, same message_id values) and send again at
		// once with half the batch. Not a failure: no backoff, Failures does not change.
		BatchLimit = FMath::Max(1, Count / 2);
		UE_LOG(LogManalens, Warning, TEXT("Manalens: 413: batch of %d events too large, sending %d"), Count, BatchLimit);
	}
	else if (Outcome != EManalensSendOutcome::Retry)
	{
		Queue.RemoveAll([&Ids](const FQueueEntry& Entry) { return Ids.Contains(Entry.MessageId); });
		bQueueDirty = true;
		Failures = 0;
		BackoffUntil = 0.0;
		if (Outcome == EManalensSendOutcome::Drop)
		{
			// Dropped, not sent again: the same batch would be refused again.
			FString Reason;
			if (const TSharedPtr<FJsonObject> Parsed = FromJson(Result.Body))
			{
				const TSharedPtr<FJsonObject>* Error = nullptr;
				if (Parsed->TryGetObjectField(TEXT("error"), Error))
				{
					FString Code;
					FString Text;
					(*Error)->TryGetStringField(TEXT("code"), Code);
					(*Error)->TryGetStringField(TEXT("message"), Text);
					Reason = FString::Printf(TEXT(" %s: %s"), *Code, *Text);
					const TArray<TSharedPtr<FJsonValue>>* Refused = nullptr;
					if ((*Error)->TryGetArrayField(TEXT("refused"), Refused))
					{
						TArray<FString> Codes;
						for (const TSharedPtr<FJsonValue>& Item : *Refused)
						{
							const TSharedPtr<FJsonObject>* ItemObject = nullptr;
							FString ItemCode;
							if (Item.IsValid() && Item->TryGetObject(ItemObject) && (*ItemObject)->TryGetStringField(TEXT("code"), ItemCode))
							{
								Codes.AddUnique(ItemCode);
							}
						}
						if (Codes.Num() > 0)
						{
							Reason += FString::Printf(TEXT(" (%s)"), *FString::Join(Codes, TEXT(", ")));
						}
					}
				}
			}
			UE_LOG(LogManalens, Warning, TEXT("Manalens: the collector refused %d events (HTTP %d)%s; they are dropped"), Count, Result.Status, *Reason.Left(300));
			RaiseSendFailed(Result.Status, Count);
		}
		else
		{
			// An accepted batch can still list refused events (P-2787). They are
			// not sent again: the same events would be refused again.
			FString Codes;
			const int32 Dropped = FMath::Min(Count, ParseRejected(Result.Body, Codes));
			if (Dropped > 0)
			{
				UE_LOG(LogManalens, Warning, TEXT("Manalens: the collector refused %d of %d events (HTTP %d)%s; they are dropped"), Dropped, Count, Result.Status, Codes.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *Codes.Left(300)));
				RaiseSendFailed(Result.Status, Dropped);
				Count -= Dropped;
			}
		}
	}
	else
	{
		++Failures;
		const double Delay = FMath::Min(MaxBackoffSeconds, FMath::Pow(2.0, static_cast<double>(Failures)));
		double Wait = Delay * (0.5 + 0.5 * Random());
		// Retry-After (429 or 503) wins when it asks for a longer wait. At most 1 hour.
		const double RetryAfter = ParseRetryAfter(Result.RetryAfter, Now());
		if (RetryAfter > 0.0)
		{
			Wait = FMath::Max(Wait, FMath::Min(RetryAfter, 3600.0));
		}
		BackoffUntil = Now() + Wait;
		if (Config.bDebug)
		{
			UE_LOG(LogManalens, Log, TEXT("Manalens: send failed (HTTP %d); retry in %.0f s"), Result.Status, Wait);
		}
	}
	bSending = false;
	SaveQueue();
	if (OnFlushed)
	{
		OnFlushed(Outcome == EManalensSendOutcome::Sent ? Count : 0, Outcome == EManalensSendOutcome::Sent);
	}
	if (Outcome != EManalensSendOutcome::Retry && Queue.Num() > 0)
	{
		bFlushRequested = true;
	}
}

void FManalensClient::RaiseSendFailed(int32 Status, int32 Dropped)
{
	if (Status == 0)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: %d events dropped (queue over %d events, or older than 7 days)"), Dropped, Config.MaxQueue);
	}
	if (OnSendFailed)
	{
		OnSendFailed(Status, Dropped);
	}
}

// Errors -----------------------------------------------------------------------

FString FManalensClient::RecordError(const FString& Message, const FManalensErrorOptions& Options, const FString& Source, const TSharedPtr<FJsonObject>& Extra, double At, const FString& MessageId, bool bTouch)
{
	if (!bInitialized || !bIdentityActive)
	{
		return FString();
	}
	const bool bAuto = Source == SourceAuto;
	const FString ErrorType = Options.ErrorType.Left(128);
	const FString File = Options.File.Left(256);
	const FString Function = Options.Function.Left(256);
	// The error's own keys come first, so a trim to the limits drops custom keys first.
	TSharedPtr<FJsonObject> Props = MakeShared<FJsonObject>();
	Props->SetStringField(TEXT("severity"), SeverityName(Options.Severity));
	Props->SetStringField(TEXT("message"), Message.Left(MaxString));
	Props->SetBoolField(TEXT("handled"), Options.bHandled);
	Props->SetStringField(TEXT("source"), Source);
	FString FingerprintValue = Options.Fingerprint.Left(128);
	if (FingerprintValue.IsEmpty())
	{
		FingerprintValue = Fingerprint(ErrorType, Message, File, Function);
	}
	Props->SetStringField(TEXT("fingerprint"), FingerprintValue);
	if (!ErrorType.IsEmpty())
	{
		Props->SetStringField(TEXT("error_type"), ErrorType);
	}
	if (!File.IsEmpty())
	{
		Props->SetStringField(TEXT("file"), File);
		Props->SetField(TEXT("line"), Number(Options.Line));
	}
	if (!Function.IsEmpty())
	{
		Props->SetStringField(TEXT("function"), Function);
	}
	if (!Options.Stack.IsEmpty())
	{
		Props->SetStringField(TEXT("stack"), Options.Stack.Left(MaxString));
	}
	if (RecentEvents.Num() > 0)
	{
		Props->SetStringField(TEXT("breadcrumbs"), FString::Join(RecentEvents, TEXT(",")).Right(MaxString));
	}
	for (const TPair<FString, FString>& Pair : Options.Properties)
	{
		if (!Props->HasField(Pair.Key))
		{
			Props->SetStringField(Pair.Key, Pair.Value);
		}
	}
	const TSharedRef<FJsonObject> ExtraScalars = Scalars(Extra);
	for (const auto& Pair : ExtraScalars->Values)
	{
		const FString Key(*Pair.Key);
		if (!Props->HasField(Key))
		{
			Props->SetField(Key, Pair.Value);
		}
	}
	if (!ErrorAllowed(FingerprintValue, bAuto))
	{
		++ErrorsDropped;
		return FString();
	}
	if (bTouch)
	{
		Touch();
	}
	// BeforeSend runs in Enqueue, after the fingerprint and the limits.
	const FString Id = MessageId.IsEmpty() ? NewId() : MessageId;
	if (!Enqueue(TEXT("track"), TEXT("error"), Props, At, nullptr, Id))
	{
		return FString();
	}
	if (bAuto)
	{
		++AutoErrors;
	}
	// Keep the error on disk now: a crash can follow it.
	SaveQueue();
	return Id;
}

// Hangs (P-2833) ---------------------------------------------------------------

void FManalensClient::StartHangWatchdog()
{
	if (Config.HangThresholdSeconds <= 0.0 || bServerQuiet || Hang)
	{
		return;
	}
	Hang = MakeUnique<FManalensHangWatchdog>(Config.HangThresholdSeconds, MonotonicNow, Now, [this](const FManalensHangWatchdog::FHang& Stuck) { SaveHang(Stuck); });
	Hang->CaptureStackAddresses = CaptureHangStackAddresses;
	Hang->SymbolizeStack = SymbolizeHangStack;
	Hang->IsDebuggerPaused = IsHangDebuggerPaused;
	if (Config.bHangWatchdogThread)
	{
		Hang->StartThread();
	}
}

void FManalensClient::CheckHang()
{
	if (Hang)
	{
		Hang->Check();
	}
}

void FManalensClient::SetHangPaused(bool bPaused)
{
	if (Hang)
	{
		Hang->SetPaused(FManalensHangWatchdog::EPauseCause::MapLoad, bPaused);
	}
}

void FManalensClient::StopHangWatchdog()
{
	if (Hang)
	{
		Hang->StopThread();
	}
}

FString FManalensClient::HangMessage(double Seconds)
{
	return FString::Printf(TEXT("The main thread was blocked for %.1f s"), Seconds);
}

void FManalensClient::SaveHang(const FManalensHangWatchdog::FHang& Stuck) const
{
	// The watchdog thread, while the game thread is stuck: write the hang at
	// once, because the player may kill the game before it recovers.
	TSharedRef<FJsonObject> File = MakeShared<FJsonObject>();
	File->SetStringField(TEXT("message_id"), Stuck.MessageId);
	File->SetStringField(TEXT("session_id"), Stuck.SessionId);
	File->SetNumberField(TEXT("started_at"), Stuck.StartedAt);
	File->SetNumberField(TEXT("threshold_s"), Stuck.ThresholdSeconds);
	File->SetStringField(TEXT("breadcrumbs"), Stuck.Breadcrumbs);
	if (!Stuck.Stack.IsEmpty())
	{
		File->SetStringField(TEXT("stack"), Stuck.Stack);
	}
	WriteAtomically(PathOf(HangFileName), ToJson(File));
}

void FManalensClient::RecordRecoveredHang(const FManalensHangWatchdog::FHang& Ended)
{
	// The game thread is back after a hang: send it once, with its duration.
	FManalensErrorOptions Options;
	Options.Severity = EManalensSeverity::Error;
	Options.ErrorType = HangErrorType;
	Options.bHandled = false;
	Options.Stack = Ended.Stack;
	TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	// Seconds with one decimal (6.2), as in the other SDKs.
	const double DurationS = FMath::RoundToDouble(Ended.DurationSeconds * 10.0) / 10.0;
	Extra->SetField(TEXT("hang_duration_s"), Number(DurationS)); // 6.2, not 6.2000000000000002
	const FString Id = RecordError(HangMessage(DurationS), Options, SourceAuto, Extra, Ended.StartedAt, Ended.MessageId);
	if (!Id.IsEmpty() && !SessionId.IsEmpty())
	{
		// A crash later in this session links to this hang.
		SessionHangId = Id;
		SaveSession();
	}
	// Remove the saved hang only now: the error is in the queue file. Keep the
	// file of a newer hang that started since.
	Hang->ClearSaved([this]() { IFileManager::Get().Delete(*PathOf(HangFileName), false, true, true); });
}

FString FManalensClient::SendSavedHang()
{
	const FString Path = PathOf(HangFileName);
	FString Text;
	if (!IFileManager::Get().FileExists(*Path) || !FFileHelper::LoadFileToString(Text, *Path))
	{
		return FString();
	}
	IFileManager::Get().Delete(*Path, false, true, true);
	const TSharedPtr<FJsonObject> Saved = FromJson(Text);
	FString Id;
	if (!Saved.IsValid() || !Saved->TryGetStringField(TEXT("message_id"), Id) || Id.IsEmpty())
	{
		return FString();
	}
	for (const FQueueEntry& Entry : Queue)
	{
		if (Entry.MessageId == Id)
		{
			return Id; // Already queued: the game recovered, then was killed.
		}
	}
	// Sent in its session, with the build of the run that hung.
	const FString OwnSession = SessionId;
	const FString OwnBuild = Config.Build;
	const FString OwnVersion = Config.AppVersion;
	FString Value;
	if (Saved->TryGetStringField(TEXT("session_id"), Value) && !Value.IsEmpty())
	{
		SessionId = Value;
	}
	const TSharedPtr<FJsonObject>* Session = nullptr;
	if (State->TryGetObjectField(TEXT("session"), Session))
	{
		(*Session)->TryGetStringField(TEXT("build"), Config.Build);
		(*Session)->TryGetStringField(TEXT("app_version"), Config.AppVersion);
	}
	FManalensErrorOptions Options;
	Options.Severity = EManalensSeverity::Error;
	Options.ErrorType = HangErrorType;
	Options.bHandled = false;
	Saved->TryGetStringField(TEXT("stack"), Options.Stack);
	TSharedPtr<FJsonObject> Extra = MakeShared<FJsonObject>();
	FString Crumbs;
	if (Saved->TryGetStringField(TEXT("breadcrumbs"), Crumbs) && !Crumbs.IsEmpty())
	{
		Extra->SetStringField(TEXT("breadcrumbs"), Crumbs.Right(MaxString));
	}
	double Threshold = 0.0;
	double StartedAt = LastActivityAt;
	Saved->TryGetNumberField(TEXT("threshold_s"), Threshold);
	Saved->TryGetNumberField(TEXT("started_at"), StartedAt);
	Id = RecordError(HangMessage(Threshold), Options, SourceAuto, Extra, StartedAt, Id, false);
	SessionId = OwnSession;
	Config.Build = OwnBuild;
	Config.AppVersion = OwnVersion;
	return Id;
}

TArray<uint64> FManalensClient::GameThreadStackAddresses()
{
	constexpr uint32 MaxDepth = 64;
	uint64 Frames[MaxDepth] = {};
	const uint32 Depth = FPlatformStackWalk::CaptureThreadStackBackTrace(GGameThreadId, Frames, MaxDepth);
	TArray<uint64> Addresses;
	Addresses.Reserve(Depth);
	for (uint32 Index = 0; Index < Depth && Index < MaxDepth; ++Index)
	{
		if (Frames[Index] != 0)
		{
			Addresses.Add(Frames[Index]);
		}
	}
	return Addresses;
}

FString FManalensClient::SymbolizeStack(const TArray<uint64>& Addresses)
{
	const TArray<FString> HomeDirs = { FPlatformProcess::UserHomeDir(), FPlatformProcess::UserDir() };
	TArray<FString> Lines;
	for (const uint64 Address : Addresses)
	{
		FProgramCounterSymbolInfo Info;
		FPlatformStackWalk::ProgramCounterToSymbolInfo(Address, Info);
		const FString Module = FPaths::GetCleanFilename(FString(ANSI_TO_TCHAR(Info.ModuleName)));
		FString Function = FString(ANSI_TO_TCHAR(Info.FunctionName));
		if (Function.IsEmpty())
		{
			Function = FString::Printf(TEXT("UnknownFunction+0x%llx"), static_cast<unsigned long long>(Info.OffsetInModule));
		}
		FString Line = (Module.IsEmpty() ? FString(TEXT("Unknown")) : Module) + TEXT("!") + Function;
		const FString File = FString(ANSI_TO_TCHAR(Info.Filename));
		if (!File.IsEmpty() && Info.LineNumber > 0)
		{
			Line += FString::Printf(TEXT(" [%s:%d]"), *FPaths::GetCleanFilename(File), Info.LineNumber);
		}
		// The signal handler that read the stack (macOS, Linux) is not the game's code.
		if (Function.Contains(TEXT("_sigtramp")) || Function.Contains(TEXT("__restore_rt")))
		{
			Lines.Reset();
			continue;
		}
		Lines.Add(CleanStackFrame(Line, HomeDirs));
	}
	FString Stack;
	for (const FString& Line : Lines)
	{
		const FString Next = Stack.IsEmpty() ? Line : Stack + TEXT("\n") + Line;
		if (Next.Len() > MaxCrashStack)
		{
			break;
		}
		Stack = Next;
	}
	return Stack;
}

bool FManalensClient::ErrorAllowed(const FString& FingerprintValue, bool bAuto)
{
	const double At = Now();
	if (const double* Last = ErrorSeen.Find(FingerprintValue))
	{
		if (At - *Last < ErrorDedupeSeconds)
		{
			return false;
		}
	}
	ErrorTimes.RemoveAll([At](double Time) { return At - Time >= ErrorWindowSeconds; });
	if (ErrorTimes.Num() >= ErrorWindowMax)
	{
		return false;
	}
	if (bAuto && AutoErrors >= Config.MaxAutoErrorsPerSession)
	{
		return false;
	}
	if (ErrorSeen.Num() > 500)
	{
		ErrorSeen.Reset();
	}
	ErrorSeen.Add(FingerprintValue, At);
	ErrorTimes.Add(At);
	return true;
}

FString FManalensClient::Fingerprint(const FString& ErrorType, const FString& Message, const FString& File, const FString& Function)
{
	const FString Basis = (!File.IsEmpty() || !Function.IsEmpty())
		? FString::Printf(TEXT("%s|%s|%s"), *ErrorType, *File, *Function)
		: FString::Printf(TEXT("%s|%s"), *ErrorType, *NormalizeMessage(Message));
	const FTCHARToUTF8 Utf8(*Basis);
	uint8 Hash[FSHA1::DigestSize];
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Hash);
	return BytesToHex(Hash, FSHA1::DigestSize).ToLower().Left(16);
}

// Crash stacks (sdks/SPEC.md, P-2831) ----------------------------------------------

namespace ManalensCrash
{
	/** The text of the first <Tag>...</Tag>, with the XML entities decoded. */
	FString XmlField(const FString& Xml, const TCHAR* Tag)
	{
		const FString Open = FString::Printf(TEXT("<%s>"), Tag);
		const FString Close = FString::Printf(TEXT("</%s>"), Tag);
		const int32 Start = Xml.Find(Open, ESearchCase::CaseSensitive);
		if (Start == INDEX_NONE)
		{
			return FString();
		}
		const int32 From = Start + Open.Len();
		const int32 End = Xml.Find(Close, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (End == INDEX_NONE)
		{
			return FString();
		}
		FString Text = Xml.Mid(From, End - From);
		Text.ReplaceInline(TEXT("&lt;"), TEXT("<"));
		Text.ReplaceInline(TEXT("&gt;"), TEXT(">"));
		Text.ReplaceInline(TEXT("&apos;"), TEXT("'"));
		Text.ReplaceInline(TEXT("&quot;"), TEXT("\""));
		Text.ReplaceInline(TEXT("&amp;"), TEXT("&"));
		return Text;
	}

	/** One CallStack line ("0x047baa34 Game!Function(int*) [File.cpp:12]) ") as "Game!Function(int*) [File.cpp:12]". */
	FString Frame(const FString& Line)
	{
		FString Text = Line.TrimStartAndEnd();
		if (Text.StartsWith(TEXT("0x")))
		{
			int32 Space = INDEX_NONE;
			if (Text.FindChar(TEXT(' '), Space))
			{
				Text = Text.Mid(Space + 1).TrimStart();
			}
		}
		if (Text.EndsWith(TEXT("])")))
		{
			Text.LeftChopInline(1);
		}
		for (const TCHAR* Unknown : { TEXT(" [UnknownFile]"), TEXT(" []") })
		{
			if (Text.EndsWith(Unknown))
			{
				Text.LeftChopInline(FCString::Strlen(Unknown));
			}
		}
		return Text.TrimEnd();
	}

	FString ErrorType(const FString& Message, const FString& CrashType)
	{
		FRegexMatcher Exception(FRegexPattern(TEXT("(EXCEPTION_[A-Z_]+)")), Message);
		if (Exception.FindNext())
		{
			return Exception.GetCaptureGroup(1);
		}
		FRegexMatcher Signal(FRegexPattern(TEXT("signal (\\d+)")), Message);
		if (Signal.FindNext())
		{
			switch (FCString::Atoi(*Signal.GetCaptureGroup(1)))
			{
			case 4: return TEXT("SIGILL");
			case 5: return TEXT("SIGTRAP");
			case 6: return TEXT("SIGABRT");
			case 7: return TEXT("SIGBUS");
			case 8: return TEXT("SIGFPE");
			case 11: return TEXT("SIGSEGV");
			default: return FString::Printf(TEXT("signal %s"), *Signal.GetCaptureGroup(1));
			}
		}
		if (Message.Contains(TEXT("Segmentation fault")))
		{
			return TEXT("SIGSEGV");
		}
		if (CrashType == TEXT("Assert") || Message.StartsWith(TEXT("Assertion failed")))
		{
			return TEXT("assert");
		}
		return TEXT("crash");
	}

	FString Hash(const FString& Basis)
	{
		const FTCHARToUTF8 Utf8(*Basis);
		uint8 Digest[FSHA1::DigestSize];
		FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
		return BytesToHex(Digest, FSHA1::DigestSize).ToLower().Left(16);
	}
}

TSharedPtr<FJsonObject> FManalensClient::ParseCrashContext(const FString& Xml, const FString& GameName, const TArray<FString>& HomeDirs)
{
	const FString CrashType = ManalensCrash::XmlField(Xml, TEXT("CrashType"));
	if (CrashType == TEXT("Ensure") || CrashType == TEXT("Stall") || CrashType == TEXT("AbnormalShutdown"))
	{
		return nullptr; // Not a crash: the game went on (an ensure or a hang report).
	}
	if (!GameName.IsEmpty())
	{
		const FString InFile = ManalensCrash::XmlField(Xml, TEXT("GameName"));
		if (InFile != GameName && InFile != FString(TEXT("UE-")) + GameName)
		{
			return nullptr; // Another game of the same engine version (the macOS folder is shared).
		}
	}
	TArray<FString> Lines;
	ManalensCrash::XmlField(Xml, TEXT("CallStack")).ParseIntoArrayLines(Lines);
	TArray<FString> Frames;
	for (const FString& Line : Lines)
	{
		const FString Frame = CleanStackFrame(ManalensCrash::Frame(Line), HomeDirs);
		if (!Frame.IsEmpty())
		{
			Frames.Add(Frame);
		}
	}
	FString Message = ManalensCrash::XmlField(Xml, TEXT("ErrorMessage")).TrimStartAndEnd();
	int32 LineEnd = INDEX_NONE;
	if (Message.FindChar(TEXT('\n'), LineEnd))
	{
		Message = Message.Left(LineEnd).TrimEnd();
	}
	if (Frames.Num() == 0 && Message.IsEmpty())
	{
		return nullptr;
	}
	const FString Type = ManalensCrash::ErrorType(Message, CrashType);
	TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
	Out->SetStringField(TEXT("error_type"), Type);
	// The message gets the full log cleaning (an assertion text can hold a
	// token). Only the Windows exception names (EXCEPTION_ACCESS_VIOLATION) are
	// kept: they are set by the engine, and the token rule would mask them.
	TArray<FString> Kept;
	FString Protected = Message.IsEmpty() ? FString(TEXT("The game crashed")) : Message;
	{
		FRegexMatcher Names(FRegexPattern(TEXT("EXCEPTION_[A-Z_]+")), Protected);
		while (Names.FindNext())
		{
			Kept.AddUnique(Names.GetCaptureGroup(0));
		}
	}
	for (int32 Index = 0; Index < Kept.Num(); ++Index)
	{
		Protected.ReplaceInline(*Kept[Index], *FString::Printf(TEXT("@@EXC%d@@"), Index), ESearchCase::CaseSensitive);
	}
	FString CleanMessage = CleanLogMessage(Protected, HomeDirs);
	for (int32 Index = 0; Index < Kept.Num(); ++Index)
	{
		CleanMessage.ReplaceInline(*FString::Printf(TEXT("@@EXC%d@@"), Index), *Kept[Index], ESearchCase::CaseSensitive);
	}
	Out->SetStringField(TEXT("message"), CleanMessage.Left(MaxString));
	Out->SetStringField(TEXT("crash_source"), TEXT("unreal_crash_folder"));
	if (Frames.Num() == 0)
	{
		return Out;
	}
	FString Stack;
	for (const FString& Frame : Frames)
	{
		const FString Next = Stack.IsEmpty() ? Frame : Stack + TEXT("\n") + Frame;
		if (Next.Len() > MaxCrashStack)
		{
			break;
		}
		Stack = Next;
	}
	Out->SetStringField(TEXT("stack"), Stack);
	// A frame has a name when it is Module!Function, not Module!UnknownFunction or an address.
	bool bSymbolicated = false;
	for (const FString& Frame : Frames)
	{
		int32 Bang = INDEX_NONE;
		if (Frame.FindChar(TEXT('!'), Bang))
		{
			const FString Name = Frame.Mid(Bang + 1);
			if (!Name.StartsWith(TEXT("UnknownFunction")) && !Name.StartsWith(TEXT("0x")) && !Name.IsEmpty())
			{
				bSymbolicated = true;
				break;
			}
		}
	}
	Out->SetBoolField(TEXT("symbolicated"), bSymbolicated);
	FString Basis = Type;
	const FRegexPattern Noise(TEXT("0x[0-9a-fA-F]+|\\[[^\\]]*\\]|\\+\\s*[0-9a-fA-Fx]+|:\\d+|\\s+"));
	for (int32 Index = 0; Index < FMath::Min(Frames.Num(), 5); ++Index)
	{
		FString Normal;
		FRegexMatcher Matcher(Noise, Frames[Index]);
		int32 Last = 0;
		while (Matcher.FindNext())
		{
			Normal += Frames[Index].Mid(Last, Matcher.GetMatchBeginning() - Last);
			Last = Matcher.GetMatchEnding();
		}
		Normal += Frames[Index].Mid(Last);
		Basis += TEXT("|") + Normal;
	}
	Out->SetStringField(TEXT("fingerprint"), ManalensCrash::Hash(Basis));
	return Out;
}

TSharedPtr<FJsonObject> FManalensClient::CrashData(double StartedAt)
{
	const FString Platform = Config.Platform.IsEmpty() ? BuildPlatform() : Config.Platform;
	if (!Config.bCaptureCrashStacks || Config.CrashDirs.Num() == 0 || !(Platform == TEXT("windows") || Platform == TEXT("macos") || Platform == TEXT("linux")))
	{
		return nullptr;
	}
	IFileManager& Files = IFileManager::Get();
	FString Best;
	double BestTime = 0.0;
	for (const FString& Dir : Config.CrashDirs)
	{
		TArray<FString> Folders;
		Files.FindFiles(Folders, *FPaths::Combine(Dir, TEXT("*")), false, true);
		for (const FString& Folder : Folders)
		{
			const FString Path = FPaths::Combine(Dir, Folder, TEXT("CrashContext.runtime-xml"));
			const FDateTime Stamp = Files.GetTimeStamp(*Path);
			if (Stamp == FDateTime::MinValue())
			{
				continue;
			}
			const double Time = static_cast<double>((Stamp - FDateTime(1970, 1, 1)).GetTicks()) / ETimespan::TicksPerSecond;
			// File times can have whole seconds.
			if (Time + 1.0 >= StartedAt && Time > BestTime)
			{
				Best = Path;
				BestTime = Time;
			}
		}
	}
	if (Best.IsEmpty())
	{
		return nullptr;
	}
	FString SeenPath;
	double SeenTime = 0.0;
	State->TryGetStringField(TEXT("crash_file"), SeenPath);
	State->TryGetNumberField(TEXT("crash_file_time"), SeenTime);
	if (SeenPath == Best && SeenTime == BestTime)
	{
		return nullptr; // Already sent.
	}
	State->SetStringField(TEXT("crash_file"), Best);
	State->SetNumberField(TEXT("crash_file_time"), BestTime);
	// At most the first 256 KB: the call stack is near the top of the file.
	TUniquePtr<FArchive> Reader(Files.CreateFileReader(*Best));
	if (!Reader)
	{
		return nullptr;
	}
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(static_cast<int32>(FMath::Min<int64>(Reader->TotalSize(), MaxCrashFileBytes)));
	Reader->Serialize(Bytes.GetData(), Bytes.Num());
	Reader->Close();
	FString Xml;
	FFileHelper::BufferToString(Xml, Bytes.GetData(), Bytes.Num());
	return ParseCrashContext(Xml, Config.CrashGameName, { FPlatformProcess::UserHomeDir(), FPlatformProcess::UserDir() });
}

// Properties -------------------------------------------------------------------

TSharedRef<FJsonObject> FManalensClient::Scalars(const TSharedPtr<FJsonObject>& Values) const
{
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	if (!Values.IsValid())
	{
		return Out;
	}
	for (const auto& Pair : Values->Values)
	{
		const FString Key(*Pair.Key);
		if (Key.Len() < 1 || Key.Len() > 100 || !Pair.Value.IsValid())
		{
			continue;
		}
		switch (Pair.Value->Type)
		{
		case EJson::Null:
			Out->SetField(Key, MakeShared<FJsonValueNull>());
			break;
		case EJson::Boolean:
			Out->SetBoolField(Key, Pair.Value->AsBool());
			break;
		case EJson::Number:
		{
			const double Value = Pair.Value->AsNumber();
			if (FMath::IsFinite(Value))
			{
				Out->SetField(Key, Number(Value));
			}
			break;
		}
		case EJson::String:
			Out->SetStringField(Key, Pair.Value->AsString().Left(MaxString));
			break;
		default:
			UE_LOG(LogManalens, Warning, TEXT("Manalens: property %s is not a scalar; dropped"), *Key);
			break;
		}
	}
	return Out;
}

TSharedRef<FJsonObject> FManalensClient::Fit(const TSharedPtr<FJsonObject>& Values, const TCHAR* Field) const
{
	if (!Values.IsValid())
	{
		return MakeShared<FJsonObject>();
	}
	const TSharedRef<FJsonObject> All = Values.ToSharedRef();
	if (All->Values.Num() <= MaxPropertyKeys && Utf8Length(ToJson(All)) <= MaxPropertyBytes)
	{
		return All;
	}
	// The collector refuses a whole batch for one event over a limit, so keep keys
	// in order while they fit, and drop the rest with a warning.
	TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
	for (const auto& Pair : All->Values)
	{
		if (Out->Values.Num() >= MaxPropertyKeys)
		{
			break;
		}
		const FString Key(*Pair.Key);
		Out->SetField(Key, Pair.Value);
		if (Utf8Length(ToJson(Out)) > MaxPropertyBytes)
		{
			Out->RemoveField(Key);
			break;
		}
	}
	UE_LOG(LogManalens, Warning, TEXT("Manalens: %s over the limit of %d keys or %d bytes; %d keys dropped"), Field, MaxPropertyKeys, MaxPropertyBytes, All->Values.Num() - Out->Values.Num());
	return Out;
}

// Storage ----------------------------------------------------------------------

FString FManalensClient::PathOf(const TCHAR* Name) const
{
	return FPaths::Combine(Config.StorageDir, Name);
}

bool FManalensClient::WriteAtomically(const FString& Path, const FString& Text) const
{
	// Write a temp file, then rename, so a crash never leaves a cut-off file.
	const FString Temp = Path + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Text, *Temp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return false;
	}
	return IFileManager::Get().Move(*Path, *Temp, true, true);
}

void FManalensClient::LoadState()
{
	State = MakeShared<FJsonObject>();
	const FString Path = PathOf(StateFileName);
	if (!IFileManager::Get().FileExists(*Path))
	{
		return; // A first run.
	}
	FString Text;
	TSharedPtr<FJsonObject> Parsed;
	if (FFileHelper::LoadFileToString(Text, *Path))
	{
		Parsed = FromJson(Text);
	}
	if (!Parsed.IsValid())
	{
		// Fail closed: it may hold an opt-out. Keep a copy, record nothing, and never overwrite it.
		bStateUnreadable = true;
		IFileManager::Get().Copy(*(Path + TEXT(".unreadable")), *Path);
		UE_LOG(LogManalens, Warning, TEXT("Manalens: could not read %s; no events until SetConsent(true)"), *Path);
		return;
	}
	State = Parsed;
}

void FManalensClient::SaveState()
{
	if (bStateUnreadable || !State.IsValid())
	{
		return; // Never overwrite a state file that could not be read.
	}
	if (!WriteAtomically(PathOf(StateFileName), ToJson(State.ToSharedRef())) && Config.bDebug)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: could not save %s"), *PathOf(StateFileName));
	}
}

void FManalensClient::SaveSession()
{
	if (SessionId.IsEmpty())
	{
		return;
	}
	TSharedRef<FJsonObject> Session = MakeShared<FJsonObject>();
	Session->SetStringField(TEXT("id"), SessionId);
	Session->SetNumberField(TEXT("started_at"), SessionStartedAt);
	Session->SetNumberField(TEXT("last_activity_at"), LastActivityAt);
	Session->SetBoolField(TEXT("focused"), bFocused);
	Session->SetBoolField(TEXT("exited"), false);
	Session->SetStringField(TEXT("build"), Config.Build);
	Session->SetStringField(TEXT("app_version"), Config.AppVersion);
	if (!SessionHangId.IsEmpty())
	{
		Session->SetStringField(TEXT("hang_message_id"), SessionHangId);
	}
	State->SetObjectField(TEXT("session"), Session);
	SaveState();
}

void FManalensClient::LoadQueue()
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *PathOf(QueueFileName)))
	{
		return;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		// "<unix seconds>\t<message id>\t<event JSON>". The JSON writer escapes tabs and line breaks.
		TArray<FString> Parts;
		if (Line.ParseIntoArray(Parts, TEXT("\t"), false) != 3 || !Parts[0].IsNumeric() || !Parts[2].StartsWith(TEXT("{")) || !Parts[2].EndsWith(TEXT("}")))
		{
			continue;
		}
		FQueueEntry& Entry = Queue.AddDefaulted_GetRef();
		Entry.At = FCString::Atod(*Parts[0]);
		Entry.MessageId = Parts[1];
		Entry.Json = Parts[2];
	}
	const int32 Overflow = Queue.Num() - Config.MaxQueue;
	if (Overflow > 0)
	{
		Queue.RemoveAt(0, Overflow);
		RaiseSendFailed(0, Overflow);
	}
	Prune();
}

void FManalensClient::SaveQueue()
{
	if (!bQueueDirty || !bIdentityActive)
	{
		return;
	}
	FString Text;
	for (const FQueueEntry& Entry : Queue)
	{
		Text += FString::Printf(TEXT("%.3f\t%s\t%s\n"), Entry.At, *Entry.MessageId, *Entry.Json);
	}
	if (WriteAtomically(PathOf(QueueFileName), Text))
	{
		bQueueDirty = false;
	}
}

void FManalensClient::WriteOptOut()
{
	const bool bOk = WriteAtomically(PathOf(OptOutFileName), TEXT("opted out\n"));
	bOptOutPending = !bOk;
	if (!bOk)
	{
		UE_LOG(LogManalens, Warning, TEXT("Manalens: could not write the opt-out marker; tracking stays off and the write is retried"));
	}
}

// Transport --------------------------------------------------------------------

void FManalensClient::DefaultTransport(const FString& InUrl, const TArray<TPair<FString, FString>>& Headers, const FString& Body, FManalensSendDone Done)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(InUrl);
	Request->SetVerb(TEXT("POST"));
	for (const TPair<FString, FString>& Header : Headers)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}
	Request->SetContentAsString(Body);
	Request->SetTimeout(10.0f);
	TSharedRef<FManalensSendDone> Callback = MakeShared<FManalensSendDone>(MoveTemp(Done));
	Request->OnProcessRequestComplete().BindLambda([Callback](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
	{
		FManalensHttpResult Result;
		if (bConnected && Response.IsValid())
		{
			Result.Status = Response->GetResponseCode();
			Result.RetryAfter = Response->GetHeader(TEXT("Retry-After"));
			Result.Body = Response->GetContentAsString().Left(4096);
		}
		(*Callback)(Result);
	});
	if (!Request->ProcessRequest())
	{
		(*Callback)(FManalensHttpResult());
	}
}

// Helpers ----------------------------------------------------------------------

FString FManalensClient::NewUuidV4()
{
	FGuid Guid = FGuid::NewGuid();
	// Manalens accepts RFC 4122 UUIDs only: set the version (4) and the variant (10xx) bits.
	Guid.B = (Guid.B & 0xFFFF0FFFu) | 0x00004000u;
	Guid.C = (Guid.C & 0x3FFFFFFFu) | 0x80000000u;
	return Guid.ToString(EGuidFormats::DigitsWithHyphensLower);
}

bool FManalensClient::IsUuidV4(const FString& Value)
{
	if (Value.Len() != 36)
	{
		return false;
	}
	for (int32 Index = 0; Index < 36; ++Index)
	{
		const TCHAR Char = Value[Index];
		if (Index == 8 || Index == 13 || Index == 18 || Index == 23)
		{
			if (Char != TEXT('-'))
			{
				return false;
			}
		}
		else if (!((Char >= TEXT('0') && Char <= TEXT('9')) || (Char >= TEXT('a') && Char <= TEXT('f'))))
		{
			return false;
		}
	}
	const TCHAR Variant = Value[19];
	return Value[14] == TEXT('4') && (Variant == TEXT('8') || Variant == TEXT('9') || Variant == TEXT('a') || Variant == TEXT('b'));
}

FString FManalensClient::KeyChecksum(const FString& PrefixAndBody)
{
	uint32 Value = Crc32(PrefixAndBody);
	FString Out;
	do
	{
		Out.InsertAt(0, Base62[Value % 62]);
		Value /= 62;
	} while (Value > 0);
	while (Out.Len() < KeyChecksumLength)
	{
		Out.InsertAt(0, TEXT('0'));
	}
	return Out;
}

FString FManalensClient::PublishableKeyRefusal(const FString& Key)
{
	if (Key.IsEmpty())
	{
		return TEXT("the Publishable key is required");
	}
	if (Key.StartsWith(SecretPrefix, ESearchCase::CaseSensitive))
	{
		return TEXT("this is a Secret key (mana_sk_). Never put a Secret key in a game build; use the Publishable key (mana_pk_...)");
	}
	if (Key.StartsWith(ApiPrefix, ESearchCase::CaseSensitive))
	{
		return TEXT("this is an API key. Never put an API key in a game build; use the Publishable key (mana_pk_...)");
	}
	if (!Key.StartsWith(PublishablePrefix, ESearchCase::CaseSensitive))
	{
		return TEXT("not a Publishable key: it must start with mana_pk_");
	}
	const int32 PrefixLength = FCString::Strlen(PublishablePrefix);
	bool bWellFormed = Key.Len() == PrefixLength + KeyBodyLength + KeyChecksumLength;
	for (int32 Index = PrefixLength; bWellFormed && Index < Key.Len(); ++Index)
	{
		bWellFormed = FCString::Strchr(Base62, Key[Index]) != nullptr;
	}
	if (!bWellFormed || KeyChecksum(Key.Left(Key.Len() - KeyChecksumLength)) != Key.Right(KeyChecksumLength))
	{
		return TEXT("malformed Publishable key (check that you copied all of it)");
	}
	return FString();
}

FString FManalensClient::FormatTimestamp(double Unix)
{
	const int64 Millis = static_cast<int64>(FMath::FloorToDouble(Unix * 1000.0 + 0.5));
	const FDateTime Time = FDateTime(1970, 1, 1) + FTimespan(Millis * ETimespan::TicksPerMillisecond);
	return Time.ToIso8601();
}

FString FManalensClient::FormatNumber(double Value)
{
	if (!FMath::IsFinite(Value))
	{
		return TEXT("null");
	}
	if (Value == FMath::FloorToDouble(Value) && FMath::Abs(Value) < 1e15)
	{
		return FString::Printf(TEXT("%lld"), static_cast<long long>(Value));
	}
	FString Short = FString::Printf(TEXT("%.15g"), Value);
	if (FCString::Atod(*Short) == Value)
	{
		return Short;
	}
	return FString::Printf(TEXT("%.17g"), Value);
}

FString FManalensClient::BuildPlatform()
{
#if PLATFORM_WINDOWS
	return TEXT("windows");
#elif PLATFORM_MAC
	return TEXT("macos");
#elif PLATFORM_LINUX || PLATFORM_LINUXARM64
	return TEXT("linux");
#elif PLATFORM_ANDROID
	return TEXT("android");
#elif PLATFORM_IOS && !PLATFORM_TVOS && !PLATFORM_VISIONOS
	return TEXT("ios");
// Consoles (P-2776). The console platform extensions define these macros; a
// desktop engine does not, so each check tests defined() first. Switch 2 comes
// before Switch in case a Switch 2 build also defines PLATFORM_SWITCH. If a
// console build still reports other, set the Platform setting.
#elif defined(PLATFORM_PS5) && PLATFORM_PS5
	return TEXT("ps5");
#elif defined(PLATFORM_XSX) && PLATFORM_XSX
	return TEXT("xbox_series");
#elif defined(PLATFORM_XB1) && PLATFORM_XB1
	return TEXT("xbox_one");
#elif defined(PLATFORM_SWITCH2) && PLATFORM_SWITCH2
	return TEXT("switch2");
#elif defined(PLATFORM_SWITCH) && PLATFORM_SWITCH
	return TEXT("switch");
#else
	// tvOS, visionOS, PS4, and other targets.
	return TEXT("other");
#endif
}

const TArray<FString>& FManalensClient::Platforms()
{
	static const TArray<FString> Values = { TEXT("windows"), TEXT("macos"), TEXT("linux"), TEXT("web"), TEXT("android"), TEXT("ios"), TEXT("ps5"), TEXT("xbox_series"), TEXT("xbox_one"), TEXT("switch"), TEXT("switch2"), TEXT("other") };
	return Values;
}

const TArray<FString>& FManalensClient::Storefronts()
{
	static const TArray<FString> Values = { TEXT("steam"), TEXT("itch"), TEXT("epic"), TEXT("gog"), TEXT("app_store"), TEXT("play"), TEXT("psn"), TEXT("xbox"), TEXT("nintendo"), TEXT("other") };
	return Values;
}

const TCHAR* FManalensClient::SeverityName(EManalensSeverity Severity)
{
	switch (Severity)
	{
	case EManalensSeverity::Debug: return TEXT("debug");
	case EManalensSeverity::Info: return TEXT("info");
	case EManalensSeverity::Warning: return TEXT("warning");
	case EManalensSeverity::Critical: return TEXT("critical");
	default: return TEXT("error");
	}
}
