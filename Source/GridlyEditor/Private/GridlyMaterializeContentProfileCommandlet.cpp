// Copyright Epic Games, Inc. All Rights Reserved.

#include "GridlyMaterializeContentProfileCommandlet.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "GridlyCultureConverter.h"
#include "GridlyGameSettings.h"
#include "GridlyLocalizedTextConverter.h"
#include "HttpModule.h"
#include "HttpManager.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Internationalization/StringTable.h"
#include "Internationalization/StringTableCore.h"
#include "JsonObjectConverter.h"
#include "LocalizationCommandletExecution.h"
#include "LocalizationConfigurationScript.h"
#include "LocalizationSettings.h"
#include "LocalizationTargetTypes.h"
#include "Modules/ModuleManager.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogGridlyMaterializeContentProfileCommandlet, Log, All);

namespace
{
bool RunLocalizationCommandletTasks(const TArray<LocalizationCommandletExecution::FTask>& Tasks);
}

UGridlyMaterializeContentProfileCommandlet::UGridlyMaterializeContentProfileCommandlet(
	const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	IsClient = false;
	IsEditor = true;
	IsServer = false;
	LogToConsole = true;
}

int32 UGridlyMaterializeContentProfileCommandlet::Main(const FString& Params)
{
	UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display, TEXT("=== Gridly content profile materialization ==="));

	FMaterializeOptions Options;
	if (!ParseOptions(Params, Options) || !ValidateOptions(Options))
	{
		return 1;
	}

	UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	if (GameSettings)
	{
		GameSettings->ActiveContentProfile = Options.LocalizationProfile;
	}

	const FGridlyContentFilterRule* ActiveRule = FGridlyLocalizedTextConverter::GetActiveContentFilterRule(GameSettings);
	const bool bIsFullGameProfile = Options.LocalizationProfile.Equals(TEXT("FullGame"), ESearchCase::IgnoreCase);
	if ((!GameSettings || !GameSettings->bEnableContentProfileFiltering) && !bIsFullGameProfile)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("Content profile filtering is disabled. Enable bEnableContentProfileFiltering before materializing profile assets."));
		return 1;
	}

	if (!ActiveRule && !bIsFullGameProfile)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("No ProfileRules entry was found for localization profile '%s'."), *Options.LocalizationProfile);
		return 1;
	}

	TArray<FGridlyTableRow> GridlyRows;
	if (!FetchGridlyRows(GridlyRows))
	{
		return 1;
	}

	TMap<FString, FGridlyTableRow> RowsByNamespaceAndKey;
	TMultiMap<FString, FGridlyTableRow> RowsByKey;
	BuildRecordLookup(GridlyRows, RowsByNamespaceAndKey, RowsByKey);

	FMaterializeStats Stats;
	if (!MaterializeStringTables(Options, RowsByNamespaceAndKey, RowsByKey, Stats))
	{
		return 1;
	}

	if (Options.bApplyToCanonicalForBuild && !RunGatherTextForLocalizationTargets())
	{
		return 1;
	}

	if (!MaterializeLocalizationArchives(Options, RowsByNamespaceAndKey, RowsByKey, Stats))
	{
		return 1;
	}

	if (Options.bApplyToCanonicalForBuild && !RunCompileTextForLocalizationTargets())
	{
		return 1;
	}

	UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
		TEXT("Content profile materialization complete. LocalizationProfile='%s', TablesDuplicated=%d, KeysPreserved=%d, SourceValuesRedacted=%d, ArchivesMaterialized=%d, TranslationsRedacted=%d, OriginalAssetsModified=%d"),
		*Options.LocalizationProfile, Stats.TablesDuplicated, Stats.KeysPreserved, Stats.ValuesRedacted, Stats.ArchivesMaterialized,
		Stats.TranslationsRedacted, Options.bApplyToCanonicalForBuild ? 1 : 0);
	return 0;
}

