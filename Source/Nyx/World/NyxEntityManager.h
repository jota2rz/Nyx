// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Nyx/Data/NyxTypes.h"
#include "ModuleBindings/Types/PlayerType.g.h"
#include "NyxEntityManager.generated.h"

class UNyxMovementComponent;
class ANyxPlayerPawn;
struct FEventContext;

/**
 * World subsystem that manages actor lifecycle based on SpacetimeDB table events.
 *
 * Spike 6 update:
 *  - Wires directly to PlayerTable::OnInsert/OnUpdate/OnDelete
 *  - Spawns ANyxPlayerPawn for local player (with prediction)
 *  - Spawns basic actors for remote players (with interpolation)
 *  - Routes server updates to UNyxMovementComponent for reconciliation
 */
UCLASS()
class NYX_API UNyxEntityManager : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/**
	 * Start listening for SpacetimeDB table events.
	 * Call this after the spatial subscription is applied.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|World")
	void StartListening();

	/** Stop listening and destroy all managed entities. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|World")
	void StopListening();

	/** Get the actor for a given player identity. */
	AActor* GetPlayerActor(const FSpacetimeDBIdentity& Identity) const;

	/** Get the local player's pawn (null until spawned). */
	ANyxPlayerPawn* GetLocalPlayerPawn() const { return LocalPlayerPawn; }

	// ──── Delegates ────

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnPlayerSpawnedBP, AActor*, Actor, bool, bIsLocal);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPlayerRemovedBP, AActor*, Actor);

	UPROPERTY(BlueprintAssignable, Category = "Nyx|World")
	FOnPlayerSpawnedBP OnPlayerSpawnedBP;

	UPROPERTY(BlueprintAssignable, Category = "Nyx|World")
	FOnPlayerRemovedBP OnPlayerRemovedBP;

	/**
	 * The pawn class to spawn for the local player.
	 * Should be ANyxPlayerPawn or subclass.
	 */
	UPROPERTY(EditAnywhere, Category = "Nyx|World")
	TSubclassOf<ANyxPlayerPawn> LocalPlayerPawnClass;

private:
	// ──── SpacetimeDB table callbacks (AddDynamic-compatible) ────

	UFUNCTION()
	void HandlePlayerInsert(const FEventContext& Context, const FPlayerType& NewRow);

	UFUNCTION()
	void HandlePlayerUpdate(const FEventContext& Context, const FPlayerType& OldRow, const FPlayerType& NewRow);

	UFUNCTION()
	void HandlePlayerDelete(const FEventContext& Context, const FPlayerType& DeletedRow);

	/** Spawn an actor for a player. Returns the spawned actor. */
	AActor* SpawnPlayerActor(const FPlayerType& PlayerData, bool bIsLocal);

	/** Maps SpacetimeDB identities (as hex string) to spawned actors */
	UPROPERTY()
	TMap<FString, TObjectPtr<AActor>> ManagedPlayers;

	/** The local player's pawn (kept as typed reference for convenience) */
	UPROPERTY()
	TObjectPtr<ANyxPlayerPawn> LocalPlayerPawn;

	/** Whether we're actively listening for DB events */
	bool bIsListening = false;

	/** Helper to convert FSpacetimeDBIdentity to map key */
	static FString IdentityKey(const FSpacetimeDBIdentity& Id);
};
