// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GridlySetContentProfileAndMaterializeCommandlet.generated.h"

UCLASS()
class UGridlySetContentProfileAndMaterializeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGridlySetContentProfileAndMaterializeCommandlet(const FObjectInitializer& ObjectInitializer);

	virtual int32 Main(const FString& Params) override;

private:
	bool ResolveLocalizationProfile(const FString& Params, FString& OutLocalizationProfile) const;
};