bool UGridlyMaterializeContentProfileCommandlet::ParseOptions(const FString& Params, FMaterializeOptions& OutOptions) const
{
	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamVals;
	UCommandlet::ParseCommandLine(*Params, Tokens, Switches, ParamVals);

	const UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	OutOptions.LocalizationProfile = ParamVals.FindRef(TEXT("LocalizationProfile"));
	if (OutOptions.LocalizationProfile.IsEmpty())
	{
		OutOptions.LocalizationProfile = ParamVals.FindRef(TEXT("Profile"));
	}
	if (OutOptions.LocalizationProfile.IsEmpty())
	{
		OutOptions.LocalizationProfile = ParamVals.FindRef(TEXT("ContentProfile"));
	}
	if (OutOptions.LocalizationProfile.IsEmpty() && GameSettings)
	{
		OutOptions.LocalizationProfile = GameSettings->ActiveContentProfile;
	}

	OutOptions.SourceRoot = ParamVals.FindRef(TEXT("SourceRoot"));
	if (OutOptions.SourceRoot.IsEmpty() && GameSettings)
	{
		OutOptions.SourceRoot = GameSettings->StringTableSavePath;
	}

	OutOptions.OutputRoot = ParamVals.FindRef(TEXT("OutputRoot"));
	if (OutOptions.OutputRoot.IsEmpty() && !OutOptions.LocalizationProfile.IsEmpty())
	{
		OutOptions.OutputRoot = FString::Printf(TEXT("/Game/LocalizationProfiles/%s/StringTables"), *OutOptions.LocalizationProfile);
	}
	OutOptions.ArchiveOutputRoot = ParamVals.FindRef(TEXT("ArchiveOutputRoot"));
	if (OutOptions.ArchiveOutputRoot.IsEmpty())
	{
		OutOptions.ArchiveOutputRoot = FPaths::ProjectSavedDir() / TEXT("LocalizationProfiles") / OutOptions.LocalizationProfile;
	}
	OutOptions.bReplaceExisting = true;
	if (Switches.Contains(TEXT("NoReplaceExisting")) || ParamVals.FindRef(TEXT("ReplaceExisting")).Equals(TEXT("false"), ESearchCase::IgnoreCase))
	{
		OutOptions.bReplaceExisting = false;
	}
	OutOptions.bApplyToCanonicalForBuild = Switches.Contains(TEXT("ApplyToCanonicalForBuild")) ||
		ParamVals.FindRef(TEXT("ApplyToCanonicalForBuild")).ToBool();

	if (OutOptions.LocalizationProfile.IsEmpty())
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error, TEXT("Missing localization profile. Pass -LocalizationProfile=Demo or set ActiveContentProfile."));
		return false;
	}

	if (OutOptions.SourceRoot.IsEmpty())
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error, TEXT("Missing source root. Pass -SourceRoot=/Game/Localization/StringTables."));
		return false;
	}

	return true;
}

bool UGridlyMaterializeContentProfileCommandlet::ValidateOptions(const FMaterializeOptions& Options) const
{
	if (!Options.bApplyToCanonicalForBuild && Options.SourceRoot.Equals(Options.OutputRoot, ESearchCase::IgnoreCase))
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("OutputRoot must be different from SourceRoot. Refusing to write over canonical StringTable assets."));
		return false;
	}

	if (!Options.SourceRoot.StartsWith(TEXT("/Game")) || !Options.OutputRoot.StartsWith(TEXT("/Game")))
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("SourceRoot and OutputRoot must be long package paths under /Game."));
		return false;
	}

	UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
		TEXT("LocalizationProfile='%s', SourceRoot='%s', OutputRoot='%s', ArchiveOutputRoot='%s', ReplaceExisting=%s"),
		*Options.LocalizationProfile, *Options.SourceRoot, *Options.OutputRoot, *Options.ArchiveOutputRoot,
		Options.bReplaceExisting ? TEXT("true") : TEXT("false"));
	if (Options.bApplyToCanonicalForBuild)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Warning,
			TEXT("Build overlay mode is enabled. Canonical StringTable and localization archive files in this workspace will be backed up and overwritten with redacted values for this build."));
	}
	else
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
			TEXT("Canonical StringTable and localization archive assets are read-only for this commandlet. Redacted values are written only to OutputRoot and ArchiveOutputRoot."));
	}

	return true;
}

