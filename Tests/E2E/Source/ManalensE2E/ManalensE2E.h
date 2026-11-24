// Copyright (c) 2026 Manalens. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ManalensE2E.generated.h"

class UManalensSubsystem;

/** Drives one game launch for scripts/test-unreal-sdk-local.mjs. Created only with -ManalensE2E. */
UCLASS()
class UManalensE2ESubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	bool OnTick(float DeltaTime);
	void Finish(const FString& Result);

	UPROPERTY()
	TObjectPtr<UManalensSubsystem> Manalens;

	FString RunId;
	FString ResultPath;
	FTSTicker::FDelegateHandle TickHandle;
	double Elapsed = 0.0;
	bool bTracked = false;
	bool bFinished = false;
};
