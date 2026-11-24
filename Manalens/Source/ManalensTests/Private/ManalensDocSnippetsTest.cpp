// Copyright (c) 2026 Manalens. MIT License.
//
// The C++ examples of docs/sdks/unreal.mdx and README.md. The test compiles
// them, and runs them on a subsystem that is not initialized, where
// every call must be a safe no-op.

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "ManalensSubsystem.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace ManalensDocSnippets
{
	void Run(UManalensSubsystem* Manalens, bool bOpenUrl)
	{
		// Track events
		Manalens->Track(TEXT("level_complete"), { { TEXT("mode"), TEXT("story") } }, { { TEXT("level"), 3 }, { TEXT("score"), 1200 } }, {});
		Manalens->Track(TEXT("level_complete"), {}, { { TEXT("level"), 3 }, { TEXT("score"), 1200 } }, {});

		// Identify players
		Manalens->Identify(TEXT("player_42"), { { TEXT("plan"), TEXT("supporter") } }, {}, {});
		Manalens->Reset(); // on sign out

		// Open your website from the game
		if (bOpenUrl)
		{
			FPlatformProcess::LaunchURL(*Manalens->LinkUrl(TEXT("https://example.com/wiki")), nullptr, nullptr);
		}

		// Error tracking
		FManalensErrorOptions Options;
		Options.ErrorType = TEXT("SaveFailed");
		Options.Severity = EManalensSeverity::Critical;
		Options.bHandled = false;
		Manalens->TrackError(TEXT("Save failed"), Options);
		Manalens->SetBeforeSend([](TSharedPtr<FJsonObject> Props) { return Props; });

		// Consent
		Manalens->SetConsent(true);
		Manalens->SetConsent(false);
		Manalens->Flush();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FManalensDocSnippetsTest, "Manalens.Docs.SnippetsCompileAndAreSafeWhenOff", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FManalensDocSnippetsTest::RunTest(const FString& Parameters)
{
	// A subsystem that the engine did not initialize: the SDK is off.
	UGameInstance* GameInstance = NewObject<UGameInstance>(GetTransientPackage());
	UManalensSubsystem* Off = NewObject<UManalensSubsystem>(GameInstance);
	ManalensDocSnippets::Run(Off, false);
	TestFalse(TEXT("off when the engine did not initialize it"), Off->IsActive());
	TestEqual(TEXT("LinkUrl returns the URL unchanged when off"), Off->LinkUrl(TEXT("https://example.com")), FString(TEXT("https://example.com")));
	TestTrue(TEXT("no anonymous ID when off"), Off->GetAnonymousId().IsEmpty());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
