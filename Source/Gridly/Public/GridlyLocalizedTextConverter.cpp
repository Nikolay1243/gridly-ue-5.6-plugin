// Copyright (c) 2021 LocalizeDirect AB

#include "GridlyLocalizedTextConverter.h"

#include "Gridly.h"
#include "GridlyCultureConverter.h"
#include "GridlyDataTableImporterJSON.h"
#include "GridlyGameSettings.h"
#include "Internationalization/PolyglotTextData.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
bool DoesTokenMatchFlag(const FString& Token, const FString& FlagName)
{
	FString NormalizedToken = Token.TrimStartAndEnd().TrimQuotes();
	NormalizedToken.RemoveFromStart(TEXT("'"));
	NormalizedToken.RemoveFromEnd(TEXT("'"));
	return NormalizedToken.Equals(FlagName, ESearchCase::IgnoreCase);
}

void AddStringTokens(const FString& Value, TArray<FString>& OutTokens)
{
	FString TokenizedValue = Value;
	for (int32 Index = 0; Index < TokenizedValue.Len(); ++Index)
	{
		const TCHAR Character = TokenizedValue[Index];
		if (Character == TEXT(',') || Character == TEXT(';') || Character == TEXT('|') ||
			Character == TEXT('\n') || Character == TEXT('\r') || Character == TEXT('\t') ||
			Character == TEXT('[') || Character == TEXT(']') || Character == TEXT('{') ||
			Character == TEXT('}') || Character == TEXT('(') || Character == TEXT(')') ||
			Character == TEXT('"') || Character == TEXT('\'') || Character == TEXT(':'))
		{
			TokenizedValue[Index] = TEXT(' ');
		}
	}

	TokenizedValue.ParseIntoArray(OutTokens, TEXT(" "), true);
}
}

const FGridlyContentFilterRule* FGridlyLocalizedTextConverter::GetActiveContentFilterRule(
	const UGridlyGameSettings* GameSettings)
{
	if (!GameSettings || !GameSettings->bEnableContentProfileFiltering || GameSettings->ActiveContentProfile.IsEmpty())
	{
		return nullptr;
	}

	return GameSettings->ProfileRules.Find(GameSettings->ActiveContentProfile);
}

bool FGridlyLocalizedTextConverter::DoesGridlyCellContainFlag(const FGridlyTableCell& GridlyTableCell,
	const FString& FlagName)
{
	if (FlagName.IsEmpty() || GridlyTableCell.Value.IsEmpty())
	{
		return false;
	}

	if (DoesTokenMatchFlag(GridlyTableCell.Value, FlagName))
	{
		return true;
	}

	TArray<TSharedPtr<FJsonValue>> JsonValues;
	const TSharedRef<TJsonReader<>> JsonReader = TJsonReaderFactory<>::Create(GridlyTableCell.Value);
	if (FJsonSerializer::Deserialize(JsonReader, JsonValues))
	{
		for (const TSharedPtr<FJsonValue>& JsonValue : JsonValues)
		{
			if (!JsonValue.IsValid())
			{
				continue;
			}

			FString JsonToken;
			if (JsonValue->Type == EJson::String)
			{
				JsonToken = JsonValue->AsString();
			}
			else
			{
				const TSharedRef<TJsonWriter<>> JsonWriter = TJsonWriterFactory<>::Create(&JsonToken);
				FJsonSerializer::Serialize(JsonValue.ToSharedRef(), TEXT(""), JsonWriter);
			}

			if (DoesTokenMatchFlag(JsonToken, FlagName))
			{
				return true;
			}
		}
	}

	TArray<FString> Tokens;
	AddStringTokens(GridlyTableCell.Value, Tokens);
	for (const FString& Token : Tokens)
	{
		if (DoesTokenMatchFlag(Token, FlagName))
		{
			return true;
		}
	}

	return false;
}

bool FGridlyLocalizedTextConverter::ShouldRedactRecord(const FGridlyTableRow& TableRow,
	const UGridlyGameSettings* GameSettings, const FGridlyContentFilterRule* ContentFilterRule)
{
	if (!GameSettings || !ContentFilterRule || ContentFilterRule->FlagName.IsEmpty() || GameSettings->FlagsColumnIdOrName.IsEmpty())
	{
		return false;
	}

	for (const FGridlyTableCell& GridlyTableCell : TableRow.Cells)
	{
		const bool bIsConfiguredFlagsColumn = GridlyTableCell.ColumnId.Equals(GameSettings->FlagsColumnIdOrName, ESearchCase::IgnoreCase);
		if (bIsConfiguredFlagsColumn && DoesGridlyCellContainFlag(GridlyTableCell, ContentFilterRule->FlagName))
		{
			return true;
		}
	}

	return false;
}

