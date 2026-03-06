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

	/** Proxy: return nullptr to prevent pawn spawning (game server handles it). */
	virtual APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot) override;

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

	/**
	 * Returns true when running as any kind of server (dedicated, listen, or PIE listen).
	 * Use this instead of IsRunningDedicatedServer() so the SpacetimeDB flow
	 * also activates when testing with "Play As Listen Server" in PIE.
	 */
	bool IsNyxServer() const;

	// ──── Zone Transfer (Spike 19) ────

	/** Address of the server to transfer players to when they cross the zone boundary. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Server")
	FString TransferAddress;

	/** X coordinate of the zone boundary. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Server")
	float ZoneBoundaryX = 0.f;

	/**
	 * Which side of the boundary this server owns.
	 * true = this server owns X < BoundaryX ("west")
	 * false = this server owns X >= BoundaryX ("east")
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Nyx|Server")
	bool bOwnsNegativeSide = true;

	/** Returns true when this process is running as a MultiServer proxy (not a real game server). */
	bool IsProxyServer() const;

private:
	void OnAuthStateChanged(ENyxAuthState NewState);

	/** Timer-based zone boundary check for all connected players. */
	void CheckZoneBoundaries();

	// ──── Proxy-based pawn migration (MultiServer Replication Plugin) ────
	//
	// When a proxy-connected player crosses the zone boundary, authority
	// transfers seamlessly between game servers:
	//
	//   Server A (releasing):
	//     1. Detects player left its zone
	//     2. Saves pawn state, destroys pawn
	//     3. Swaps real PC → ANoPawnPlayerController
	//     4. Proxy detects: "Server A now has NoPawnPC for this route"
	//
	//   Server B (claiming):
	//     1. Detects NoPawnPC at a position inside its zone
	//     2. Spawns pawn at NoPawnPC's location
	//     3. Swaps ANoPawnPlayerController → real PC, possesses pawn
	//     4. Proxy detects: "Server B now has real PC for this route"
	//
	//   Proxy finalizes: primary server flips from A to B. RPCs route to B.
	//   Client sees no interruption — the proxy handles the transition.

	/**
	 * Server A side: release authority over a proxy player.
	 * Saves state, destroys pawn, swaps PC to NoPawnPlayerController.
	 * The proxy will detect this and begin the migration handshake.
	 */
	void ReleasePawnAuthority(APlayerController* PC, ANyxCharacter* NyxChar);

	/**
	 * Server B side: claim authority over a NoPawnPlayerController.
	 * Spawns a pawn at the NoPawnPC's position, creates a new real PC,
	 * swaps NoPawnPC → real PC, and possesses the pawn.
	 * The proxy will detect this and finalize the migration.
	 */
	void ClaimPawnAuthority(APlayerController* NoPawnPC, const FVector& SpawnLocation, const FRotator& SpawnRotation);

	/** Tracks players currently being transferred (prevent double-transfer). */
	TSet<APlayerController*> PlayersBeingTransferred;

	/** Grace period after arriving from a transfer — prevents immediate bounce-back. */
	TMap<APlayerController*, double> TransferArrivalTimes;

	/** How many seconds to wait after transfer before the zone check can trigger again. */
	static constexpr float TransferGracePeriodSeconds = 5.0f;

	FTimerHandle ZoneCheckTimerHandle;
};
