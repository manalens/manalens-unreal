// Copyright (c) 2026 Manalens. MIT License.

#include "ManalensClient.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
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
	Transport = [this](const FString& InUrl, const TArray<TPair<FString, FString>>& Headers, const FString& Body, FManalensSendDone Done)
	{
		DefaultTransport(InUrl, Headers, Body, MoveTemp(Done));
	};
}

FManalensClient::~FManalensClient()
{
	*Alive = false;
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

void FManalensClient::TrackError(const FString& Message, const FManalensErrorOptions& Options, const TSharedPtr<FJsonObject>& Extra)
{
	RecordError(Message, Options, TEXT("manual"), Extra);
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
	// A log line can hold paths, URLs, emails, and tokens: clean it before it is
	// queued. The fingerprint is computed from the cleaned text, so the same error
	// on two machines with different user names groups together.
	RecordError(CleanLogMessage(Message), Options, TEXT("unreal_log"), Extra);
}

bool FManalensClient::IsSkippedLogCategory(const FString& Category)
{
	// Our own lines (a loop), failed sends, online and platform services (account
	// IDs and tokens), and Slate (UI text).
	return Category == TEXT("LogManalens") || Category == TEXT("LogHttp") || Category == TEXT("LogSlate") || Category == TEXT("LogSteamShared")
		|| Category.StartsWith(TEXT("LogOnline"), ESearchCase::CaseSensitive) || Category.StartsWith(TEXT("LogEOS"), ESearchCase::CaseSensitive);
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

	/** The value after key=, token=, secret=, password=, authorization:, or bearer. */
	void MaskKeyValues(FString& Text)
	{
		const TCHAR* Markers[] = { TEXT("key="), TEXT("token="), TEXT("secret="), TEXT("password="), TEXT("authorization:"), TEXT("bearer ") };
		const FString Redacted = TEXT("[redacted]");
		for (const TCHAR* Marker : Markers)
		{
			const bool bToLineEnd = FCString::Strcmp(Marker, TEXT("authorization:")) == 0;
			int32 From = 0;
			int32 At = INDEX_NONE;
			while ((At = Text.Find(Marker, ESearchCase::IgnoreCase, ESearchDir::FromStart, From)) != INDEX_NONE)
			{
				int32 Start = At + FCString::Strlen(Marker);
				while (Start < Text.Len() && Text[Start] == TEXT(' '))
				{
					++Start;
				}
				int32 End = Start;
				if (bToLineEnd)
				{
					// Authorization: <scheme> <credentials>, to the end of the line.
					while (End < Text.Len() && Text[End] != TEXT('\n') && Text[End] != TEXT('\r') && Text[End] != TEXT('"') && Text[End] != TEXT('\''))
					{
						++End;
					}
				}
				else
				{
					while (End < Text.Len() && !IsEnd(Text[End]) && Text[End] != TEXT('&') && Text[End] != TEXT(',') && Text[End] != TEXT(';'))
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

	/** Runs of 24 or more characters from A-Z, a-z, 0-9, _, and - (keys, tokens, and IDs). */
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
			if (End - Index >= 24)
			{
				Out += TEXT("[redacted]");
				Index = End;
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
	const double At = Now();
	if (At >= NextTickAt)
	{
		NextTickAt = At + Config.FlushIntervalSeconds;
		if (bIdentityActive)
		{
			// While the game has focus the player is active: keep the session alive.
			if (bFocused)
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
}

void FManalensClient::SetFocused(bool bInFocused)
{
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
		Touch();
	}
}

void FManalensClient::BeginShutdown()
{
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
		// No normal exit and the game had focus: count it as a crash. A game the
		// OS closed in the background is not a crash.
		if (!bExited && bWasFocused)
		{
			TSharedPtr<FJsonObject> Crash = MakeShared<FJsonObject>();
			Crash->SetStringField(TEXT("previous_session_id"), PreviousId);
			for (const TCHAR* Key : { TEXT("build"), TEXT("app_version") })
			{
				FString Value;
				if (Session->TryGetStringField(Key, Value) && !Value.IsEmpty())
				{
					Crash->SetStringField(FString(TEXT("previous_")) + Key, Value);
				}
			}
			Enqueue(TEXT("track"), TEXT("app_unclean_exit"), Crash, LastActivityAt);
		}
		EndSession(LastActivityAt);
	}
	StartSession(At);
	TSharedPtr<FJsonObject> Open = MakeShared<FJsonObject>();
	Open->SetBoolField(TEXT("is_first_open"), bFirstOpen);
	FString FirstOpenAt = FormatTimestamp(At);
	State->TryGetStringField(TEXT("first_open_at"), FirstOpenAt);
	Open->SetStringField(TEXT("first_open_at"), FirstOpenAt);
	Enqueue(TEXT("track"), TEXT("game_open"), Open);
	SaveState();
	if (DeferredIdentify.IsSet())
	{
		const TPair<FString, TSharedPtr<FJsonObject>> Pending = DeferredIdentify.GetValue();
		DeferredIdentify.Reset();
		Identify(Pending.Key, Pending.Value);
	}
	// Sent on the next Update(), so events of the first frame share the request.
	bFlushRequested = true;
}

void FManalensClient::StartSession(double At)
{
	SessionId = NewId();
	SessionStartedAt = At;
	LastActivityAt = At;
	AutoErrors = 0;
	SaveSession();
	Enqueue(TEXT("track"), TEXT("session_start"), nullptr, At);
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
	Enqueue(TEXT("track"), TEXT("session_end"), Ended, At);
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

void FManalensClient::Enqueue(const FString& Type, const FString& Event, const TSharedPtr<FJsonObject>& Properties, double At, const TSharedPtr<FJsonObject>& Traits)
{
	const double When = At < 0.0 ? Now() : At;
	TSharedRef<FJsonObject> Envelope = MakeShared<FJsonObject>();
	const FString MessageId = NewId();
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
	if (Type == TEXT("track") && Event != TEXT("error"))
	{
		RecentEvents.Add(Event);
		if (RecentEvents.Num() > Breadcrumbs)
		{
			RecentEvents.RemoveAt(0);
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
	const int32 Count = FMath::Min(Config.FlushAt, Queue.Num());
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
	Body += TEXT("]}");
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
	// A 4xx other than 408 and 429 will fail again: drop the batch (as the Godot,
	// Unity, and web SDKs do). Network errors (0) and 5xx stay and back off.
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
	const EManalensSendOutcome Outcome = ClassifyStatus(Result.Status);
	if (Outcome != EManalensSendOutcome::Retry)
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

void FManalensClient::RecordError(const FString& Message, const FManalensErrorOptions& Options, const FString& Source, const TSharedPtr<FJsonObject>& Extra)
{
	if (!bInitialized || !bIdentityActive)
	{
		return;
	}
	const bool bManual = Source == TEXT("manual");
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
	if (!ErrorAllowed(FingerprintValue, bManual))
	{
		++ErrorsDropped;
		return;
	}
	if (BeforeSend)
	{
		const TSharedPtr<FJsonObject> Changed = BeforeSend(Props);
		if (!Changed.IsValid())
		{
			return;
		}
		Props = Scalars(Changed);
	}
	if (!bManual)
	{
		++AutoErrors;
	}
	Touch();
	Enqueue(TEXT("track"), TEXT("error"), Props);
	// Keep the error on disk now: a crash can follow it.
	SaveQueue();
}

bool FManalensClient::ErrorAllowed(const FString& FingerprintValue, bool bManual)
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
	if (!bManual && AutoErrors >= Config.MaxAutoErrorsPerSession)
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
#else
	// tvOS, visionOS, consoles, and other targets.
	return TEXT("other");
#endif
}

const TArray<FString>& FManalensClient::Storefronts()
{
	static const TArray<FString> Values = { TEXT("steam"), TEXT("itch"), TEXT("epic"), TEXT("gog"), TEXT("app_store"), TEXT("play"), TEXT("other") };
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
