// Copyright (c) 2021 LocalizeDirect AB

#pragma once

#include "CoreMinimal.h"

#include "GridlyTableRow.h"

struct FGridlyContentFilterRule;
class UGridlyGameSettings;

class GRIDLY_API FGridlyLocalizedTextConverter
{
public:
	static bool TableRowsToPolyglotTextDatas(const TArray<FGridlyTableRow>& TableRows,
		TMap<FString, FPolyglotTextData>& OutPolyglotTextDatas);
	static bool WritePoFile(const TArray<FPolyglotTextData>& PolyglotTextDatas, const FString& TargetCulture, const FString& Path);

	static const FGridlyContentFilterRule* GetActiveContentFilterRule(const UGridlyGameSettings* GameSettings);
	static bool DoesGridlyCellContainFlag(const FGridlyTableCell& GridlyTableCell, const FString& FlagName);
	static bool ShouldRedactRecord(const FGridlyTableRow& TableRow, const UGridlyGameSettings* GameSettings,
		const FGridlyContentFilterRule* ContentFilterRule);
	static FString ApplyContentProfileFilteringToText(const FString& Text, const FGridlyTableRow& TableRow,
		const UGridlyGameSettings* GameSettings, bool bIsSourceText, bool bRespectImportOptIn = true);
};
