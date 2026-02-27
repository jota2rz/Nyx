// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Nyx/Data/NyxTypes.h"
#include "NyxEntityManager.generated.h"

/**
 * World subsystem that manages actor lifecycle based on SpacetimeDB table events.
 *
 * Replaces UE5's built-in replication. Instead of:
 *   - UPROPERTY(Replicated)
 *   - UFUNCTION(Server/Client)
 *   - APlayerState
 *
 * We use:
 *   - SpacetimeDB table rows → Actor spawn/update/destroy
 *   - Reducer calls → Server RPCs
 *   - Subscription queries → Interest management
 *
 * Flow:
 *   1. SpacetimeDB row inserted → OnPlayerInserted → Spawn actor
 *   2. SpacetimeDB row updated  → OnPlayerUpdated  → Update actor transform
 *   3. SpacetimeDB row deleted  → OnPlayerDeleted  → Destroy actor
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
	 * Start listening for database events from the network subsystem.
	 * Call this after authentication is complete.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|World")
	void StartListening();

	/** Stop listening and destroy all managed entities. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|World")
	void StopListening();

	/** Get the actor for a given entity ID. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|World")
	AActor* GetEntityActor(FNyxEntityId EntityId) const;

	/** Get the entity ID associated with a local player. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|World")
	FNyxEntityId GetLocalPlayerEntityId() const { return LocalPlayerEntityId; }

	/** Check if an entity is the local player. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|World")
	bool IsLocalPlayer(FNyxEntityId EntityId) const { return EntityId == LocalPlayerEntityId; }

	// ──── Delegates ────

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnEntitySpawnedBP, FNyxEntityId, EntityId, AActor*, Actor);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnEntityUpdatedBP, FNyxEntityId, EntityId, AActor*, Actor);
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnEntityDestroyedBP, FNyxEntityId, EntityId);

	UPROPERTY(BlueprintAssignable, Category = "Nyx|World")
	FOnEntitySpawnedBP OnEntitySpawnedBP;

	UPROPERTY(BlueprintAssignable, Category = "Nyx|World")
	FOnEntityUpdatedBP OnEntityUpdatedBP;

	UPROPERTY(BlueprintAssignable, Category = "Nyx|World")
	FOnEntityDestroyedBP OnEntityDestroyedBP;

	/**
	 * The actor class to spawn for remote players.
	 * TODO: Make this configurable via data asset.
	 */
	UPROPERTY(EditAnywhere, Category = "Nyx|World")
	TSubclassOf<AActor> RemotePlayerActorClass;

private:
	void HandlePlayerInserted(const FNyxPlayerData& PlayerData);
	void HandlePlayerUpdated(const FNyxPlayerData& OldData, const FNyxPlayerData& NewData);
	void HandlePlayerDeleted(const FNyxPlayerData& PlayerData);

	/** Spawns an actor for an entity */
	AActor* SpawnEntityActor(const FNyxPlayerData& Data);

	/** Updates an existing actor's transform and state */
	void UpdateEntityActor(AActor* Actor, const FNyxPlayerData& NewData);

	/** Maps SpacetimeDB entity IDs to spawned UE5 actors */
	UPROPERTY()
	TMap<int64, TObjectPtr<AActor>> ManagedEntities;

	/** The local player's entity ID (set when our CreatePlayer reducer succeeds) */
	FNyxEntityId LocalPlayerEntityId;

	/** Whether we're actively listening for DB events */
	bool bIsListening = false;

	FDelegateHandle InsertHandle;
	FDelegateHandle UpdateHandle;
	FDelegateHandle DeleteHandle;
};
