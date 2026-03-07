// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Misc/NetworkGuid.h"
#include "Nyx/Data/NyxTypes.h"
#include "ModuleBindings/Types/CharacterStatsType.g.h"
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

	// ──── Seamless Migration: Deterministic GUID Forcing ────
	//
	// Both servers compute the same deterministic GUID for a given ClientHandshakeId.
	// Server-A assigns this GUID at PostLogin; Server-B forces the same GUID on claim.
	// Combined with EChannelCloseReason::Migrated, the proxy reuses the same
	// proxy-side object, and the client sees zero interruption.

	/** Compute a deterministic migration GUID from HandshakeId and slot index.
	 *  Slot 0 = PlayerController, Slot 1 = Pawn. */
	static FNetworkGUID MakeMigrationGUID(uint32 HandshakeId, uint8 Slot);

	/** Assign a deterministic migration GUID to an actor (replaces any auto-assigned GUID). */
	void AssignMigrationGUID(AActor* Actor, uint32 HandshakeId, uint8 Slot);

	/** Tracks players currently being transferred (prevent double-transfer). */
	TSet<APlayerController*> PlayersBeingTransferred;

	/** Grace period after arriving from a transfer — prevents immediate bounce-back. */
	TMap<APlayerController*, double> TransferArrivalTimes;

	/** PCs currently being claimed via migration — PostLogin skips SpacetimeDB for these. */
	TSet<APlayerController*> MigrationClaimPCs;

	/**
	 * Per-NoPawnPC migration tracking.
	 * Tracks position transitions to detect when a migration claim should happen.
	 */
	struct FNoPawnMigrationTracking
	{
		/** Whether the NoPawnPC position was ever observed outside our zone. */
		bool bWasEverOutsideOurZone = false;

		/** Whether the NoPawnPC position has moved significantly from its initial value.
		 *  Proxy-setup NoPawnPCs start at (0,0,0) or the boundary. Real migration
		 *  NoPawnPCs receive position updates from the proxy as it syncs the player's
		 *  actual world position. We use this to distinguish the two cases. */
		bool bPositionHasMoved = false;

		/** True if this NoPawnPC was created by our own ReleasePawnAuthority().
		 *  Prevents the releasing server from immediately re-claiming when
		 *  the proxy bounces the NoPawnPC position back into our zone. */
		bool bReleasedByUs = false;

		/** The first position we observed for this NoPawnPC. */
		FVector InitialPosition = FVector::ZeroVector;

		/** When the NoPawnPC most recently entered our zone (0 = not currently in zone). */
		double EnteredOurZoneTime = 0.0;

		/** When we first detected this NoPawnPC. */
		double FirstSeenTime = 0.0;

		/** When we released this NoPawnPC (set by ReleasePawnAuthority). 0 = not released by us. */
		double ReleasedByUsTime = 0.0;
	};

	/** Migration tracking for each NoPawnPC we monitor. */
	TMap<APlayerController*, FNoPawnMigrationTracking> NoPawnTracking;

	/** How many seconds to wait after transfer before the zone check can trigger again. */
	static constexpr float TransferGracePeriodSeconds = 5.0f;

	/** Grace period for migration claims after a position transition (outside→inside our zone).
	 *  Must be long enough for the primary server to detect boundary crossing (0.5s timer)
	 *  and complete release (destroy pawn, swap PC, proxy detection) BEFORE we claim.
	 *  Too short → race condition: Server-2 claims while Server-1 still has active pawn,
	 *  causing a ghost pawn on the client that steals possession. */
	static constexpr float NoPawnClaimGracePeriodSeconds = 2.0f;

	/** Grace period for NoPawnPCs that started in our zone and whose position has moved.
	 *  Longer than transition grace to give the primary server time to detect the
	 *  boundary crossing and release. */
	static constexpr float SettledNoPawnClaimGracePeriodSeconds = 4.0f;

	// ──── SpacetimeDB Migration Signal ────
	//
	// The proxy cannot reliably signal Server-B when Server-A releases.
	// Position-based detection has a deadlock: after Server-A releases,
	// the proxy stops syncing NoPawnPC positions (no pawn viewpoint),
	// but stale-position detection has false positives (standing player
	// also produces zero position updates due to proxy's Equals optimization).
	//
	// Solution: use SpacetimeDB as the coordination mechanism.
	// When Server-A releases, it saves the character state (SaveCharacterState).
	// Both servers subscribe to character_stats. Server-B receives the update,
	// sees the saved position is in its zone, and claims the NoPawnPC.

	/** Called when SpacetimeDB notifies us that another server saved a character.
	 *  Checks if the saved position is in our zone and triggers a migration claim. */
	UFUNCTION()
	void HandleExternalCharacterSaved(const FCharacterStatsType& Stats);

	/** True when SpacetimeDB signals that a character was saved with a position
	 *  in our zone by another server. Consumed by CheckZoneBoundaries. */
	bool bMigrationClaimPending = false;

	/** The position from the SpacetimeDB save (crossing position). */
	FVector MigrationClaimPosition = FVector::ZeroVector;

	FTimerHandle ZoneCheckTimerHandle;
};
