// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "NyxHUD.generated.h"

/**
 * Simple debug HUD overlay showing Proxy, Server, and Zone info.
 * Draws text in the top-left corner so players always know which
 * server/zone/proxy they are connected to.
 */
UCLASS()
class NYX_API ANyxHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
