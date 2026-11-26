// Copyright (c) 2026 Manalens. MIT License.

#include "ManalensSettings.h"

UManalensSettings::UManalensSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("Manalens");
}

FString UManalensSettings::StorefrontValue(EManalensStorefront Value)
{
	switch (Value)
	{
	case EManalensStorefront::Steam: return TEXT("steam");
	case EManalensStorefront::Itch: return TEXT("itch");
	case EManalensStorefront::Epic: return TEXT("epic");
	case EManalensStorefront::Gog: return TEXT("gog");
	case EManalensStorefront::AppStore: return TEXT("app_store");
	case EManalensStorefront::Play: return TEXT("play");
	case EManalensStorefront::Psn: return TEXT("psn");
	case EManalensStorefront::Xbox: return TEXT("xbox");
	case EManalensStorefront::Nintendo: return TEXT("nintendo");
	case EManalensStorefront::Other: return TEXT("other");
	default: return FString();
	}
}