bool UGridlyMaterializeContentProfileCommandlet::FetchGridlyRows(TArray<FGridlyTableRow>& OutRows) const
{
	const UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	if (!GameSettings || GameSettings->ImportApiKey.IsEmpty())
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error, TEXT("No Gridly import API key configured."));
		return false;
	}

	TArray<FString> ViewIds;
	for (const FString& ViewId : GameSettings->ImportFromViewIds)
	{
		if (!ViewId.IsEmpty())
		{
			ViewIds.Add(ViewId);
		}
	}

	if (ViewIds.Num() == 0)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error, TEXT("No Gridly import view IDs configured."));
		return false;
	}

	const int32 Limit = FMath::Max(1, GameSettings->ImportMaxRecordsPerRequest);
	for (const FString& ViewId : ViewIds)
	{
		int32 Offset = 0;
		int32 TotalCount = 0;
		do
		{
			const FString PaginationSettings =
				FGenericPlatformHttp::UrlEncode(FString::Printf(TEXT("{\"offset\":%d,\"limit\":%d}"), Offset, Limit));
			const FString Url = FString::Printf(TEXT("https://api.gridly.com/v1/views/%s/records?page=%s"), *ViewId, *PaginationSettings);

			TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = FHttpModule::Get().CreateRequest();
			HttpRequest->SetHeader(TEXT("Accept"), TEXT("application/json"));
			HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
			HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("ApiKey %s"), *GameSettings->ImportApiKey));
			HttpRequest->SetVerb(TEXT("GET"));
			HttpRequest->SetURL(Url);
			HttpRequest->ProcessRequest();

			while (HttpRequest->GetStatus() == EHttpRequestStatus::Processing)
			{
				FPlatformProcess::Sleep(0.1f);
				FHttpModule::Get().GetHttpManager().Tick(-1.f);
			}

			const FHttpResponsePtr Response = HttpRequest->GetResponse();
			if (!Response.IsValid() || Response->GetResponseCode() != EHttpResponseCodes::Ok)
			{
				UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
					TEXT("Failed to fetch Gridly records for view '%s'. HTTP status: %d"),
					*ViewId, Response.IsValid() ? Response->GetResponseCode() : 0);
				return false;
			}

			TArray<FGridlyTableRow> PageRows;
			if (!FJsonObjectConverter::JsonArrayStringToUStruct(Response->GetContentAsString(), &PageRows, 0, 0))
			{
				UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
					TEXT("Failed to parse Gridly records for view '%s'."), *ViewId);
				return false;
			}

			OutRows.Append(PageRows);
			TotalCount = FCString::Atoi(*Response->GetHeader(TEXT("X-Total-Count")));
			Offset += Limit;
		}
		while (Offset < TotalCount);
	}

	UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display, TEXT("Fetched %d Gridly records."), OutRows.Num());
	return true;
}

void UGridlyMaterializeContentProfileCommandlet::BuildRecordLookup(const TArray<FGridlyTableRow>& Rows,
	TMap<FString, FGridlyTableRow>& OutRowsByNamespaceAndKey, TMultiMap<FString, FGridlyTableRow>& OutRowsByKey) const
{
	const UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	if (!GameSettings)
	{
		return;
	}

	const bool bUseCombinedNamespaceKey = GameSettings->bUseCombinedNamespaceId;
	const bool bUsePathAsNamespace = !bUseCombinedNamespaceKey && GameSettings->NamespaceColumnId == TEXT("path");

	for (const FGridlyTableRow& Row : Rows)
	{
		FString Namespace = bUsePathAsNamespace ? Row.Path : TEXT("");
		FString Key = Row.Id;
		const FString OriginalRowId = Row.Id;

		if (!bUsePathAsNamespace && !bUseCombinedNamespaceKey)
		{
			for (const FGridlyTableCell& Cell : Row.Cells)
			{
				if (Cell.ColumnId.Equals(GameSettings->NamespaceColumnId, ESearchCase::IgnoreCase))
				{
					Namespace = Cell.Value;
					break;
				}
			}
		}

		if (bUseCombinedNamespaceKey)
		{
			FString NewKey;
			if (Key.Split(TEXT(","), &Namespace, &NewKey))
			{
				Key = NewKey;
			}
		}

		Namespace = Namespace.Replace(TEXT(" "), TEXT(""));
		if (!Namespace.IsEmpty() && !Key.IsEmpty())
		{
			OutRowsByNamespaceAndKey.Add(MakeRecordLookupKey(Namespace, Key), Row);
		}
		if (!Key.IsEmpty())
		{
			OutRowsByKey.Add(Key, Row);
		}

		FString PathNamespace = Row.Path.Replace(TEXT(" "), TEXT(""));
		if (!PathNamespace.IsEmpty() && !Key.IsEmpty())
		{
			OutRowsByNamespaceAndKey.Add(MakeRecordLookupKey(PathNamespace, Key), Row);
		}

		FString RowIdNamespace;
		FString RowIdKey;
		if (OriginalRowId.Split(TEXT(","), &RowIdNamespace, &RowIdKey))
		{
			RowIdNamespace = RowIdNamespace.Replace(TEXT(" "), TEXT(""));
			if (!RowIdNamespace.IsEmpty() && !RowIdKey.IsEmpty())
			{
				OutRowsByNamespaceAndKey.Add(MakeRecordLookupKey(RowIdNamespace, RowIdKey), Row);
				OutRowsByKey.Add(RowIdKey, Row);
			}
		}
	}

	UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
		TEXT("Built flag lookup for %d namespace/key records and %d key records."),
		OutRowsByNamespaceAndKey.Num(), OutRowsByKey.Num());
}

