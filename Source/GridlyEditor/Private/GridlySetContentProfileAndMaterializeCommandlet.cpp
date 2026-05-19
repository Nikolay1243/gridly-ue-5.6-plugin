// Copyright Epic Games, Inc. All Rights Reserved.

#include "GridlySetContentProfileAndMaterializeCommandlet.h"

#include "GridlyGameSettings.h"
#include "GridlyMaterializeContentProfileCommandlet.h"

DEFINE_LOG_CATEGORY_STATIC(LogGridlySetContentProfileAndMaterializeCommandlet, Log, All);

UGridlySetContentProfileAndMaterializeCommandlet::UGridlySetContentProfileAndMaterializeCommandlet(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UGridlySetContentProfileAndMaterializeCommandlet::Main(const FString& Params)
{
	FString LocalizationProfile;
	if (!ResolveLocalizationProfile(Params, LocalizationProfile))
	{
		UE_LOG(LogGridlySetContentProfileAndMaterializeCommandlet, Error,
			TEXT("Missing localization profile. Pass -LocalizationProfile=Demo, -Profile=Demo, or a profile token."));
		return 1;
	}

	UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	if (!GameSettings)
	{
		UE_LOG(LogGridlySetContentProfileAndMaterializeCommandlet, Error,
			TEXT("Unable to load Gridly game settings."));
		return 1;
	}

	GameSettings->ActiveContentProfile = LocalizationProfile;
	GameSettings->SaveConfig();

	UE_LOG(LogGridlySetContentProfileAndMaterializeCommandlet, Display,
		TEXT("Set ActiveContentProfile to '%s'. Starting content profile materialization."), *LocalizationProfile);

	UGridlyMaterializeContentProfileCommandlet* MaterializeCommandlet =
		NewObject<UGridlyMaterializeContentProfileCommandlet>();
	if (!MaterializeCommandlet)
	{
		UE_LOG(LogGridlySetContentProfileAndMaterializeCommandlet, Error,
			TEXT("Failed to create GridlyMaterializeContentProfileCommandlet."));
		return 1;
	}

	return MaterializeCommandlet->Main(Params);
}

bool UGridlySetContentProfileAndMaterializeCommandlet::ResolveLocalizationProfile(const FString& Params,
	FString& OutLocalizationProfile) const
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamVals;
	UCommandlet::ParseCommandLine(*Params, Tokens, Switches, ParamVals);

	OutLocalizationProfile = ParamVals.FindRef(TEXT("LocalizationProfile"));
	if (OutLocalizationProfile.IsEmpty())
	{
		OutLocalizationProfile = ParamVals.FindRef(TEXT("Profile"));
	}
	if (OutLocalizationProfile.IsEmpty())
	{
		OutLocalizationProfile = ParamVals.FindRef(TEXT("ContentProfile"));
	}
	if (OutLocalizationProfile.IsEmpty() && Tokens.Num() > 0)
	{
		OutLocalizationProfile = Tokens[0];
	}

	OutLocalizationProfile.TrimStartAndEndInline();
	return !OutLocalizationProfile.IsEmpty();
}
