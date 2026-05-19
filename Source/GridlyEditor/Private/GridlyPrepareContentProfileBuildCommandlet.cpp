// Copyright Epic Games, Inc. All Rights Reserved.

#include "GridlyPrepareContentProfileBuildCommandlet.h"

#include "GridlyGameSettings.h"
#include "GridlyMaterializeContentProfileCommandlet.h"

DEFINE_LOG_CATEGORY_STATIC(LogGridlyPrepareContentProfileBuildCommandlet, Log, All);

UGridlyPrepareContentProfileBuildCommandlet::UGridlyPrepareContentProfileBuildCommandlet(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UGridlyPrepareContentProfileBuildCommandlet::Main(const FString& Params)
{
	FString LocalizationProfile;
	if (!ResolveLocalizationProfile(Params, LocalizationProfile))
	{
		UE_LOG(LogGridlyPrepareContentProfileBuildCommandlet, Error,
			TEXT("Missing localization profile. Pass -LocalizationProfile=Demo, -Profile=Demo, or a profile token."));
		return 1;
	}

	UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	if (!GameSettings)
	{
		UE_LOG(LogGridlyPrepareContentProfileBuildCommandlet, Error, TEXT("Unable to load Gridly game settings."));
		return 1;
	}

	GameSettings->ActiveContentProfile = LocalizationProfile;
	GameSettings->SaveConfig();

	FString MaterializeParams = Params;
	if (!MaterializeParams.Contains(TEXT("LocalizationProfile=")) && !MaterializeParams.Contains(TEXT("Profile=")))
	{
		MaterializeParams += FString::Printf(TEXT(" -LocalizationProfile=%s"), *LocalizationProfile);
	}
	if (!MaterializeParams.Contains(TEXT("ApplyToCanonicalForBuild")))
	{
		MaterializeParams += TEXT(" -ApplyToCanonicalForBuild");
	}

	UE_LOG(LogGridlyPrepareContentProfileBuildCommandlet, Warning,
		TEXT("Preparing content profile build overlay for '%s'. Canonical files in this workspace will be backed up under Saved/GridlyContentProfileBackups and overwritten for the current build."),
		*LocalizationProfile);

	UGridlyMaterializeContentProfileCommandlet* MaterializeCommandlet =
		NewObject<UGridlyMaterializeContentProfileCommandlet>();
	return MaterializeCommandlet ? MaterializeCommandlet->Main(MaterializeParams) : 1;
}

bool UGridlyPrepareContentProfileBuildCommandlet::ResolveLocalizationProfile(const FString& Params,
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