bool UGridlyMaterializeContentProfileCommandlet::MaterializeStringTables(const FMaterializeOptions& Options,
	const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey, const TMultiMap<FString, FGridlyTableRow>& RowsByKey,
	FMaterializeStats& OutStats) const
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	FARFilter Filter;
	Filter.PackagePaths.Add(*Options.SourceRoot);
	Filter.ClassPaths.Add(UStringTable::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = true;

	TArray<FAssetData> StringTableAssets;
	AssetRegistryModule.Get().SearchAllAssets(true);
	AssetRegistryModule.Get().GetAssets(Filter, StringTableAssets);
	if (StringTableAssets.Num() == 0)
	{
		// SourceRoot is usually a folder, but accepting an exact StringTable package/object path makes
		// build commandlet calls easier for projects that store a single canonical StringTable asset.
		TArray<FString> ExactStringTablePaths;
		ExactStringTablePaths.Add(Options.SourceRoot);
		ExactStringTablePaths.Add(FString::Printf(TEXT("%s.%s"), *Options.SourceRoot,
			*FPackageName::GetLongPackageAssetName(Options.SourceRoot)));

		for (const FString& ExactStringTablePath : ExactStringTablePaths)
		{
			if (UStringTable* ExactStringTable = LoadObject<UStringTable>(nullptr, *ExactStringTablePath))
			{
				StringTableAssets.Add(FAssetData(ExactStringTable));
				break;
			}
		}

		if (StringTableAssets.Num() == 0)
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Warning,
				TEXT("No StringTable assets found under SourceRoot '%s'. Pass a folder such as /Game/LocStringTables, or an exact StringTable asset path such as /Game/LocStringTables/Test."), *Options.SourceRoot);
			return false;
		}
	}

	for (const FAssetData& AssetData : StringTableAssets)
	{
		UStringTable* SourceStringTable = Cast<UStringTable>(AssetData.GetAsset());
		if (!SourceStringTable)
		{
			continue;
		}

		if (!MaterializeStringTable(SourceStringTable, AssetData.PackageName.ToString(), Options, RowsByNamespaceAndKey,
			RowsByKey, OutStats))
		{
			return false;
		}
	}

	return true;
}

