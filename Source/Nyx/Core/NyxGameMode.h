// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Nyx/Data/NyxTypes.h"
#include "NyxGameMode.generated.h"

class ANyxCharacter;

/**
 * Nyx game mode — Option 4 architecture.
 *
 * Runs on the UE5 dedicated server. The dedicated server is authoritative
 * for movement, physics, and replication. SpacetimeDB is used as a backend
 * for persistence, combat compute, and orchestration.
 *
 * Server flow:
 *   1. StartPlay: connect to SpacetimeDB, register this zone server
 *   2. Player joins: PostLogin → load character from SpacetimeDB
 *   3. Character stats arrive → apply to ANyxCharacter → replicate to client
 *   4. Combat: hit events → SpacetimeDB::ResolveHit → stats update → replicate
 *   5. Periodic auto-save of character positions/state
 *   6. Player leaves: Logout → save character → deregister
 *
 * Also supports standalone/PIE mode for development (auto-login mock).
 */
UCLASS()
class NYX_API ANyxGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ANyxGameMode();

	virtual void StartPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** Called when a player logs in. On dedicated server: load character from SpacetimeDB. */
	virtual void PostLogin(APlayerController* NewPlayer) override;

	/** Called when a player logs out. On dedicated server: save character to SpacetimeDB. */
	virtual void Logout(AController* Exiting) override;

	// ──── Legacy (Phase 0 / PIE development) ────

	/**
	 * Called to enter the game world after authentication.
	 * Used in standalone/PIE mode. On dedicated server, PostLogin handles this.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx")
	void EnterWorld();

	/**
	 * For quick testing: auto-login with mock backend.
	 * Set to true for development without SpacetimeDB running.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Debug")
	bool bAutoLoginMock = false;

	// ──── Dedicated Server Config ────

	/** SpacetimeDB host for the dedicated server to connect to. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Server")
	FString SpacetimeDBHost = TEXT("127.0.0.1:3000");

	/** SpacetimeDB database name. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Server")
	FString DatabaseName = TEXT("nyx");

	/** The zone this dedicated server manages. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Server")
	FString ZoneId = TEXT("default");

	/** Unique server ID for orchestration. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Server")
	FString DedicatedServerId = TEXT("server-1");

private:
	void OnAuthStateChanged(ENyxAuthState NewState);
};
