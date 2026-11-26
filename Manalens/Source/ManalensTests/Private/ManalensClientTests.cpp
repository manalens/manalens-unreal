// Copyright (c) 2026 Manalens. MIT License.
//
// Automation tests of FManalensClient: a fake clock, fake IDs, and a fake
// transport, so they need no network. Run them headless:
//   UnrealEditor-Cmd <project> -ExecCmds="Automation RunTests Manalens; Quit" -unattended -nullrhi -nopause

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "ManalensClient.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, ManalensTests)

#if WITH_DEV_AUTOMATION_TESTS

namespace ManalensTests
{
	constexpr EAutomationTestFlags Flags = EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter;

	double UnixOf(int32 Year, int32 Month, int32 Day, int32 Hour, int32 Minute, int32 Second)
	{
		return static_cast<double>((FDateTime(Year, Month, Day, Hour, Minute, Second) - FDateTime(1970, 1, 1)).GetTicks()) / ETimespan::TicksPerSecond;
	}

	/** A key in the Publishable key format with a valid checksum. Not a real key. */
	FString SyntheticKey()
	{
		const FString PrefixAndBody = FString(TEXT("mana_pk_")) + FString::ChrN(43, TEXT('T'));
		return PrefixAndBody + FManalensClient::KeyChecksum(PrefixAndBody);
	}

	TSharedPtr<FJsonObject> ParseObject(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	struct FRequest
	{
		FString Url;
		TArray<TPair<FString, FString>> Headers;
		FString Body;

		TArray<TSharedPtr<FJsonObject>> Events() const
		{
			TArray<TSharedPtr<FJsonObject>> Out;
			const TSharedPtr<FJsonObject> Parsed = ParseObject(Body);
			const TArray<TSharedPtr<FJsonValue>>* Array = nullptr;
			if (Parsed.IsValid() && Parsed->TryGetArrayField(TEXT("events"), Array))
			{
				for (const TSharedPtr<FJsonValue>& Value : *Array)
				{
					Out.Add(Value->AsObject());
				}
			}
			return Out;
		}

		TArray<FString> MessageIds() const
		{
			TArray<FString> Out;
			for (const TSharedPtr<FJsonObject>& Event : Events())
			{
				Out.Add(Event->GetStringField(TEXT("message_id")));
			}
			return Out;
		}
	};

	/** A fake clock, sequential IDs, and a transport that answers with Status (or not at all when bHold). */
	struct FHarness
	{
		double Time = UnixOf(2027, 1, 15, 8, 0, 0);
		int32 NextId = 1;
		bool bSequentialIds = true;
		int32 Status = 202;
		FString RetryAfter;
		FString ResponseBody;
		bool bHold = false;
		TArray<FRequest> Requests;
		TArray<TPair<int32, int32>> SendFailed;
		FString Dir;

		FHarness()
		{
			Dir = FPaths::Combine(FPaths::AutomationTransientDir(), TEXT("Manalens"), FGuid::NewGuid().ToString());
		}

		~FHarness()
		{
			IFileManager::Get().DeleteDirectory(*Dir, false, true);
		}

		FManalensConfig Config() const
		{
			FManalensConfig Out;
			Out.PublishableKey = SyntheticKey();
			Out.Endpoint = TEXT("https://collector.test/");
			Out.AppVersion = TEXT("1.4.0");
			Out.Build = TEXT("571");
			Out.Storefront = TEXT("steam");
			Out.Platform = TEXT("windows");
			Out.Locale = TEXT("en-US");
			Out.UtcOffsetMinutes = 9999; // not sent
			Out.StorageDir = Dir;
			return Out;
		}

		TSharedRef<FManalensClient> Make()
		{
			TSharedRef<FManalensClient> Client = MakeShared<FManalensClient>();
			Client->Now = [this]() { return Time; };
			Client->NewId = [this]() { return bSequentialIds ? FString::Printf(TEXT("00000000-0000-4000-8000-%012d"), NextId++) : FManalensClient::NewUuidV4(); };
			Client->Random = []() { return 0.5; };
			Client->OnSendFailed = [this](int32 InStatus, int32 Dropped) { SendFailed.Emplace(InStatus, Dropped); };
			Client->Transport = [this](const FString& Url, const TArray<TPair<FString, FString>>& Headers, const FString& Body, FManalensSendDone Done)
			{
				Requests.Add({ Url, Headers, Body });
				if (!bHold)
				{
					FManalensHttpResult Result;
					Result.Status = Status;
					Result.RetryAfter = RetryAfter;
					Result.Body = ResponseBody;
					Done(Result);
				}
			};
			return Client;
		}

		TSharedRef<FManalensClient> Start(const FManalensConfig& InConfig)
		{
			TSharedRef<FManalensClient> Client = Make();
			Client->Init(InConfig);
			return Client;
		}

		TSharedRef<FManalensClient> Start() { return Start(Config()); }
	};

	TArray<TSharedPtr<FJsonObject>> Queued(const FManalensClient& Client)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		for (const FString& Json : Client.GetQueuedEvents())
		{
			Out.Add(ParseObject(Json));
		}
		return Out;
	}

	FString NameOf(const TSharedPtr<FJsonObject>& Event)
	{
		FString Name;
		if (!Event->TryGetStringField(TEXT("event"), Name))
		{
			Name = Event->GetStringField(TEXT("type"));
		}
		return Name;
	}

	TArray<FString> Names(const TArray<TSharedPtr<FJsonObject>>& Events)
	{
		TArray<FString> Out;
		for (const TSharedPtr<FJsonObject>& Event : Events)
		{
			Out.Add(NameOf(Event));
		}
		return Out;
	}

	TSharedPtr<FJsonObject> Object(std::initializer_list<TPair<FString, TSharedPtr<FJsonValue>>> Fields)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Field : Fields)
		{
			Out->SetField(Field.Key, Field.Value);
		}
		return Out;
	}

	TSharedPtr<FJsonValue> Num(double Value) { return MakeShared<FJsonValueNumber>(Value); }
	TSharedPtr<FJsonValue> Str(const FString& Value) { return MakeShared<FJsonValueString>(Value); }
	TSharedPtr<FJsonValue> Bool(bool Value) { return MakeShared<FJsonValueBoolean>(Value); }

	/** Deep equality that ignores key order. Writes the first difference to Diff. */
	bool JsonEquals(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B, const FString& Path, FString& Diff)
	{
		if (!A.IsValid() || !B.IsValid() || A->Type != B->Type)
		{
			Diff = FString::Printf(TEXT("%s: type differs"), *Path);
			return false;
		}
		switch (A->Type)
		{
		case EJson::Object:
		{
			const TSharedPtr<FJsonObject> OA = A->AsObject();
			const TSharedPtr<FJsonObject> OB = B->AsObject();
			TArray<FString> KA;
			TArray<FString> KB;
			for (const auto& Pair : OA->Values)
			{
				KA.Add(FString(*Pair.Key));
			}
			for (const auto& Pair : OB->Values)
			{
				KB.Add(FString(*Pair.Key));
			}
			KA.Sort();
			KB.Sort();
			if (KA != KB)
			{
				Diff = FString::Printf(TEXT("%s: keys [%s] and [%s]"), *Path, *FString::Join(KA, TEXT(",")), *FString::Join(KB, TEXT(",")));
				return false;
			}
			for (const FString& Key : KA)
			{
				if (!JsonEquals(OA->TryGetField(Key), OB->TryGetField(Key), Path + TEXT(".") + Key, Diff))
				{
					return false;
				}
			}
			return true;
		}
		case EJson::Array:
		{
			const TArray<TSharedPtr<FJsonValue>>& AA = A->AsArray();
			const TArray<TSharedPtr<FJsonValue>>& AB = B->AsArray();
			if (AA.Num() != AB.Num())
			{
				Diff = FString::Printf(TEXT("%s: %d and %d items"), *Path, AA.Num(), AB.Num());
				return false;
			}
			for (int32 Index = 0; Index < AA.Num(); ++Index)
			{
				if (!JsonEquals(AA[Index], AB[Index], FString::Printf(TEXT("%s[%d]"), *Path, Index), Diff))
				{
					return false;
				}
			}
			return true;
		}
		case EJson::Number:
			if (A->AsNumber() != B->AsNumber())
			{
				Diff = FString::Printf(TEXT("%s: %s and %s"), *Path, *A->AsString(), *B->AsString());
				return false;
			}
			return true;
		case EJson::String:
			if (A->AsString() != B->AsString())
			{
				Diff = FString::Printf(TEXT("%s: \"%s\" and \"%s\""), *Path, *A->AsString(), *B->AsString());
				return false;
			}
			return true;
		case EJson::Boolean:
			if (A->AsBool() != B->AsBool())
			{
				Diff = FString::Printf(TEXT("%s: booleans differ"), *Path);
				return false;
			}
			return true;
		default:
			return true;
		}
	}

	/** sdks/unity/Tests~/fixtures and sdks/unreal/Tests/fixtures: from the environment, else next to the plugin in the repository. */
	FString FixturesDir(const TCHAR* EnvName, const TCHAR* RelativeToPlugin)
	{
		const FString FromEnv = FPlatformMisc::GetEnvironmentVariable(EnvName);
		if (!FromEnv.IsEmpty())
		{
			return FromEnv;
		}
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("Manalens"));
		return Plugin.IsValid() ? FPaths::ConvertRelativePathToFull(FPaths::Combine(Plugin->GetBaseDir(), RelativeToPlugin)) : FString();
	}
}