bool UGridlyMaterializeContentProfileCommandlet::MaterializeStringTable(UStringTable* SourceStringTable,
	const FString& SourcePackageName, const FMaterializeOptions& Options,
	const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey, const TMultiMap<FString, FGridlyTableRow>& RowsByKey,
	FMaterializeStats& OutStats) const
{
	const FString OutputPackageName = Options.bApplyToCanonicalForBuild ? SourcePackageName : MakeOutputPackageName(SourcePackageName, Options);
	if (OutputPackageName.Equals(SourcePackageName, ESearchCase::IgnoreCase))
	{
		if (!Options.bApplyToCanonicalForBuild)
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
				TEXT("Refusing to materialize over source package '%s'."), *SourcePackageName);
			return false;
		}
	}

	const FString OutputAssetName = FPackageName::GetLongPackageAssetName(OutputPackageName);
	const FString FilePath = FPackageName::LongPackageNameToFilename(OutputPackageName, FPackageName::GetAssetPackageExtension());
	const bool bOutputPackageExists = FPaths::FileExists(FilePath);
	if (!Options.bReplaceExisting && bOutputPackageExists)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("Output package already exists: %s. Pass -ReplaceExisting to overwrite generated profile assets."), *OutputPackageName);
		return false;
	}

	if (Options.bApplyToCanonicalForBuild && !BackupFileForBuildOverlay(FilePath, Options))
	{
		return false;
	}

	UPackage* OutputPackage = Options.bApplyToCanonicalForBuild ? SourceStringTable->GetOutermost() : CreatePackage(*OutputPackageName);
	if (!OutputPackage)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("Failed to create output package '%s'."), *OutputPackageName);
		return false;
	}

	if (!Options.bApplyToCanonicalForBuild)
	{
		if (UObject* ExistingObject = StaticFindObject(UStringTable::StaticClass(), OutputPackage, *OutputAssetName))
		{
			ExistingObject->Rename(nullptr, GetTransientPackage(), REN_DontCreateRedirectors | REN_ForceNoResetLoaders);
		}
	}

	UStringTable* OutputStringTable = Options.bApplyToCanonicalForBuild
		? SourceStringTable
		: NewObject<UStringTable>(OutputPackage, FName(*OutputAssetName), RF_Public | RF_Standalone);
	if (!OutputStringTable)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("Failed to create output StringTable '%s'."), *OutputPackageName);
		return false;
	}

	const FString Namespace = SourceStringTable->GetName();
	FStringTable& OutputTable = OutputStringTable->GetMutableStringTable().Get();
	const UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	const FGridlyContentFilterRule* ActiveRule = FGridlyLocalizedTextConverter::GetActiveContentFilterRule(GameSettings);
	int32 TableKeys = 0;
	int32 TableRedactions = 0;
	int32 TableMatchedRows = 0;
	int32 TableFlaggedRows = 0;
	int32 TableAlreadyRedactedRows = 0;
	int32 TableKeyFallbackMatches = 0;
	int32 TableAmbiguousKeyFallbacks = 0;
	TArray<TPair<FString, FString>> RedactedSourceStrings;

	SourceStringTable->GetStringTable().Get().EnumerateSourceStrings(
		[this, &RowsByNamespaceAndKey, &RowsByKey, &RedactedSourceStrings, GameSettings, &Namespace, &TableKeys,
		&TableRedactions, &TableMatchedRows, &TableFlaggedRows, &TableAlreadyRedactedRows, &TableKeyFallbackMatches,
		&TableAmbiguousKeyFallbacks, ActiveRule]
		(const FString& Key, const FString& SourceString) -> bool
		{
			FString OutputString = SourceString;
			FGridlyTableRow KeyFallbackRow;
			const FGridlyTableRow* GridlyRow = RowsByNamespaceAndKey.Find(MakeRecordLookupKey(Namespace, Key));
			if (!GridlyRow)
			{
				TArray<FGridlyTableRow> KeyCandidates;
				RowsByKey.MultiFind(Key, KeyCandidates);
				if (KeyCandidates.Num() == 1)
				{
					KeyFallbackRow = KeyCandidates[0];
					GridlyRow = &KeyFallbackRow;
					++TableKeyFallbackMatches;
				}
				else if (KeyCandidates.Num() > 1)
				{
					++TableAmbiguousKeyFallbacks;
				}
			}

			if (GridlyRow)
			{
				++TableMatchedRows;
				const bool bShouldRedactRecord = FGridlyLocalizedTextConverter::ShouldRedactRecord(*GridlyRow, GameSettings, ActiveRule);
				if (bShouldRedactRecord)
				{
					++TableFlaggedRows;
				}

				OutputString = FGridlyLocalizedTextConverter::ApplyContentProfileFilteringToText(SourceString, *GridlyRow,
					GameSettings, true, false);
				if (OutputString != SourceString)
				{
					++TableRedactions;
				}
				else if (bShouldRedactRecord && ActiveRule && SourceString == ActiveRule->ReplacementText)
				{
					++TableAlreadyRedactedRows;
				}
			}

			RedactedSourceStrings.Emplace(Key, OutputString);
			++TableKeys;
			return true;
		});

	for (const TPair<FString, FString>& RedactedSourceString : RedactedSourceStrings)
	{
		OutputTable.SetSourceString(RedactedSourceString.Key, RedactedSourceString.Value);
	}

	if (!Options.bApplyToCanonicalForBuild)
	{
		FAssetRegistryModule::AssetCreated(OutputStringTable);
	}
	OutputPackage->MarkPackageDirty();

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
	if (!UPackage::SavePackage(OutputPackage, OutputStringTable, *FilePath, SaveArgs))
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("Failed to save materialized StringTable '%s' to '%s'."), *OutputPackageName, *FilePath);
		return false;
	}

	++OutStats.TablesDuplicated;
	OutStats.KeysPreserved += TableKeys;
	OutStats.ValuesRedacted += TableRedactions;

	UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
		TEXT("Materialized '%s' -> '%s' (%d keys, %d matched Gridly rows, %d flagged rows, %d key fallback matches, %d ambiguous key fallbacks, %d changed to redacted, %d already redacted)."),
		*SourcePackageName, *OutputPackageName, TableKeys, TableMatchedRows, TableFlaggedRows,
		TableKeyFallbackMatches, TableAmbiguousKeyFallbacks, TableRedactions, TableAlreadyRedactedRows);
	return true;
}

bool UGridlyMaterializeContentProfileCommandlet::MaterializeLocalizationArchives(const FMaterializeOptions& Options,
	const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey, const TMultiMap<FString, FGridlyTableRow>& RowsByKey,
	FMaterializeStats& OutStats) const
{
	const TArray<ULocalizationTarget*> LocalizationTargets = ULocalizationSettings::GetGameTargetSet()->TargetObjects;
	if (LocalizationTargets.Num() == 0)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Warning,
			TEXT("No localization targets found; only StringTable source assets were materialized."));
		return true;
	}

	for (ULocalizationTarget* LocalizationTarget : LocalizationTargets)
	{
		if (LocalizationTarget && !MaterializeArchivesForTarget(LocalizationTarget, Options, RowsByNamespaceAndKey,
			RowsByKey, OutStats))
		{
			return false;
		}
	}

	return true;
}

