// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Nyx/Data/NyxTypes.h"
#include "NyxGameMode.generated.h"

/**
 * Local-only game mode for Nyx.
 *
 * This game mode does NOT run on a UE5 dedicated server.
 * There is no UE5 server in our architecture — SpacetimeDB IS the server.
 *
 * This game mode:
 *  - Manages the local game state machine (login → lobby → world)
 *  - Triggers entity manager startup after auth is complete
 *  - Handles local UI state transitions
 */
UCLASS()
class NYX_API ANyxGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ANyxGameMode();

	virtual void StartPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/**
	 * Called to enter the game world after authentication.
	 * Starts entity listening and subscribes to world data.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx")
	void EnterWorld();

	/**
	 * For quick testing: auto-login with mock backend.
	 * Set to true for development without SpacetimeDB running.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Debug")
	bool bAutoLoginMock = false;

private:
	void OnAuthStateChanged(ENyxAuthState NewState);
};