using namespace ManalensTests;

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensUuidTest, "Manalens.Ids.UuidV4", Flags)
bool FManalensUuidTest::RunTest(const FString& Parameters)
{
	TSet<FString> Seen;
	for (int32 Index = 0; Index < 2000; ++Index)
	{
		const FString Id = FManalensClient::NewUuidV4();
		if (!FManalensClient::IsUuidV4(Id) || Id.Len() != 36 || Id[14] != TEXT('4') || Id != Id.ToLower())
		{
			AddError(FString::Printf(TEXT("not a lower-case UUID v4: %s"), *Id));
			return false;
		}
		Seen.Add(Id);
	}
	TestEqual(TEXT("2000 IDs are unique"), Seen.Num(), 2000);
	TestTrue(TEXT("a v4 UUID"), FManalensClient::IsUuidV4(TEXT("00000000-0000-4000-8000-000000000001")));
	TestFalse(TEXT("version 1"), FManalensClient::IsUuidV4(TEXT("00000000-0000-1000-8000-000000000001")));
	TestFalse(TEXT("variant c"), FManalensClient::IsUuidV4(TEXT("00000000-0000-4000-c000-000000000001")));
	TestFalse(TEXT("upper case"), FManalensClient::IsUuidV4(TEXT("ABCDEF00-0000-4000-8000-000000000001")));
	TestFalse(TEXT("no hyphens"), FManalensClient::IsUuidV4(TEXT("00000000000040008000000000000001")));
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensPayloadTest, "Manalens.Payload.MatchesUnityFixtures", Flags)
bool FManalensPayloadTest::RunTest(const FString& Parameters)
{
	// The scenario of the Unity SDK fixtures (sdks/unity/Tests~/fixtures): a first
	// launch with events, an identify, two errors, and a crash; then a second
	// launch with a new build and a normal quit.
	FHarness H;
	const double T0 = H.Time;
	{
		TSharedRef<FManalensClient> Client = H.Start();
		TestTrue(TEXT("started"), Client->IsIdentityActive());
		Client->Track(TEXT("level_start"), Object({ { TEXT("level"), Num(3) }, { TEXT("mode"), Str(TEXT("story")) } }));
		Client->Track(TEXT("resource"), Object({
			{ TEXT("flow"), Str(TEXT("source")) }, { TEXT("currency"), Str(TEXT("gold")) }, { TEXT("amount"), Num(25) }, { TEXT("ratio"), Num(0.75) },
			{ TEXT("float"), Num(0.1) }, { TEXT("big"), Num(9000000000.0) }, { TEXT("price"), Num(4.99) }, { TEXT("boss"), Bool(false) },
			{ TEXT("note"), MakeShared<FJsonValueNull>() }, { TEXT("text"), Str(TEXT("quote \" slash \\ line\nend")) } }));
		Client->Identify(TEXT("player_42"), Object({ { TEXT("plan"), Str(TEXT("supporter")) }, { TEXT("level"), Num(12) }, { TEXT("vip"), Bool(true) } }));
		FManalensErrorOptions Options;
		Options.Severity = EManalensSeverity::Critical;
		Options.ErrorType = TEXT("SaveFailed");
		Options.bHandled = false;
		Client->TrackError(TEXT("Save failed for slot 3"), Options, Object({ { TEXT("slot"), Num(3) } }));
		H.Time = T0 + 5;
		Client->TrackLogError(TEXT("LogTemp"), TEXT("Accessed None trying to read property Player"), false);
		TestEqual(TEXT("nothing is sent before the first Update()"), H.Requests.Num(), 0);
		H.Time = T0 + 15;
		Client->Update();
		TestEqual(TEXT("one request on the flush interval"), H.Requests.Num(), 1);
		// No FinishShutdown(): the process "crashes" here.
	}
	{
		H.Time = T0 + 135;
		FManalensConfig Config = H.Config();
		Config.AppVersion = TEXT("1.4.1-beta.2+572");
		Config.Build = TEXT("572");
		Config.Storefront = TEXT("itch");
		TSharedRef<FManalensClient> Client = H.Start(Config);
		Client->Track(TEXT("level_complete"), Object({ { TEXT("level"), Num(3) }, { TEXT("score"), Num(1200) } }));
		H.Time = T0 + 177;
		Client->BeginShutdown();
		Client->FinishShutdown();
	}
	if (!TestEqual(TEXT("two requests"), H.Requests.Num(), 2))
	{
		return false;
	}
	for (const FRequest& Request : H.Requests)
	{
		TestEqual(TEXT("URL"), Request.Url, FString(TEXT("https://collector.test/v0/events")));
		TestTrue(TEXT("publishable key header"), Request.Headers.Contains(TPair<FString, FString>(TEXT("x-publishable-key"), SyntheticKey())));
		TestTrue(TEXT("at most 50 events"), Request.Events().Num() >= 1 && Request.Events().Num() <= 50);
		for (const TSharedPtr<FJsonObject>& Event : Request.Events())
		{
			TestEqual(TEXT("engine"), Event->GetObjectField(TEXT("context"))->GetObjectField(TEXT("app"))->GetStringField(TEXT("engine")), FString(TEXT("unreal")));
			TestTrue(TEXT("anonymous_id is a UUID v4"), FManalensClient::IsUuidV4(Event->GetStringField(TEXT("anonymous_id"))));
			TestTrue(TEXT("every event has a session"), Event->HasField(TEXT("session_id")));
		}
	}
	const TArray<TSharedPtr<FJsonObject>> First = H.Requests[0].Events();
	const TArray<TSharedPtr<FJsonObject>> Second = H.Requests[1].Events();
	TestEqual(TEXT("first request"), FString::Join(Names(First), TEXT(",")), FString(TEXT("session_start,game_open,level_start,resource,identify,error,error")));
	TestEqual(TEXT("second request"), FString::Join(Names(Second), TEXT(",")), FString(TEXT("app_unclean_exit,session_end,session_start,game_open,level_complete,session_end")));
	TestEqual(TEXT("the manual error has the same fingerprint as in the Godot and Unity SDKs"), First[5]->GetObjectField(TEXT("properties"))->GetStringField(TEXT("fingerprint")), FString(TEXT("38d0e3d0139dea87")));
	TestTrue(TEXT("integers have no fraction"), H.Requests[0].Body.Contains(TEXT("\"amount\":25,")) && H.Requests[0].Body.Contains(TEXT("\"big\":9000000000,")));
	TestTrue(TEXT("short decimals"), H.Requests[0].Body.Contains(TEXT("\"float\":0.1,")) && H.Requests[0].Body.Contains(TEXT("\"price\":4.99,")));

	// Write the bodies for the repository tests (tests/unreal-sdk.test.mjs).
	const FString OutDir = FPlatformMisc::GetEnvironmentVariable(TEXT("MANALENS_WRITE_FIXTURES"));
	if (!OutDir.IsEmpty())
	{
		for (int32 Index = 0; Index < H.Requests.Num(); ++Index)
		{
			FFileHelper::SaveStringToFile(H.Requests[Index].Body + TEXT("\n"), *FPaths::Combine(OutDir, FString::Printf(TEXT("request-%d.json"), Index + 1)), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		}
		AddInfo(FString::Printf(TEXT("fixtures written to %s"), *OutDir));
	}

	// The same events as the Unity SDK, except engine, library_name, and the log error.
	const FString UnityDir = FixturesDir(TEXT("MANALENS_UNITY_FIXTURES"), TEXT("../../unity/Tests~/fixtures"));
	if (!IFileManager::Get().FileExists(*FPaths::Combine(UnityDir, TEXT("request-1.json"))))
	{
		AddWarning(FString::Printf(TEXT("Unity fixtures not found in %s; set MANALENS_UNITY_FIXTURES to compare"), *UnityDir));
		return true;
	}
	for (int32 Index = 0; Index < 2; ++Index)
	{
		FString Text;
		FFileHelper::LoadFileToString(Text, *FPaths::Combine(UnityDir, FString::Printf(TEXT("request-%d.json"), Index + 1)));
		const TSharedPtr<FJsonObject> Unity = ParseObject(Text);
		const TSharedPtr<FJsonObject> Ours = ParseObject(H.Requests[Index].Body);
		if (!TestTrue(TEXT("Unity fixture parses"), Unity.IsValid()) || !TestTrue(TEXT("body parses"), Ours.IsValid()))
		{
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>& UnityEvents = Unity->GetArrayField(TEXT("events"));
		const TArray<TSharedPtr<FJsonValue>>& OurEvents = Ours->GetArrayField(TEXT("events"));
		TestEqual(TEXT("event count"), OurEvents.Num(), UnityEvents.Num());
		for (int32 E = 0; E < FMath::Min(UnityEvents.Num(), OurEvents.Num()); ++E)
		{
			TSharedPtr<FJsonObject> U = UnityEvents[E]->AsObject();
			TSharedPtr<FJsonObject> O = OurEvents[E]->AsObject();
			U->GetObjectField(TEXT("context"))->GetObjectField(TEXT("app"))->SetStringField(TEXT("engine"), TEXT("unreal"));
			U->GetObjectField(TEXT("context"))->SetStringField(TEXT("library_name"), FManalensClient::LibraryName);
			// Each SDK has its own version.
			U->GetObjectField(TEXT("context"))->SetStringField(TEXT("library_version"), FManalensClient::LibraryVersion);
			U->SetStringField(TEXT("version"), FManalensClient::LibraryVersion);
			FString Source;
			if (U->GetObjectField(TEXT("properties"))->TryGetStringField(TEXT("source"), Source) && Source == TEXT("unity_log"))
			{
				// An engine log error: Unity and Unreal logs differ. Compare the envelope only.
				TestEqual(TEXT("log error source"), O->GetObjectField(TEXT("properties"))->GetStringField(TEXT("source")), FString(TEXT("unreal_log")));
				U->RemoveField(TEXT("properties"));
				O->RemoveField(TEXT("properties"));
			}
			FString Diff;
			if (!JsonEquals(MakeShared<FJsonValueObject>(U), MakeShared<FJsonValueObject>(O), FString::Printf(TEXT("request-%d.events[%d]"), Index + 1, E), Diff))
			{
				AddError(FString::Printf(TEXT("differs from the Unity fixture: %s"), *Diff));
			}
		}
	}
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensSessionTest, "Manalens.Session.Rollover", Flags)
bool FManalensSessionTest::RunTest(const FString& Parameters)
{
	FHarness H;
	H.bHold = true; // Keep every event in the queue.
	FManalensConfig Config = H.Config();
	Config.FlushAt = 50;
	Config.MaxQueue = 5000;
	TSharedRef<FManalensClient> Client = H.Start(Config);
	const double T0 = H.Time;
	const FString First = Client->GetSessionId();
	TestTrue(TEXT("the session ID is a UUID v4"), FManalensClient::IsUuidV4(First));
	TestNotEqual(TEXT("the session ID is not the anonymous ID"), First, Client->GetAnonymousId());

	H.Time = T0 + 60;
	Client->Track(TEXT("a"));
	H.Time = T0 + 60 + 29 * 60;
	Client->Track(TEXT("b"));
	TestEqual(TEXT("29 minutes idle: same session"), Client->GetSessionId(), First);
	H.Time = T0 + 60 + 29 * 60 + 31 * 60;
	Client->Track(TEXT("c"));
	const FString Second = Client->GetSessionId();
	TestNotEqual(TEXT("31 minutes idle: a new session"), Second, First);

	TArray<TSharedPtr<FJsonObject>> Events = Queued(*Client);
	TestEqual(TEXT("events"), FString::Join(Names(Events), TEXT(",")), FString(TEXT("session_start,game_open,a,b,session_end,session_start,c")));
	const TSharedPtr<FJsonObject> End = Events[4];
	TestEqual(TEXT("session_end is in the old session"), End->GetStringField(TEXT("session_id")), First);
	TestEqual(TEXT("session_end at the last activity"), End->GetStringField(TEXT("original_timestamp")), FManalensClient::FormatTimestamp(T0 + 60 + 29 * 60));
	TestEqual(TEXT("duration_s"), End->GetObjectField(TEXT("properties"))->GetNumberField(TEXT("duration_s")), 30.0 * 60.0);
	TestEqual(TEXT("c is in the new session"), Events[6]->GetStringField(TEXT("session_id")), Second);

	// A game that stays active gets a new session every 24 hours.
	const double Start = H.Time;
	for (int32 Step = 1; Step <= 26 * 6; ++Step)
	{
		H.Time = Start + Step * 600.0;
		Client->Track(TEXT("tick"));
	}
	int32 Starts = 0;
	for (const TSharedPtr<FJsonObject>& Event : Queued(*Client))
	{
		if (NameOf(Event) == TEXT("session_start"))
		{
			++Starts;
		}
	}
	TestEqual(TEXT("one more session after 24 hours of play"), Starts, 3);
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensQueueTest, "Manalens.Queue.PersistenceAndAge", Flags)
bool FManalensQueueTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("events dropped"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FHarness H;
	const double T0 = H.Time;
	TArray<FString> Sent;
	{
		H.Status = 0; // offline
		TSharedRef<FManalensClient> Client = H.Start();
		Client->Track(TEXT("level_start"));
		Client->Update(); // flush requested at start
		TestEqual(TEXT("one attempt"), H.Requests.Num(), 1);
		TestEqual(TEXT("offline: the batch stays queued"), Client->GetQueueSize(), 3);
		TestTrue(TEXT("offline: backoff"), Client->GetBackoffUntil() > H.Time);
		Sent = H.Requests[0].MessageIds();
		Client->FinishShutdown();
	}
	{
		H.Time = T0 + 3600;
		H.Status = 202;
		TSharedRef<FManalensClient> Client = H.Start();
		TestEqual(TEXT("the queue is loaded from disk, with session_end, session_start, and game_open of this launch"), Client->GetQueueSize(), 6);
		Client->Flush();
		TestEqual(TEXT("sent"), H.Requests.Num(), 2);
		const TArray<FString> Again = H.Requests[1].MessageIds();
		for (const FString& Id : Sent)
		{
			TestTrue(TEXT("a retry keeps the message_id"), Again.Contains(Id));
		}
		TestEqual(TEXT("the queue is empty"), Client->GetQueueSize(), 0);
		Client->FinishShutdown();
	}
	{
		// Events older than 7 days are dropped at load.
		H.Status = 0;
		H.Time = T0 + 7200;
		TSharedRef<FManalensClient> Client = H.Start();
		Client->Track(TEXT("old"));
		Client->FinishShutdown();
	}
	{
		H.SendFailed.Reset();
		H.Time = T0 + 7200 + 7 * 24 * 3600;
		H.bHold = true;
		TSharedRef<FManalensClient> Client = H.Start();
		TestTrue(TEXT("old events are dropped"), H.SendFailed.Num() == 1 && H.SendFailed[0].Key == 0 && H.SendFailed[0].Value >= 3);
		for (const TSharedPtr<FJsonObject>& Event : Queued(*Client))
		{
			TestNotEqual(TEXT("no event older than 7 days"), NameOf(Event), FString(TEXT("old")));
		}
		Client->FinishShutdown();
	}
	{
		// At most MaxQueue events: the oldest are dropped first.
		FHarness Small;
		Small.bHold = true;
		FManalensConfig Config = Small.Config();
		Config.MaxQueue = 25;
		TSharedRef<FManalensClient> Client = Small.Start(Config);
		for (int32 Index = 0; Index < 40; ++Index)
		{
			Client->Track(FString::Printf(TEXT("e%d"), Index));
		}
		TestEqual(TEXT("queue capped"), Client->GetQueueSize(), 25);
		int32 Dropped = 0;
		for (const TPair<int32, int32>& Failure : Small.SendFailed)
		{
			Dropped += Failure.Value;
		}
		TestEqual(TEXT("dropped events are reported"), Dropped, 17);
		TestEqual(TEXT("the newest event is kept"), NameOf(Queued(*Client).Last()), FString(TEXT("e39")));
	}
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensInstallKillTest, "Manalens.Queue.InstallSavedBeforeAnySend", Flags)
bool FManalensInstallKillTest::RunTest(const FString& Parameters)
{
	FHarness H;
	H.bHold = true; // No send completes.
	const double T0 = H.Time;
	FString OnDisk;
	{
		// First launch, offline, killed before any send, tick, focus loss or quit.
		TSharedRef<FManalensClient> Client = H.Start();
		FFileHelper::LoadFileToString(OnDisk, *FPaths::Combine(H.Dir, FManalensClient::QueueFileName));
	}
	TestTrue(TEXT("game_open is on disk before any send"), OnDisk.Contains(TEXT("\"game_open\"")) && OnDisk.Contains(TEXT("\"is_first_open\":true")));
	H.Time = T0 + 3 * 24 * 3600;
	TSharedRef<FManalensClient> Client = H.Start();
	TArray<TSharedPtr<FJsonObject>> Opens;
	for (const TSharedPtr<FJsonObject>& Event : Queued(*Client))
	{
		if (NameOf(Event) == TEXT("game_open"))
		{
			Opens.Add(Event);
		}
	}
	if (TestEqual(TEXT("two game_open events after the relaunch"), Opens.Num(), 2))
	{
		TestTrue(TEXT("the first is the install"), Opens[0]->GetObjectField(TEXT("properties"))->GetBoolField(TEXT("is_first_open")));
		TestEqual(TEXT("the install keeps the time it happened"), Opens[0]->GetStringField(TEXT("original_timestamp")), FString(TEXT("2027-01-15T08:00:00.000Z")));
		TestFalse(TEXT("the second is not"), Opens[1]->GetObjectField(TEXT("properties"))->GetBoolField(TEXT("is_first_open")));
		TestEqual(TEXT("same anonymous ID"), Opens[0]->GetStringField(TEXT("anonymous_id")), Opens[1]->GetStringField(TEXT("anonymous_id")));
	}
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensShutdownTickTest, "Manalens.Session.NoNewSessionWhileShuttingDown", Flags)
bool FManalensShutdownTickTest::RunTest(const FString& Parameters)
{
	FHarness H;
	H.bHold = true; // The shutdown send never completes, so the game keeps calling Update().
	TSharedRef<FManalensClient> Client = H.Start();
	Client->BeginShutdown();
	Client->SetFocused(true);
	H.Time += H.Config().FlushIntervalSeconds + 1;
	Client->Update();
	Client->Update();
	const TArray<FString> Q = Names(Queued(*Client));
	TestEqual(TEXT("the queue ends with session_end"), Q.Num() > 0 ? Q.Last() : FString(), FString(TEXT("session_end")));
	TestEqual(TEXT("one session_start"), Q.FilterByPredicate([](const FString& N) { return N == TEXT("session_start"); }).Num(), 1);
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensConsentTest, "Manalens.Consent.WithdrawWipesState", Flags)
bool FManalensConsentTest::RunTest(const FString& Parameters)
{
	FHarness H;
	H.bHold = true;
	FManalensConfig Config = H.Config();
	Config.bConsentRequired = true;
	const FString StatePath = FPaths::Combine(H.Dir, FManalensClient::StateFileName);
	const FString QueuePath = FPaths::Combine(H.Dir, FManalensClient::QueueFileName);
	const FString OptOutPath = FPaths::Combine(H.Dir, FManalensClient::OptOutFileName);
	FString OldId;
	{
		TSharedRef<FManalensClient> Client = H.Start(Config);
		TestFalse(TEXT("no identity before consent"), Client->IsIdentityActive());
		Client->Track(TEXT("ignored"));
		Client->Identify(TEXT("player_42"));
		TestEqual(TEXT("no events before consent"), Client->GetQueueSize(), 0);
		TestTrue(TEXT("no ID before consent"), Client->GetAnonymousId().IsEmpty());
		TestEqual(TEXT("LinkUrl is unchanged before consent"), Client->LinkUrl(TEXT("https://example.com")), FString(TEXT("https://example.com")));

		Client->SetConsent(true);
		TestTrue(TEXT("consent starts the identity"), Client->IsIdentityActive());
		TestEqual(TEXT("game_open and the deferred identify"), FString::Join(Names(Queued(*Client)), TEXT(",")), FString(TEXT("session_start,game_open,identify")));
		OldId = Client->GetAnonymousId();
		Client->Track(TEXT("level_start"));
		Client->Update();
		H.Time += 20;
		Client->Update(); // saves the queue
		TestTrue(TEXT("the queue is on disk"), IFileManager::Get().FileExists(*QueuePath));

		Client->SetConsent(false);
		TestFalse(TEXT("withdrawn: identity off"), Client->IsIdentityActive());
		TestEqual(TEXT("withdrawn: the queue is empty"), Client->GetQueueSize(), 0);
		TestTrue(TEXT("withdrawn: no anonymous ID"), Client->GetAnonymousId().IsEmpty());
		TestTrue(TEXT("withdrawn: no session"), Client->GetSessionId().IsEmpty());
		TestTrue(TEXT("withdrawn: no user ID"), Client->GetUserId().IsEmpty());
		TestFalse(TEXT("withdrawn: the queue file is deleted"), IFileManager::Get().FileExists(*QueuePath));
		TestTrue(TEXT("withdrawn: the opt-out marker exists"), IFileManager::Get().FileExists(*OptOutPath));
		FString State;
		FFileHelper::LoadFileToString(State, *StatePath);
		TestFalse(TEXT("withdrawn: the state file has no ID"), State.Contains(OldId) || State.Contains(TEXT("anonymous_id")) || State.Contains(TEXT("player_42")));
		Client->Track(TEXT("ignored"));
		TestEqual(TEXT("withdrawn: no events"), Client->GetQueueSize(), 0);
		Client->FinishShutdown();
	}
	{
		// The opt-out wins on the next launch, even when consent is not required.
		Config.bConsentRequired = false;
		TSharedRef<FManalensClient> Client = H.Start(Config);
		TestFalse(TEXT("the opt-out is kept"), Client->IsIdentityActive());
		Client->SetConsent(true);
		TestTrue(TEXT("a new opt-in starts again"), Client->IsIdentityActive());
		TestNotEqual(TEXT("with a new anonymous ID"), Client->GetAnonymousId(), OldId);
		TestFalse(TEXT("the opt-out marker is removed"), IFileManager::Get().FileExists(*OptOutPath));
	}
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensRetryTest, "Manalens.Retry.StatusCodes", Flags)
bool FManalensRetryTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("the collector refused"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	const TPair<int32, EManalensSendOutcome> Cases[] = {
		{ 0, EManalensSendOutcome::Retry }, { 200, EManalensSendOutcome::Sent }, { 202, EManalensSendOutcome::Sent },
		{ 400, EManalensSendOutcome::Drop }, { 401, EManalensSendOutcome::Drop }, { 403, EManalensSendOutcome::Drop }, { 404, EManalensSendOutcome::Drop },
		{ 408, EManalensSendOutcome::Retry }, { 413, EManalensSendOutcome::Drop }, { 429, EManalensSendOutcome::Retry },
		{ 500, EManalensSendOutcome::Retry }, { 502, EManalensSendOutcome::Retry }, { 503, EManalensSendOutcome::Retry },
	};
	for (const TPair<int32, EManalensSendOutcome>& Case : Cases)
	{
		TestEqual(FString::Printf(TEXT("HTTP %d"), Case.Key), static_cast<int32>(FManalensClient::ClassifyStatus(Case.Key)), static_cast<int32>(Case.Value));
	}
	TestEqual(TEXT("Retry-After seconds"), FManalensClient::ParseRetryAfter(TEXT("120"), 0.0), 120.0);
	TestEqual(TEXT("no Retry-After"), FManalensClient::ParseRetryAfter(TEXT(""), 0.0), 0.0);
	TestEqual(TEXT("bad Retry-After"), FManalensClient::ParseRetryAfter(TEXT("soon"), 0.0), 0.0);
	TestEqual(TEXT("Retry-After date"), FManalensClient::ParseRetryAfter(TEXT("Fri, 15 Jan 2027 08:01:00 GMT"), UnixOf(2027, 1, 15, 8, 0, 0)), 60.0);
	// P-2787: the refused events of an accepted batch.
	FString RejectedCodes;
	TestEqual(TEXT("rejected count"), FManalensClient::ParseRejected(TEXT("{\"accepted\":1,\"rejected\":2,\"errors\":[{\"index\":1,\"code\":\"INVALID_EVENT\"},{\"index\":2,\"code\":\"SESSION_ID_REQUIRED\"}]}"), RejectedCodes), 2);
	TestEqual(TEXT("rejected codes"), RejectedCodes, FString(TEXT("INVALID_EVENT, SESSION_ID_REQUIRED")));
	TestEqual(TEXT("rejected count in a cut body"), FManalensClient::ParseRejected(TEXT("{\"accepted\":1,\"rejected\": 60,\"errors\":[{\"index\":1,\"co"), RejectedCodes), 60);
	TestTrue(TEXT("no codes from a cut body"), RejectedCodes.IsEmpty());
	TestEqual(TEXT("older collector body"), FManalensClient::ParseRejected(TEXT("{\"accepted\":1}"), RejectedCodes), 0);
	TestEqual(TEXT("not JSON"), FManalensClient::ParseRejected(TEXT("<html>"), RejectedCodes), 0);

	FHarness H;
	H.Status = 503;
	TSharedRef<FManalensClient> Client = H.Start();
	Client->Update();
	const TArray<FString> FirstIds = H.Requests[0].MessageIds();
	TestEqual(TEXT("503: kept"), Client->GetQueueSize(), 2);
	TestTrue(TEXT("503: backoff of 1 to 2 seconds"), Client->GetBackoffUntil() >= H.Time + 1.0 && Client->GetBackoffUntil() <= H.Time + 2.0);
	Client->Flush();
	TestEqual(TEXT("no send during the backoff"), H.Requests.Num(), 1);

	H.Time += 3;
	H.Status = 429;
	H.RetryAfter = TEXT("600");
	Client->Flush();
	TestEqual(TEXT("429: sent again"), H.Requests.Num(), 2);
	TestTrue(TEXT("429: same message IDs"), H.Requests[1].MessageIds() == FirstIds);
	// P-2799: each send carries the device clock of that send, so the collector can correct the event times.
	TestEqual(TEXT("sent_at of the first send"), ParseObject(H.Requests[0].Body)->GetStringField(TEXT("sent_at")), FManalensClient::FormatTimestamp(H.Time - 3));
	TestEqual(TEXT("sent_at of the retry"), ParseObject(H.Requests[1].Body)->GetStringField(TEXT("sent_at")), FManalensClient::FormatTimestamp(H.Time));
	TestTrue(TEXT("429: Retry-After is honoured"), Client->GetBackoffUntil() >= H.Time + 600.0);

	H.Time += 601;
	H.Status = 408;
	H.RetryAfter.Reset();
	Client->Flush();
	TestEqual(TEXT("408: kept"), Client->GetQueueSize(), 2);

	H.Time += 60;
	H.Status = 400;
	H.ResponseBody = TEXT("{\"error\":{\"code\":\"INVALID_EVENT\",\"message\":\"bad\",\"refused\":[{\"index\":0,\"code\":\"SESSION_ID_REQUIRED\"}]}}");
	Client->Flush();
	TestEqual(TEXT("400: dropped"), Client->GetQueueSize(), 0);
	TestTrue(TEXT("400: reported"), H.SendFailed.Num() == 1 && H.SendFailed[0].Key == 400 && H.SendFailed[0].Value == 2);
	TestEqual(TEXT("400: backoff cleared"), Client->GetBackoffUntil(), 0.0);

	Client->Track(TEXT("after"));
	H.Status = 413;
	Client->Flush();
	TestEqual(TEXT("413: dropped"), Client->GetQueueSize(), 0);

	Client->Track(TEXT("ok"));
	H.Status = 202;
	Client->Flush();
	TestEqual(TEXT("202: sent"), Client->GetQueueSize(), 0);
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensKeyTest, "Manalens.Keys.SecretKeyRefused", Flags)
bool FManalensKeyTest::RunTest(const FString& Parameters)
{
	AddExpectedErrorPlain(TEXT("Publishable key refused"), EAutomationExpectedErrorFlags::Contains, 4);
	const FString Valid = SyntheticKey();
	TestTrue(TEXT("a well-formed Publishable key"), FManalensClient::PublishableKeyRefusal(Valid).IsEmpty());
	const FString SecretBody = FString(TEXT("mana_sk_")) + FString::ChrN(43, TEXT('T'));
	const FString Secret = SecretBody + FManalensClient::KeyChecksum(SecretBody);
	TestTrue(TEXT("a Secret key"), FManalensClient::PublishableKeyRefusal(Secret).Contains(TEXT("Secret key")));
	TestTrue(TEXT("an API key"), FManalensClient::PublishableKeyRefusal(TEXT("mana_api_test")).Contains(TEXT("API key")));
	TestFalse(TEXT("a placeholder"), FManalensClient::PublishableKeyRefusal(TEXT("mana_pk_test")).IsEmpty());
	TestFalse(TEXT("a bad checksum"), FManalensClient::PublishableKeyRefusal(Valid.LeftChop(1) + TEXT("0")).IsEmpty());
	TestFalse(TEXT("empty"), FManalensClient::PublishableKeyRefusal(TEXT("")).IsEmpty());

	for (const FString& Key : { Secret, FString(TEXT("mana_api_test")), FString(TEXT("mana_pk_test")), FString(TEXT("pk_live_123")) })
	{
		FHarness H;
		FManalensConfig Config = H.Config();
		Config.PublishableKey = Key;
		TSharedRef<FManalensClient> Client = H.Make();
		TestFalse(FString::Printf(TEXT("Init refuses %s"), *Key.Left(9)), Client->Init(Config));
		TestFalse(TEXT("the SDK stays off"), Client->IsIdentityActive());
		Client->Track(TEXT("level_start"));
		Client->Flush();
		TestEqual(TEXT("nothing is sent"), H.Requests.Num(), 0);
		TestFalse(TEXT("no state file"), IFileManager::Get().FileExists(*FPaths::Combine(H.Dir, FManalensClient::StateFileName)));
	}
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensErrorTest, "Manalens.Errors.RateLimits", Flags)
bool FManalensErrorTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("fingerprint as Godot and Unity"), FManalensClient::Fingerprint(TEXT("SaveFailed"), TEXT("Save failed for slot 3"), TEXT(""), TEXT("")), FString(TEXT("38d0e3d0139dea87")));
	TestEqual(TEXT("numbers and quoted values do not split a group"),
		FManalensClient::Fingerprint(TEXT(""), TEXT("Item 'sword' x 12 at 0x1F"), TEXT(""), TEXT("")),
		FManalensClient::Fingerprint(TEXT(""), TEXT("Item \"axe\" x 7 at 0xAB"), TEXT(""), TEXT("")));
	TestNotEqual(TEXT("the place in the code groups"),
		FManalensClient::Fingerprint(TEXT("E"), TEXT("m"), TEXT("A.cpp"), TEXT("F")),
		FManalensClient::Fingerprint(TEXT("E"), TEXT("m"), TEXT("B.cpp"), TEXT("F")));

	FHarness H;
	H.bHold = true;
	FManalensConfig Config = H.Config();
	Config.FlushAt = 50;
	Config.MaxQueue = 5000;
	Config.MaxAutoErrorsPerSession = 3;
	TSharedRef<FManalensClient> Client = H.Start(Config);
	auto CountErrors = [&Client]()
	{
		int32 Count = 0;
		for (const TSharedPtr<FJsonObject>& Event : Queued(*Client))
		{
			Count += NameOf(Event) == TEXT("error") ? 1 : 0;
		}
		return Count;
	};
	Client->TrackError(TEXT("Save failed"));
	H.Time += 0.5;
	Client->TrackError(TEXT("Save failed"));
	TestEqual(TEXT("the same fingerprint at most once a second"), CountErrors(), 1);
	H.Time += 0.6;
	Client->TrackError(TEXT("Save failed"));
	TestEqual(TEXT("after a second it is sent again"), CountErrors(), 2);

	H.Time += 20;
	for (int32 Index = 0; Index < 25; ++Index)
	{
		FManalensErrorOptions Options;
		Options.Fingerprint = FString::Printf(TEXT("f%d"), Index);
		Client->TrackError(TEXT("burst"), Options);
	}
	TestEqual(TEXT("at most 20 errors in 10 seconds"), CountErrors(), 22);

	H.Time += 20;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		H.Time += 1.5;
		Client->TrackLogError(TEXT("LogTemp"), FString::Printf(TEXT("engine error %c"), TEXT('a') + Index), false);
	}
	TestEqual(TEXT("at most MaxAutoErrorsPerSession log errors"), CountErrors(), 25);
	H.Time += 1.5;
	Client->TrackError(TEXT("manual after the cap"));
	TestEqual(TEXT("manual errors skip the session cap"), CountErrors(), 26);

	Client->BeforeSend = [](TSharedPtr<FJsonObject> Event) -> TSharedPtr<FJsonObject>
	{
		const TSharedPtr<FJsonObject> Props = Event->GetObjectField(TEXT("properties"));
		FString Message;
		if (Props->TryGetStringField(TEXT("message"), Message) && Message.Contains(TEXT("secret")))
		{
			return nullptr;
		}
		Props->SetStringField(TEXT("checked"), TEXT("yes"));
		return Event;
	};
	H.Time += 1.5;
	Client->TrackError(TEXT("a secret value"));
	TestEqual(TEXT("BeforeSend can drop an error"), CountErrors(), 26);
	H.Time += 1.5;
	Client->TrackError(TEXT("kept"));
	TestEqual(TEXT("BeforeSend can change an error"), Queued(*Client).Last()->GetObjectField(TEXT("properties"))->GetStringField(TEXT("checked")), FString(TEXT("yes")));
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensBeforeSendTest, "Manalens.Errors.BeforeSendOnEveryEvent", Flags)
bool FManalensBeforeSendTest::RunTest(const FString& Parameters)
{
	// G-2527: BeforeSend gets every event, can sample and scrub, and cannot break the envelope.
	FHarness H;
	H.bHold = true;
	TSharedRef<FManalensClient> Client = H.Make();
	TArray<FString> Seen;
	int32 Jumps = 0;
	FString Mode;
	FManalensClient* Raw = &Client.Get();
	Client->BeforeSend = [&Seen, &Jumps, &Mode, Raw](TSharedPtr<FJsonObject> Event) -> TSharedPtr<FJsonObject>
	{
		FString Type;
		FString Name;
		Event->TryGetStringField(TEXT("type"), Type);
		Event->TryGetStringField(TEXT("event"), Name);
		Seen.Add(Type + TEXT(":") + Name);
		if (Mode == TEXT("break_id"))
		{
			Event->SetStringField(TEXT("message_id"), TEXT("changed"));
			Event->GetObjectField(TEXT("properties"))->SetNumberField(TEXT("level"), 99);
			return Event;
		}
		if (Mode == TEXT("no_anonymous_id"))
		{
			Event->RemoveField(TEXT("anonymous_id"));
			return Event;
		}
		if (Mode == TEXT("break_app"))
		{
			Event->GetObjectField(TEXT("context"))->GetObjectField(TEXT("app"))->RemoveField(TEXT("platform"));
			return Event;
		}
		if (Mode == TEXT("nested"))
		{
			Raw->Track(TEXT("from_the_hook"));
			return Event;
		}
		if (Name == TEXT("player_jump") && Jumps++ % 10 != 0)
		{
			return nullptr; // keep 1 in 10
		}
		Event->GetObjectField(TEXT("properties"))->RemoveField(TEXT("email"));
		const TSharedPtr<FJsonObject>* Traits = nullptr;
		if (Event->TryGetObjectField(TEXT("traits"), Traits))
		{
			(*Traits)->RemoveField(TEXT("email"));
		}
		return Event;
	};
	Client->Init(H.Config());
	TestTrue(TEXT("session_start goes through the hook"), Seen.Contains(TEXT("track:session_start")));
	TestTrue(TEXT("game_open goes through the hook"), Seen.Contains(TEXT("track:game_open")));
	for (int32 Index = 0; Index < 20; ++Index)
	{
		Client->Track(TEXT("player_jump"));
	}
	TestEqual(TEXT("sampling keeps 2 of 20"), Names(Queued(*Client)).FilterByPredicate([](const FString& Name) { return Name == TEXT("player_jump"); }).Num(), 2);
	TSharedPtr<FJsonObject> Signup = MakeShared<FJsonObject>();
	Signup->SetStringField(TEXT("email"), TEXT("ana@example.com"));
	Signup->SetNumberField(TEXT("level"), 1);
	Client->Track(TEXT("signup"), Signup);
	TSharedPtr<FJsonObject> Last = Queued(*Client).Last();
	TestFalse(TEXT("the email is scrubbed"), Last->GetObjectField(TEXT("properties"))->HasField(TEXT("email")));
	TestTrue(TEXT("other properties stay"), Last->GetObjectField(TEXT("properties"))->HasField(TEXT("level")));
	TSharedPtr<FJsonObject> Traits = MakeShared<FJsonObject>();
	Traits->SetStringField(TEXT("email"), TEXT("ana@example.com"));
	Traits->SetStringField(TEXT("plan"), TEXT("supporter"));
	Client->Identify(TEXT("player_42"), Traits);
	Last = Queued(*Client).Last();
	TestEqual(TEXT("identify goes through the hook"), NameOf(Last), FString(TEXT("identify")));
	TestFalse(TEXT("the trait email is scrubbed"), Last->GetObjectField(TEXT("traits"))->HasField(TEXT("email")));
	for (const TCHAR* Broken : { TEXT("break_id"), TEXT("no_anonymous_id"), TEXT("break_app") })
	{
		Mode = Broken;
		H.Time += 2;
		TSharedPtr<FJsonObject> Level = MakeShared<FJsonObject>();
		Level->SetNumberField(TEXT("level"), 3);
		Client->Track(TEXT("level_start"), Level);
		Last = Queued(*Client).Last();
		TestEqual(FString(Broken) + TEXT(": the original event is sent"), Last->GetObjectField(TEXT("properties"))->GetNumberField(TEXT("level")), 3.0);
		TestNotEqual(FString(Broken) + TEXT(": the message ID is kept"), Last->GetStringField(TEXT("message_id")), FString(TEXT("changed")));
		TestTrue(FString(Broken) + TEXT(": the anonymous ID is kept"), Last->HasField(TEXT("anonymous_id")));
		TestTrue(FString(Broken) + TEXT(": context.app is kept"), Last->GetObjectField(TEXT("context"))->GetObjectField(TEXT("app"))->HasField(TEXT("platform")));
	}
	Mode = TEXT("nested");
	Client->Track(TEXT("level_start"));
	TestFalse(TEXT("an event recorded inside the hook is dropped"), Names(Queued(*Client)).Contains(TEXT("from_the_hook")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensIdentityTest, "Manalens.Identity.LinkUrlAndReset", Flags)
bool FManalensIdentityTest::RunTest(const FString& Parameters)
{
	FHarness H;
	H.bHold = true;
	H.bSequentialIds = false;
	TSharedRef<FManalensClient> Client = H.Start();
	const FString Id = Client->GetAnonymousId();
	const FString Stamp = FString::Printf(TEXT("%lld"), static_cast<long long>(H.Time));
	TestEqual(TEXT("LinkUrl adds mana_hid"), Client->LinkUrl(TEXT("https://example.com/wiki")), FString::Printf(TEXT("https://example.com/wiki?mana_hid=%s.%s"), *Id, *Stamp));
	TestEqual(TEXT("LinkUrl keeps the query and the fragment"), Client->LinkUrl(TEXT("https://example.com/?a=1#top")), FString::Printf(TEXT("https://example.com/?a=1&mana_hid=%s.%s#top"), *Id, *Stamp));

	Client->Identify(TEXT("player_42"), Object({ { TEXT("plan"), Str(TEXT("supporter")) } }));
	TestEqual(TEXT("user ID"), Client->GetUserId(), FString(TEXT("player_42")));
	const FString Session = Client->GetSessionId();
	Client->Reset();
	TestTrue(TEXT("Reset clears the user ID"), Client->GetUserId().IsEmpty());
	TestNotEqual(TEXT("Reset makes a new anonymous ID"), Client->GetAnonymousId(), Id);
	TestNotEqual(TEXT("Reset starts a new session"), Client->GetSessionId(), Session);
	const TArray<TSharedPtr<FJsonObject>> Events = Queued(*Client);
	TestEqual(TEXT("events"), FString::Join(Names(Events), TEXT(",")), FString(TEXT("session_start,game_open,identify,session_end,session_start")));
	TestEqual(TEXT("identify carries the traits"), Events[2]->GetObjectField(TEXT("traits"))->GetStringField(TEXT("plan")), FString(TEXT("supporter")));
	return true;
}

// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensPropertiesTest, "Manalens.Properties.Limits", Flags)
bool FManalensPropertiesTest::RunTest(const FString& Parameters)
{
	AddExpectedMessagePlain(TEXT("not a scalar"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("over the limit"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	FHarness H;
	H.bHold = true;
	TSharedRef<FManalensClient> Client = H.Start();

	TSharedPtr<FJsonObject> Many = MakeShared<FJsonObject>();
	for (int32 Index = 0; Index < 60; ++Index)
	{
		Many->SetNumberField(FString::Printf(TEXT("k%02d"), Index), Index);
	}
	Client->Track(TEXT("many"), Many);
	TestEqual(TEXT("at most 50 keys"), Queued(*Client).Last()->GetObjectField(TEXT("properties"))->Values.Num(), 50);

	TSharedPtr<FJsonObject> Big = MakeShared<FJsonObject>();
	for (int32 Index = 0; Index < 12; ++Index)
	{
		Big->SetStringField(FString::Printf(TEXT("text%d"), Index), FString::ChrN(1000, TEXT('x')));
	}
	Client->Track(TEXT("big"), Big);
	const TSharedPtr<FJsonObject> BigProps = Queued(*Client).Last()->GetObjectField(TEXT("properties"));
	TestEqual(TEXT("at most 8 KB"), BigProps->Values.Num(), 8);

	TSharedPtr<FJsonObject> Mixed = MakeShared<FJsonObject>();
	Mixed->SetObjectField(TEXT("nested"), MakeShared<FJsonObject>());
	Mixed->SetStringField(TEXT("long"), FString::ChrN(2000, TEXT('y')));
	Mixed->SetStringField(TEXT(""), TEXT("empty key"));
	Client->Track(TEXT("mixed"), Mixed);
	const TSharedPtr<FJsonObject> MixedProps = Queued(*Client).Last()->GetObjectField(TEXT("properties"));
	TestFalse(TEXT("nested objects are dropped"), MixedProps->HasField(TEXT("nested")));
	TestEqual(TEXT("strings are cut to 1024 characters"), MixedProps->GetStringField(TEXT("long")).Len(), 1024);
	TestEqual(TEXT("empty keys are dropped"), MixedProps->Values.Num(), 1);

	AddExpectedMessagePlain(TEXT("event name must be 1 to 128"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Client->Track(FString::ChrN(129, TEXT('e')));
	TestEqual(TEXT("a long event name is refused"), NameOf(Queued(*Client).Last()), FString(TEXT("mixed")));
	return true;
}

// ---------------------------------------------------------------------------

namespace ManalensTests
{
	TSharedPtr<FJsonObject> LastError(const FManalensClient& Client)
	{
		TSharedPtr<FJsonObject> Found;
		for (const TSharedPtr<FJsonObject>& Event : Queued(Client))
		{
			if (NameOf(Event) == TEXT("error"))
			{
				Found = Event;
			}
		}
		return Found;
	}

	FString PropOf(const TSharedPtr<FJsonObject>& Event, const TCHAR* Key)
	{
		FString Value;
		if (Event.IsValid())
		{
			Event->GetObjectField(TEXT("properties"))->TryGetStringField(Key, Value);
		}
		return Value;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensConsentHistoryTest, "Manalens.Consent.WithdrawClearsErrorHistory", Flags)
bool FManalensConsentHistoryTest::RunTest(const FString& Parameters)
{
	FHarness H;
	H.bHold = true;
	TSharedRef<FManalensClient> Client = H.Start();
	Client->Track(TEXT("secret_level"));
	Client->TrackError(TEXT("Save failed"));
	Client->TrackError(TEXT("Save failed")); // dropped by the 1-second limit: errors_dropped 1
	Client->SetConsent(false);
	Client->SetConsent(true);
	Client->TrackError(TEXT("Save failed"));
	const TSharedPtr<FJsonObject> Error = LastError(*Client);
	if (!TestTrue(TEXT("the same fingerprint is not held back by the old limits"), Error.IsValid()))
	{
		return false;
	}
	const FString Breadcrumbs = PropOf(Error, TEXT("breadcrumbs"));
	TestFalse(TEXT("no event name from before the withdrawal"), Breadcrumbs.Contains(TEXT("secret_level")));
	TestEqual(TEXT("only the events of the new identity"), Breadcrumbs, FString(TEXT("session_start,game_open")));
	Client->BeginShutdown();
	const TSharedPtr<FJsonObject> End = Queued(*Client).Last();
	TestEqual(TEXT("session_end"), NameOf(End), FString(TEXT("session_end")));
	TestFalse(TEXT("no errors_dropped from before the withdrawal"), End->GetObjectField(TEXT("properties"))->HasField(TEXT("errors_dropped")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensResetHistoryTest, "Manalens.Identity.ResetClearsErrorHistory", Flags)
bool FManalensResetHistoryTest::RunTest(const FString& Parameters)
{
	FHarness H;
	H.bHold = true;
	TSharedRef<FManalensClient> Client = H.Start();
	Client->Identify(TEXT("player_42"));
	Client->Track(TEXT("secret_level"));
	Client->TrackError(TEXT("Save failed"));
	Client->TrackError(TEXT("Save failed")); // dropped: errors_dropped 1
	Client->Reset();
	TArray<TSharedPtr<FJsonObject>> Events = Queued(*Client);
	const TSharedPtr<FJsonObject> End = Events[Events.Num() - 2];
	TestEqual(TEXT("session_end of the old session"), NameOf(End), FString(TEXT("session_end")));
	TestEqual(TEXT("the old session reports its dropped errors"), End->GetObjectField(TEXT("properties"))->GetNumberField(TEXT("errors_dropped")), 1.0);
	Client->TrackError(TEXT("Save failed"));
	const TSharedPtr<FJsonObject> Error = LastError(*Client);
	TestNotEqual(TEXT("a new error after Reset"), Error->GetStringField(TEXT("message_id")), Events.Last()->GetStringField(TEXT("message_id")));
	TestEqual(TEXT("in the new session"), Error->GetStringField(TEXT("session_id")), Client->GetSessionId());
	const FString Breadcrumbs = PropOf(Error, TEXT("breadcrumbs"));
	TestFalse(TEXT("no event name from before Reset"), Breadcrumbs.Contains(TEXT("secret_level")) || Breadcrumbs.Contains(TEXT("session_end")));
	TestEqual(TEXT("only the new session"), Breadcrumbs, FString(TEXT("session_start")));
	TestFalse(TEXT("no user ID on the new error"), Error->HasField(TEXT("user_id")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensLogCleaningTest, "Manalens.Errors.LogMessageCleaning", Flags)
bool FManalensLogCleaningTest::RunTest(const FString& Parameters)
{
	const TArray<FString> Home = { TEXT("/Users/anna/"), TEXT("/Users/anna/Documents/") };
	auto Clean = [&Home](const TCHAR* Text) { return FManalensClient::CleanLogMessage(Text, Home); };

	// Home folders
	TestEqual(TEXT("home dir"), Clean(TEXT("Failed to open /Users/anna/Saved/x.sav")), FString(TEXT("Failed to open ~/Saved/x.sav")));
	TestEqual(TEXT("home dir with back slashes"), Clean(TEXT("at \\Users\\anna\\Saved")), FString(TEXT("at ~\\Saved")));
	TestEqual(TEXT("/Users/<name>"), Clean(TEXT("read /Users/dave/Library/a.ini")), FString(TEXT("read ~/Library/a.ini")));
	TestEqual(TEXT("/home/<name>"), Clean(TEXT("read /home/bob/game/log.txt")), FString(TEXT("read ~/game/log.txt")));
	TestEqual(TEXT("C:\\Users\\<name>"), Clean(TEXT("read C:\\Users\\Carol\\AppData\\x.ini")), FString(TEXT("read ~\\AppData\\x.ini")));
	TestEqual(TEXT("D:/Users/<name>"), Clean(TEXT("read d:/users/Carol/x.ini")), FString(TEXT("read ~/x.ini")));
	const FString Real = FManalensClient::CleanLogMessage(FPlatformProcess::UserHomeDir() + FString(TEXT("Saved/a.sav")));
	TestFalse(TEXT("the real home dir is removed"), Real.Contains(FString(FPlatformProcess::UserHomeDir()).LeftChop(1)));
	TestTrue(TEXT("the real home dir becomes ~"), Real.StartsWith(TEXT("~")));

	// URLs
	TestEqual(TEXT("URL userinfo, query, and fragment"), Clean(TEXT("GET https://user:pass@api.example.com/v1/items?id=3&sig=abc#frag failed")), FString(TEXT("GET https://api.example.com/v1/items failed")));
	TestEqual(TEXT("URL without a path"), Clean(TEXT("connect wss://u@play.example.com?ticket=1")), FString(TEXT("connect wss://play.example.com")));
	TestEqual(TEXT("plain URL unchanged"), Clean(TEXT("GET https://api.example.com/v1/items")), FString(TEXT("GET https://api.example.com/v1/items")));

	// Emails
	TestEqual(TEXT("email"), Clean(TEXT("No account for jane.doe+x@mail.example.com.")), FString(TEXT("No account for [email].")));
	TestEqual(TEXT("not an email"), Clean(TEXT("Actor@Level failed")), FString(TEXT("Actor@Level failed")));

	// Key values
	TestEqual(TEXT("key=, token=, secret=, password="), Clean(TEXT("api_key=abc123 token=xyz&secret=s3 PASSWORD=hunter2")), FString(TEXT("api_key=[redacted] token=[redacted]&secret=[redacted] PASSWORD=[redacted]")));
	TestEqual(TEXT("authorization:"), Clean(TEXT("Authorization: Bearer abc.def")), FString(TEXT("Authorization: [redacted]")));
	TestEqual(TEXT("bearer"), Clean(TEXT("sent bearer xyz.123 to the server")), FString(TEXT("sent bearer [redacted] to the server")));
	TestEqual(TEXT("quoted values"), Clean(TEXT("password=\"hunter2 two\" and key='a b'")), FString(TEXT("password=\"[redacted]\" and key='[redacted]'")));
	TestEqual(TEXT("JSON token and api_key"), Clean(TEXT("{\"token\": \"abc def\", \"api_key\":\"k1\", \"user\": 3}")), FString(TEXT("{\"token\": \"[redacted]\", \"api_key\":\"[redacted]\", \"user\": 3}")));
	TestEqual(TEXT("JSON password and secret"), Clean(TEXT("{\"password\": \"p w\", \"secret\": 42}")), FString(TEXT("{\"password\": \"[redacted]\", \"secret\": [redacted]}")));
	TestEqual(TEXT("header forms"), Clean(TEXT("token: abc123 password : hunter2")), FString(TEXT("token: [redacted] password : [redacted]")));
	TestEqual(TEXT("JSON Authorization"), Clean(TEXT("\"Authorization\": \"Bearer abc\"")), FString(TEXT("\"Authorization\": \"[redacted]\"")));
	TestEqual(TEXT("token: Bearer"), Clean(TEXT("header token: Bearer abc")), FString(TEXT("header token: [redacted] [redacted]")));
	TestEqual(TEXT("JWT"), Clean(TEXT("jwt eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxIn0.c2ln. failed")), FString(TEXT("jwt [redacted]. failed")));
	TestEqual(TEXT("base64 with +"), Clean(TEXT("blob aGVsbG8gd29ybGQgaGVsbG8+d29ybGQ end")), FString(TEXT("blob [redacted] end")));
	TestEqual(TEXT("base64 with ="), Clean(TEXT("blob aGVsbG8gd29ybGQgaGVsbG8vd29ybGQ= end")), FString(TEXT("blob [redacted] end")));
	TestEqual(TEXT("a path stays"), Clean(TEXT("load /Game/Maps/boss_room_final/arena.umap failed")), FString(TEXT("load /Game/Maps/boss_room_final/arena.umap failed")));

	// Long tokens
	TestEqual(TEXT("24 characters"), Clean(TEXT("session ABCDEFGHIJKLMNOPQRSTUV_- expired")), FString(TEXT("session [redacted] expired")));
	TestEqual(TEXT("23 characters stay"), Clean(TEXT("session ABCDEFGHIJKLMNOPQRSTUVW expired")), FString(TEXT("session ABCDEFGHIJKLMNOPQRSTUVW expired")));
	TestEqual(TEXT("a UUID"), Clean(TEXT("id 6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a4b")), FString(TEXT("id [redacted]")));

	TestEqual(TEXT("a normal message is unchanged"), Clean(TEXT("Accessed None trying to read property Player")), FString(TEXT("Accessed None trying to read property Player")));

	// The queued event: the cleaned message, and one group for two user names.
	FHarness H;
	H.bHold = true;
	TSharedRef<FManalensClient> Client = H.Start();
	Client->TrackLogError(TEXT("LogTemp"), TEXT("Cannot load /Users/anna/Saved/a.sav for anna@example.com"), false);
	const TSharedPtr<FJsonObject> First = LastError(*Client);
	TestEqual(TEXT("the message is cleaned"), PropOf(First, TEXT("message")), FString(TEXT("Cannot load ~/Saved/a.sav for [email]")));
	H.Time += 2;
	Client->TrackLogError(TEXT("LogTemp"), TEXT("Cannot load /home/bob/Saved/a.sav for bob@example.org"), false);
	const TSharedPtr<FJsonObject> Second = LastError(*Client);
	TestNotEqual(TEXT("a second event"), Second->GetStringField(TEXT("message_id")), First->GetStringField(TEXT("message_id")));
	TestEqual(TEXT("the same fingerprint on two machines"), PropOf(Second, TEXT("fingerprint")), PropOf(First, TEXT("fingerprint")));

	// A manual error is the caller's text: unchanged.
	H.Time += 2;
	Client->TrackError(TEXT("Mail support@example.com about /Users/anna/x"));
	TestEqual(TEXT("TrackError text is unchanged"), PropOf(LastError(*Client), TEXT("message")), FString(TEXT("Mail support@example.com about /Users/anna/x")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensSkippedCategoriesTest, "Manalens.Errors.SkippedLogCategories", Flags)
bool FManalensSkippedCategoriesTest::RunTest(const FString& Parameters)
{
	const TCHAR* Skipped[] = { TEXT("LogOnline"), TEXT("LogOnlineIdentity"), TEXT("LogOnlineSession"), TEXT("LogEOS"), TEXT("LogEOSSDK"), TEXT("LogSteamShared"), TEXT("LogSlate"), TEXT("LogHttp"), TEXT("LogManalens") };
	FHarness H;
	H.bHold = true;
	TSharedRef<FManalensClient> Client = H.Start();
	for (const TCHAR* Category : Skipped)
	{
		TestTrue(FString::Printf(TEXT("%s is skipped"), Category), FManalensClient::IsSkippedLogCategory(Category));
		H.Time += 2;
		Client->TrackLogError(Category, FString::Printf(TEXT("login failed for %s"), Category), false);
	}
	TestFalse(TEXT("no error event from a skipped category"), LastError(*Client).IsValid());
	for (const TCHAR* Category : { TEXT("LogTemp"), TEXT("LogScript"), TEXT("LogBlueprintUserMessages"), TEXT("LogHttpServer2") })
	{
		TestFalse(FString::Printf(TEXT("%s is captured"), Category), FManalensClient::IsSkippedLogCategory(Category));
	}
	Client->TrackLogError(TEXT("LogTemp"), TEXT("kept"), false);
	TestEqual(TEXT("LogTemp is captured"), PropOf(LastError(*Client), TEXT("message")), FString(TEXT("kept")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensEditorCategoriesTest, "Manalens.Errors.EditorLogCategories", Flags)
bool FManalensEditorCategoriesTest::RunTest(const FString& Parameters)
{
	// Seen in a real Play In Editor session (P-2792): these were sent as game errors.
	for (const TCHAR* Category : { TEXT("LogBlueprint"), TEXT("LogEditor"), TEXT("LogEditorViewport"), TEXT("LogUnrealEd"), TEXT("LogAssetTools"),
		TEXT("LogContentBrowser"), TEXT("LogDerivedDataCache"), TEXT("LogSourceControl"), TEXT("LogShaderCompilers"), TEXT("LogPython"),
		TEXT("LogLiveCoding"), TEXT("LogCook"), TEXT("LogAutomationController") })
	{
		TestTrue(FString::Printf(TEXT("%s is an editor category"), Category), FManalensClient::IsEditorLogCategory(Category));
	}
	// The game's own messages, also those of Blueprints, stay captured.
	for (const TCHAR* Category : { TEXT("LogTemp"), TEXT("LogScript"), TEXT("LogBlueprintUserMessages"), TEXT("LogStreaming"), TEXT("LogNet"), TEXT("LogAudio") })
	{
		TestFalse(FString::Printf(TEXT("%s is not an editor category"), Category), FManalensClient::IsEditorLogCategory(Category));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