bool UGridlyMaterializeContentProfileCommandlet::MaterializeArchivesForTarget(ULocalizationTarget* LocalizationTarget,
	const FMaterializeOptions& Options, const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey,
	const TMultiMap<FString, FGridlyTableRow>& RowsByKey, FMaterializeStats& OutStats) const
{
	const FString ConfigFilePath = LocalizationConfigurationScript::GetGatherTextConfigPath(LocalizationTarget);
	const FString SectionName = TEXT("CommonSettings");

	FString SourcePath;
	FString ArchiveName;
	if (!GConfig->GetString(*SectionName, TEXT("SourcePath"), SourcePath, ConfigFilePath) ||
		!GConfig->GetString(*SectionName, TEXT("ArchiveName"), ArchiveName, ConfigFilePath))
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Warning,
			TEXT("Could not determine archive path for localization target '%s'."), *LocalizationTarget->Settings.Name);
		return true;
	}

	const FString ConfigFullPath = FPaths::ConvertRelativePathToFull(ConfigFilePath);
	const FString EngineFullPath = FPaths::ConvertRelativePathToFull(FPaths::EngineConfigDir());
	const bool bIsEngineTarget = ConfigFullPath.StartsWith(EngineFullPath);
	const FString RootPath = bIsEngineTarget ? FPaths::EngineDir() : FPaths::ProjectDir();
	const FString SourceArchiveRoot = FPaths::ConvertRelativePathToFull(FPaths::Combine(*RootPath, *SourcePath));
	const UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	FString NativeCulture;
	if (LocalizationTarget->Settings.SupportedCulturesStatistics.IsValidIndex(LocalizationTarget->Settings.NativeCultureIndex))
	{
		NativeCulture = LocalizationTarget->Settings.SupportedCulturesStatistics[LocalizationTarget->Settings.NativeCultureIndex].CultureName;
	}

	for (const FCultureStatistics& CultureStats : LocalizationTarget->Settings.SupportedCulturesStatistics)
	{
		const FString SourceArchivePath = FPaths::Combine(*SourceArchiveRoot, *CultureStats.CultureName, *ArchiveName);
		if (!FPaths::FileExists(SourceArchivePath))
		{
			continue;
		}

		const FString OutputArchivePath = Options.bApplyToCanonicalForBuild
			? SourceArchivePath
			: FPaths::Combine(*Options.ArchiveOutputRoot, *LocalizationTarget->Settings.Name, *CultureStats.CultureName, *ArchiveName);
		if (!Options.bApplyToCanonicalForBuild && !Options.bReplaceExisting && FPaths::FileExists(OutputArchivePath))
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
				TEXT("Output archive already exists: %s. Pass -ReplaceExisting to overwrite generated profile archives."),
				*OutputArchivePath);
			return false;
		}
		if (Options.bApplyToCanonicalForBuild && !BackupFileForBuildOverlay(SourceArchivePath, Options))
		{
			return false;
		}

		FString ArchiveJsonString;
		if (!FFileHelper::LoadFileToString(ArchiveJsonString, *SourceArchivePath))
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
				TEXT("Failed to load localization archive '%s'."), *SourceArchivePath);
			return false;
		}

		TSharedPtr<FJsonObject> ArchiveJson;
		const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(ArchiveJsonString);
		if (!FJsonSerializer::Deserialize(JsonReader, ArchiveJson) || !ArchiveJson.IsValid())
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
				TEXT("Failed to parse localization archive '%s'."), *SourceArchivePath);
			return false;
		}

		int32 ArchiveRedactions = 0;
		const bool bIsSourceCulture = CultureStats.CultureName.Equals(NativeCulture, ESearchCase::IgnoreCase);
		RedactArchiveJsonObject(ArchiveJson, TEXT(""), RowsByNamespaceAndKey, GameSettings, RowsByKey,
			bIsSourceCulture, ArchiveRedactions);

		FString OutputJsonString;
		const TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&OutputJsonString);
		if (!FJsonSerializer::Serialize(ArchiveJson.ToSharedRef(), JsonWriter))
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
				TEXT("Failed to serialize redacted localization archive '%s'."), *SourceArchivePath);
			return false;
		}

		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputArchivePath), true);
		if (!FFileHelper::SaveStringToFile(OutputJsonString, *OutputArchivePath))
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
				TEXT("Failed to save redacted localization archive '%s'."), *OutputArchivePath);
			return false;
		}

		++OutStats.ArchivesMaterialized;
		OutStats.TranslationsRedacted += ArchiveRedactions;
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
			TEXT("Materialized archive '%s' -> '%s' (%d translations redacted)."),
			*SourceArchivePath, *OutputArchivePath, ArchiveRedactions);
	}

	return true;
}