FString FGridlyLocalizedTextConverter::ApplyContentProfileFilteringToText(const FString& Text,
	const FGridlyTableRow& TableRow, const UGridlyGameSettings* GameSettings, bool bIsSourceText,
	bool bRespectImportOptIn)
{
	if (!GameSettings || (bRespectImportOptIn && !GameSettings->bApplyContentProfileFilteringDuringImport))
	{
		return Text;
	}

	const FGridlyContentFilterRule* ContentFilterRule = GetActiveContentFilterRule(GameSettings);
	if (!ShouldRedactRecord(TableRow, GameSettings, ContentFilterRule))
	{
		return Text;
	}

	const bool bShouldRedactText = bIsSourceText ? ContentFilterRule->bApplyToSource : ContentFilterRule->bApplyToTranslations;
	if (!bShouldRedactText)
	{
		return Text;
	}

	if (bRespectImportOptIn && GameSettings->bApplyContentProfileFilteringDuringImport)
	{
		UGridlyGameSettings* MutableGameSettings = GetMutableDefault<UGridlyGameSettings>();
		if (MutableGameSettings && !MutableGameSettings->bEditableAssetsMayContainContentProfileRedactions)
		{
			MutableGameSettings->bEditableAssetsMayContainContentProfileRedactions = true;
			MutableGameSettings->SaveConfig();
		}

		static bool bHasLoggedImportRedactionWarning = false;
		if (!bHasLoggedImportRedactionWarning)
		{
			bHasLoggedImportRedactionWarning = true;
			UE_LOG(LogGridly, Warning, TEXT("Content profile filtering is applying during import. Redacted values will be written into editable UE assets. Avoid uploading/exporting these assets to Gridly until a full unredacted import has been performed."));
		}
	}

	// Preserve the Gridly record and Unreal StringTable key, replacing only protected text for this content profile.
	return ContentFilterRule->ReplacementText;
}

bool FGridlyLocalizedTextConverter::TableRowsToPolyglotTextDatas(const TArray<FGridlyTableRow>& TableRows,
	TMap<FString, FPolyglotTextData>& OutPolyglotTextDatas)
{
	UGridlyGameSettings* GameSettings = GetMutableDefault<UGridlyGameSettings>();
	const TArray<FString> TargetCultures = FGridlyCultureConverter::GetTargetCultures();

	const bool bUseCombinedNamespaceKey = GameSettings->bUseCombinedNamespaceId;
	const bool bUsePathAsNamespace = !bUseCombinedNamespaceKey && GameSettings->NamespaceColumnId == "path";

	for (int i = 0; i < TableRows.Num(); i++)
	{
		UE_LOG(LogGridly, Verbose, TEXT("Row %d: %s (%s)"), i, *TableRows[i].Id, *TableRows[i].Path);

		FString Key = TableRows[i].Id;
		FString FullKey = Key;
		FString Namespace = bUsePathAsNamespace ? TableRows[i].Path : TEXT("");
		FString SourceCulture;
		FString SourceText;
		TMap<FString, FString> Translations;

		for (int j = 0; j < TableRows[i].Cells.Num(); j++)
		{
			const FGridlyTableCell& GridlyTableCell = TableRows[i].Cells[j];

			// If special columns

			if (!bUsePathAsNamespace && GridlyTableCell.ColumnId == GameSettings->NamespaceColumnId)
			{
				Namespace = GridlyTableCell.Value;
				continue;
			}

			// If language column

			if (GridlyTableCell.ColumnId.StartsWith(GameSettings->SourceLanguageColumnIdPrefix))
			{
				const FString GridlyCulture = GridlyTableCell.ColumnId.RightChop(GameSettings->SourceLanguageColumnIdPrefix.Len());
				FString Culture;
				if (FGridlyCultureConverter::ConvertFromGridly(TargetCultures, GridlyCulture, Culture))
				{
					SourceCulture = Culture;
					SourceText = GridlyTableCell.Value;
				}
			}
			else if (GridlyTableCell.ColumnId.StartsWith(GameSettings->TargetLanguageColumnIdPrefix))
			{
				const FString GridlyCulture = GridlyTableCell.ColumnId.RightChop(GameSettings->TargetLanguageColumnIdPrefix.Len());
				FString Culture;
				if (FGridlyCultureConverter::ConvertFromGridly(TargetCultures, GridlyCulture, Culture))
				{
					Translations.Add(Culture, GridlyTableCell.Value);
				}
			}
		}

		SourceText = ApplyContentProfileFilteringToText(SourceText, TableRows[i], GameSettings, true);
		for (TPair<FString, FString>& Pair : Translations)
		{
			Pair.Value = ApplyContentProfileFilteringToText(Pair.Value, TableRows[i], GameSettings, false);
		}

		// Namespace / key fixes

		if (bUseCombinedNamespaceKey)
		{
			FString NewKey;
			if (Key.Split(",", &Namespace, &NewKey))
			{
				Key = NewKey;
			}
		}

		Namespace = Namespace.Replace(TEXT(" "), TEXT(""));

		if (SourceText.IsEmpty() || SourceCulture.IsEmpty())
		{
			UE_LOG(LogGridly, Warning, TEXT("Could not find native culture/source string in imported text with key: %s,%s"),
				*Namespace, *Key);
			//continue;
		}

		FPolyglotTextData PolyglotTextData(ELocalizedTextSourceCategory::Game, Namespace, Key, SourceText, SourceCulture);

		for (const TPair<FString, FString>& Pair : Translations)
		{
			if (!Pair.Value.IsEmpty())
			{
				PolyglotTextData.AddLocalizedString(Pair.Key, Pair.Value);
			}
		}

		OutPolyglotTextDatas.Add(FullKey, PolyglotTextData);
	}

	return OutPolyglotTextDatas.Num() > 0;
}

