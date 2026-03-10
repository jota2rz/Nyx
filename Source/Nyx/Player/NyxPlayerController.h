// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "NyxPlayerController.generated.h"

/**
 * Nyx player controller.
 *
 * Sets PlayerCameraManagerClass to ANyxPlayerCameraManager so the client
 * camera holds its last valid position during DSTM server-transfer gaps.
 */
UCLASS()
class NYX_API ANyxPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ANyxPlayerController();
};