bool UGridlyMaterializeContentProfileCommandlet::RedactArchiveJsonObject(const TSharedPtr<FJsonObject>& JsonObject,
	const FString& CurrentNamespace, const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey,
	const UGridlyGameSettings* GameSettings, const TMultiMap<FString, FGridlyTableRow>& RowsByKey, bool bIsSourceCulture,
	int32& OutRedactedCount) const
{
	if (!JsonObject.IsValid())
	{
		return false;
	}

	FString EntryNamespace = CurrentNamespace;
	FString ObjectNamespace;
	if (JsonObject->TryGetStringField(TEXT("Namespace"), ObjectNamespace))
	{
		EntryNamespace = ObjectNamespace;
	}

	FString Key;
	if (JsonObject->TryGetStringField(TEXT("Key"), Key))
	{
		FGridlyTableRow KeyFallbackRow;
		const FGridlyTableRow* GridlyRow = RowsByNamespaceAndKey.Find(MakeRecordLookupKey(EntryNamespace, Key));
		if (!GridlyRow)
		{
			TArray<FGridlyTableRow> KeyCandidates;
			RowsByKey.MultiFind(Key, KeyCandidates);
			if (KeyCandidates.Num() == 1)
			{
				KeyFallbackRow = KeyCandidates[0];
				GridlyRow = &KeyFallbackRow;
			}
		}

		if (GridlyRow)
		{
			const TSharedPtr<FJsonValue> TranslationValue = JsonObject->TryGetField(TEXT("Translation"));
			const TSharedPtr<FJsonObject> TranslationObject = TranslationValue.IsValid() && TranslationValue->Type == EJson::Object
				? TranslationValue->AsObject()
				: nullptr;
			if (TranslationObject.IsValid())
			{
				FString TranslationText;
				TranslationObject->TryGetStringField(TEXT("Text"), TranslationText);
				const FString RedactedText = FGridlyLocalizedTextConverter::ApplyContentProfileFilteringToText(TranslationText,
					*GridlyRow, GameSettings, bIsSourceCulture, false);
				if (RedactedText != TranslationText)
				{
					TranslationObject->SetStringField(TEXT("Text"), RedactedText);
					++OutRedactedCount;
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Children = nullptr;
	if (JsonObject->TryGetArrayField(TEXT("Children"), Children))
	{
		for (const TSharedPtr<FJsonValue>& ChildValue : *Children)
		{
			RedactArchiveJsonObject(ChildValue->AsObject(), EntryNamespace, RowsByNamespaceAndKey, GameSettings,
				RowsByKey, bIsSourceCulture, OutRedactedCount);
		}
	}

	return true;
}

bool UGridlyMaterializeContentProfileCommandlet::RunGatherTextForLocalizationTargets() const
{
	TArray<LocalizationCommandletExecution::FTask> Tasks;
	for (ULocalizationTarget* LocalizationTarget : ULocalizationSettings::GetGameTargetSet()->TargetObjects)
	{
		if (!LocalizationTarget)
		{
			continue;
		}

		FString GatherScriptPath = LocalizationConfigurationScript::GetGatherTextConfigPath(LocalizationTarget);
		GatherScriptPath = FConfigCacheIni::NormalizeConfigIniPath(GatherScriptPath);
		LocalizationConfigurationScript::GenerateGatherTextConfigFile(LocalizationTarget).WriteWithSCC(GatherScriptPath);

		const bool bUseProjectFile = !LocalizationTarget->IsMemberOfEngineTargetSet();
		Tasks.Add(LocalizationCommandletExecution::FTask(
			FText::Format(NSLOCTEXT("GridlyMaterializeContentProfileCommandlet", "GatherTaskNameFormat", "Gather Text ({0})"),
				FText::FromString(LocalizationTarget->Settings.Name)),
			GatherScriptPath,
			bUseProjectFile));
	}

	if (Tasks.Num() == 0)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Warning,
			TEXT("No localization targets found for Gather Text; StringTables were redacted but localization archives/resources were not regenerated."));
		return true;
	}

	UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
		TEXT("Running Gather Text for %d localization target(s) after StringTable redaction."), Tasks.Num());
	return RunLocalizationCommandletTasks(Tasks);
}

bool UGridlyMaterializeContentProfileCommandlet::RunCompileTextForLocalizationTargets() const
{
	TArray<LocalizationCommandletExecution::FTask> Tasks;
	for (ULocalizationTarget* LocalizationTarget : ULocalizationSettings::GetGameTargetSet()->TargetObjects)
	{
		if (!LocalizationTarget)
		{
			continue;
		}

		FString CompileScriptPath = LocalizationConfigurationScript::GetCompileTextConfigPath(LocalizationTarget);
		CompileScriptPath = FConfigCacheIni::NormalizeConfigIniPath(CompileScriptPath);
		LocalizationConfigurationScript::GenerateCompileTextConfigFile(LocalizationTarget).WriteWithSCC(CompileScriptPath);

		const bool bUseProjectFile = !LocalizationTarget->IsMemberOfEngineTargetSet();
		Tasks.Add(LocalizationCommandletExecution::FTask(
			FText::Format(NSLOCTEXT("GridlyMaterializeContentProfileCommandlet", "CompileTaskNameFormat", "Compile Text ({0})"),
				FText::FromString(LocalizationTarget->Settings.Name)),
			CompileScriptPath,
			bUseProjectFile));
	}

	if (Tasks.Num() == 0)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Warning,
			TEXT("No localization targets found for Compile Text; localization resources were not regenerated."));
		return true;
	}

	UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
		TEXT("Running Compile Text for %d localization target(s) after archive redaction."), Tasks.Num());
	return RunLocalizationCommandletTasks(Tasks);
}

