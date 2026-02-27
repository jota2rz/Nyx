// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "NyxGameInstance.generated.h"

/**
 * Nyx game instance — owns the game lifecycle.
 *
 * This GameInstance does NOT use UE5's built-in networking.
 * Instead, it uses subsystems:
 *   - UNyxNetworkSubsystem: Manages SpacetimeDB connection
 *   - UNyxAuthSubsystem: Manages EOS + SpacetimeDB auth flow
 *   - UNyxEntityManager: Manages actor lifecycle from DB events (world subsystem)
 *
 * The GameInstance coordinates the high-level flow:
 *   1. Player launches game
 *   2. Login (EOS auth → SpacetimeDB connect → authenticate reducer)
 *   3. Subscribe to world data (spatial interest)
 *   4. Entity manager spawns actors from subscription data
 *   5. Player actions → reducer calls → server validation → table updates → actor updates
 */
UCLASS()
class NYX_API UNyxGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;

	/**
	 * Start the game: login and connect to the world.
	 *
	 * @param bUseMock - If true, use mock backend (no SpacetimeDB or EOS needed)
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx")
	void StartGame(bool bUseMock = false);

	/** Get the SpacetimeDB host address. Configurable for development. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Config")
	FString SpacetimeDBHost = TEXT("127.0.0.1:3000");

	/** Get the SpacetimeDB database name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Config")
	FString SpacetimeDBDatabaseName = TEXT("nyx-world");

	/** EOS login type (e.g., "accountportal", "developer", "persistentauth") */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Config")
	FString EOSLoginType = TEXT("developer");

private:
	void OnLoginComplete(bool bSuccess, const FString& ErrorMessage);
};