// Taken from "Engine\Source\Developer\Localization\Private\PortableObjectPipeline.cpp"
FString ConditionArchiveStrForPO(const FString& InStr)
{
	FString Result = InStr;
	Result.ReplaceInline(TEXT("\\"), TEXT("\\\\"), ESearchCase::CaseSensitive);
	Result.ReplaceInline(TEXT("\""), TEXT("\\\""), ESearchCase::CaseSensitive);
	Result.ReplaceInline(TEXT("\r"), TEXT("\\r"), ESearchCase::CaseSensitive);
	Result.ReplaceInline(TEXT("\n"), TEXT("\\n"), ESearchCase::CaseSensitive);
	Result.ReplaceInline(TEXT("\t"), TEXT("\\t"), ESearchCase::CaseSensitive);
	return Result;
}

bool FGridlyLocalizedTextConverter::WritePoFile(const TArray<FPolyglotTextData>& PolyglotTextDatas, const FString& TargetCulture,
	const FString& Path)
{
	TArray<FString> Lines;
	int numOfLines = 0;
	TArray<TCHAR> CharsToReplace = { TEXT('\n'), TEXT('\r'), TEXT('\t'), TEXT('"'), TEXT('\\') };

	for (int i = 0; i < PolyglotTextDatas.Num(); i++)
	{
		FString TargetString;

		if (PolyglotTextDatas[i].GetLocalizedString(TargetCulture, TargetString))
		{
			Lines.Add(FString::Printf(TEXT("msgctxt \"%s,%s\""), *PolyglotTextDatas[i].GetNamespace(),
				*PolyglotTextDatas[i].GetKey()));

			
			FString NativeString = PolyglotTextDatas[i].GetNativeString().ReplaceCharWithEscapedChar(&CharsToReplace);
			Lines.Add(FString::Printf(TEXT("msgid \"%s\""), *NativeString));

			TargetString = ConditionArchiveStrForPO(TargetString);
			//TargetString.ReplaceCharWithEscapedChar(&CharsToReplace);
			Lines.Add(FString::Printf(TEXT("msgstr \"%s\""), *TargetString));

			Lines.Add(TEXT(""));
		}		
		else {
			Lines.Add(FString::Printf(TEXT("msgctxt \"%s,%s\""), *PolyglotTextDatas[i].GetNamespace(),
				*PolyglotTextDatas[i].GetKey()));


			FString NativeString = PolyglotTextDatas[i].GetNativeString().ReplaceCharWithEscapedChar(&CharsToReplace);
			Lines.Add(FString::Printf(TEXT("msgid \"%s\""), *NativeString));

			TargetString = TEXT("");
			TargetString = TargetString.ReplaceCharWithEscapedChar(&CharsToReplace);
			Lines.Add(FString::Printf(TEXT("msgstr \"%s\""), *TargetString));

			Lines.Add(TEXT(""));
		}
		
	}

	if (FFileHelper::SaveStringArrayToFile(Lines, *Path))
	{
		UE_LOG(LogGridly, Log, TEXT("Exported .po file (%d lines): %s"), Lines.Num(), *Path);
		if (Lines.Num() > 0) {
			return true;
		}
		else {
			return false;
		}
	}
	else
	{
		UE_LOG(LogGridly, Error, TEXT("Failed to export .po file to path: %s"), *Path);
		return false;
	}	
}
