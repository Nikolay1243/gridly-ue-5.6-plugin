// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GridlyTableRow.h"
#include "GridlyMaterializeContentProfileCommandlet.generated.h"

class UStringTable;
class ULocalizationTarget;
class UGridlyGameSettings;

UCLASS()
class UGridlyMaterializeContentProfileCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGridlyMaterializeContentProfileCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;

private:
	struct FMaterializeOptions
	{
		FString LocalizationProfile;
		FString SourceRoot;
		FString OutputRoot;
		FString ArchiveOutputRoot;
		bool bReplaceExisting = true;
		bool bApplyToCanonicalForBuild = false;
	};

	struct FMaterializeStats
	{
		int32 TablesDuplicated = 0;
		int32 KeysPreserved = 0;
		int32 ValuesRedacted = 0;
		int32 ArchivesMaterialized = 0;
		int32 TranslationsRedacted = 0;
	};

	bool ParseOptions(const FString& Params, FMaterializeOptions& OutOptions) const;
	bool ValidateOptions(const FMaterializeOptions& Options) const;
	bool FetchGridlyRows(TArray<FGridlyTableRow>& OutRows) const;
	void BuildRecordLookup(const TArray<FGridlyTableRow>& Rows, TMap<FString, FGridlyTableRow>& OutRowsByNamespaceAndKey) const;
	bool MaterializeStringTables(const FMaterializeOptions& Options, const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey,
		FMaterializeStats& OutStats) const;
	bool MaterializeStringTable(UStringTable* SourceStringTable, const FString& SourcePackageName, const FMaterializeOptions& Options,
		const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey, FMaterializeStats& OutStats) const;
	bool MaterializeLocalizationArchives(const FMaterializeOptions& Options, const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey,
		FMaterializeStats& OutStats) const;
	bool MaterializeArchivesForTarget(ULocalizationTarget* LocalizationTarget, const FMaterializeOptions& Options,
		const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey, FMaterializeStats& OutStats) const;
	bool RedactArchiveJsonObject(const TSharedPtr<FJsonObject>& JsonObject, const FString& CurrentNamespace,
		const TMap<FString, FGridlyTableRow>& RowsByNamespaceAndKey, const UGridlyGameSettings* GameSettings,
		int32& OutRedactedCount) const;
	bool BackupFileForBuildOverlay(const FString& SourceFilePath, const FMaterializeOptions& Options) const;
	FString MakeRecordLookupKey(const FString& Namespace, const FString& Key) const;
	FString MakeOutputPackageName(const FString& SourcePackageName, const FMaterializeOptions& Options) const;
};
