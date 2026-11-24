// Copyright (c) 2026 Manalens. MIT License.

#pragma once

#include "CoreMinimal.h"
#include "ManalensTypes.generated.h"

/** The store the build is sold on. Sent as context.app.storefront. */
UENUM(BlueprintType)
enum class EManalensStorefront : uint8
{
	None UMETA(DisplayName = "Not set"),
	Steam UMETA(DisplayName = "Steam"),
	Itch UMETA(DisplayName = "itch.io"),
	Epic UMETA(DisplayName = "Epic Games Store"),
	Gog UMETA(DisplayName = "GOG"),
	AppStore UMETA(DisplayName = "App Store"),
	Play UMETA(DisplayName = "Google Play"),
	Other UMETA(DisplayName = "Other"),
};

/** How bad an error is. Sent as the severity property of an error event. */
UENUM(BlueprintType)
enum class EManalensSeverity : uint8
{
	Debug,
	Info,
	Warning,
	Error,
	Critical,
};

/** Optional details of an error for Track Error. Every field can stay empty. */
USTRUCT(BlueprintType)
struct MANALENS_API FManalensErrorOptions
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	EManalensSeverity Severity = EManalensSeverity::Error;

	/** The error class, for example SaveFailed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	FString ErrorType;

	/** false when the game could not recover. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	bool bHandled = true;

	/** A stack trace text (cut to 1024 characters). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	FString Stack;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	FString File;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	int32 Line = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	FString Function;

	/** Your own grouping key. By default a hash of the error type and the place in the code, or of the message without numbers. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	FString Fingerprint;

	/** More text properties, for example level or mode. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Manalens")
	TMap<FString, FString> Properties;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FManalensSendFailed, int32, Status, int32, Dropped);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FManalensFlushed, int32, Sent, bool, bOk);