namespace
{
bool RunLocalizationCommandletTasks(const TArray<LocalizationCommandletExecution::FTask>& Tasks)
{
	for (const LocalizationCommandletExecution::FTask& LocTask : Tasks)
	{
		TSharedPtr<FLocalizationCommandletProcess> CommandletProcess =
			FLocalizationCommandletProcess::Execute(LocTask.ScriptPath, LocTask.ShouldUseProjectFile);
		if (!CommandletProcess.IsValid())
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
				TEXT("Failed to start localization commandlet task '%s'."), *LocTask.Name.ToString());
			return false;
		}

		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
			TEXT("=== Starting localization task [%s] ==="), *LocTask.Name.ToString());

		FProcHandle CurrentProcessHandle = CommandletProcess->GetHandle();
		int32 ReturnCode = INDEX_NONE;

		for (;;)
		{
			const FString PipeString = FPlatformProcess::ReadPipe(CommandletProcess->GetReadPipe());
			if (!PipeString.IsEmpty())
			{
				UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Log, TEXT("%s"), *PipeString);
			}

			if (!FPlatformProcess::IsProcRunning(CurrentProcessHandle) && PipeString.IsEmpty())
			{
				break;
			}

			FPlatformProcess::Sleep(0.0f);
		}

		if (!CurrentProcessHandle.IsValid() || !FPlatformProcess::GetProcReturnCode(CurrentProcessHandle, &ReturnCode))
		{
			UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
				TEXT("Localization task '%s' completed but no return code was available."), *LocTask.Name.ToString());
			return false;
		}

		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Display,
			TEXT("===> Localization task [%s] returned: %d"), *LocTask.Name.ToString(), ReturnCode);
		if (ReturnCode != 0)
		{
			return false;
		}
	}

	return true;
}
}

bool UGridlyMaterializeContentProfileCommandlet::BackupFileForBuildOverlay(const FString& SourceFilePath,
	const FMaterializeOptions& Options) const
{
	if (!FPaths::FileExists(SourceFilePath))
	{
		return true;
	}

	const FString FullSourceFilePath = FPaths::ConvertRelativePathToFull(SourceFilePath);
	FString RelativePath = FullSourceFilePath;
	FPaths::MakePathRelativeTo(RelativePath, *FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));
	if (RelativePath.StartsWith(TEXT("..")))
	{
		RelativePath = FPaths::GetCleanFilename(FullSourceFilePath);
	}

	const FString BackupPath = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("GridlyContentProfileBackups"),
		*Options.LocalizationProfile, *RelativePath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(BackupPath), true);
	const uint32 CopyResult = IFileManager::Get().Copy(*BackupPath, *FullSourceFilePath, true, true);
	if (CopyResult != COPY_OK)
	{
		UE_LOG(LogGridlyMaterializeContentProfileCommandlet, Error,
			TEXT("Failed to backup '%s' to '%s' before applying build overlay. CopyResult=%d"),
			*FullSourceFilePath, *BackupPath, CopyResult);
		return false;
	}

	return true;
}

FString UGridlyMaterializeContentProfileCommandlet::MakeRecordLookupKey(const FString& Namespace, const FString& Key) const
{
	return FString::Printf(TEXT("%s,%s"), *Namespace, *Key);
}

FString UGridlyMaterializeContentProfileCommandlet::MakeOutputPackageName(const FString& SourcePackageName,
	const FMaterializeOptions& Options) const
{
	FString RelativePath = SourcePackageName;
	RelativePath.RemoveFromStart(Options.SourceRoot);
	RelativePath.RemoveFromStart(TEXT("/"));
	return Options.OutputRoot / RelativePath;
}
