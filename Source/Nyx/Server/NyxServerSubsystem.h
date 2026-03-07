// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "ModuleBindings/Types/CharacterStatsType.g.h"
#include "ModuleBindings/Types/CombatEventType.g.h"
#include "ModuleBindings/Types/ZoneServerType.g.h"
#include "NyxServerSubsystem.generated.h"

class UDbConnection;
class ANyxCharacter;
struct FEventContext;
struct FSpacetimeDBIdentity;
struct FSubscriptionEventContext;
struct FErrorContext;

/**
 * Dedicated-server-only subsystem: bridges UE5 to SpacetimeDB.
 *
 * Option 4 architecture: Only the UE5 dedicated server connects to
 * SpacetimeDB. Clients NEVER talk to SpacetimeDB directly.
 *
 * Responsibilities:
 *   - Owns the single SpacetimeDB WebSocket connection
 *   - Registers this server with SpacetimeDB (ZoneServer table)
 *   - Sends periodic heartbeats
 *   - Loads/saves character data
 *   - Routes combat events to SpacetimeDB for resolution
 *   - Applies SpacetimeDB combat results back to ACharacter replicated props
 *   - Manages server lifecycle (register → heartbeat → drain → deregister)
 *
 * NOT instantiated on game clients — guarded by IsRunningDedicatedServer().
 */
UCLASS()
class NYX_API UNyxServerSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	// Always create — the subsystem is inert until ConnectAndRegister() is called.
	// This allows it to work on dedicated servers, listen servers, and standalone (with -NyxServer).
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override { return true; }
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ──── Connection ────

	/**
	 * Connect to SpacetimeDB and register this zone server.
	 * @param Host - SpacetimeDB host (e.g. "127.0.0.1:3000")
	 * @param DatabaseName - SpacetimeDB database name (e.g. "nyx")
	 * @param ZoneId - The zone this server is responsible for (e.g. "castle_siege")
	 * @param ServerId - Unique ID for this server instance
	 * @param MaxEntities - Max entities this server can handle
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Server")
	void ConnectAndRegister(const FString& Host, const FString& DatabaseName,
		const FString& ZoneId, const FString& ServerId, int32 MaxEntities);

	/** Gracefully shut down: drain, deregister, disconnect. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Server")
	void Shutdown();

	/** Is the server connected and registered? */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Server")
	bool IsConnected() const { return SpacetimeDBConnection != nullptr && bRegistered; }

	/** Get the raw SpacetimeDB connection. */
	UDbConnection* GetConnection() const { return SpacetimeDBConnection; }

	// ──── Character Lifecycle ────

	/**
	 * Called when a player joins this dedicated server.
	 * Loads character data from SpacetimeDB.
	 *
	 * Note: In the full auth pipeline, the player's SpacetimeDB identity
	 * would come from EOS auth. For now, the subsystem generates a
	 * deterministic identity from the player's display name.
	 * TODO: Wire up EOS → SpacetimeDB identity mapping.
	 */
	void OnPlayerJoined(ANyxCharacter* Character, const FString& PlayerDisplayName);

	/**
	 * Called when a player leaves this dedicated server.
	 * Saves character state to SpacetimeDB.
	 */
	void OnPlayerLeft(ANyxCharacter* Character);

	// ──── Combat ────

	/** Request SpacetimeDB to resolve a hit (damage). */
	void RequestResolveHit(const FSpacetimeDBIdentity& AttackerId,
		const FSpacetimeDBIdentity& DefenderId, uint32 SkillId);

	/** Request SpacetimeDB to resolve a heal. */
	void RequestResolveHeal(const FSpacetimeDBIdentity& HealerId,
		const FSpacetimeDBIdentity& TargetId, uint32 SkillId);

	// ──── Save ────

	/** Force-save a character's current state to SpacetimeDB. */
	void SaveCharacterState(ANyxCharacter* Character);

	// ──── Delegates ────

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnCharacterLoaded, ANyxCharacter*, Character, const FCharacterStatsType&, Stats);

	/** Fired when character stats are loaded from SpacetimeDB. */
	UPROPERTY(BlueprintAssignable, Category = "Nyx|Server")
	FOnCharacterLoaded OnCharacterLoaded;

	/** Fired when SpacetimeDB notifies us that a character we DON'T manage was saved.
	 *  This happens when another server saves a character during migration release.
	 *  The GameMode uses this to detect a pending migration claim. */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnExternalCharacterSaved, const FCharacterStatsType&, Stats);

	UPROPERTY(BlueprintAssignable, Category = "Nyx|Server")
	FOnExternalCharacterSaved OnExternalCharacterSaved;

	// ──── Config ────

	/** This server's zone ID. */
	UPROPERTY(BlueprintReadOnly, Category = "Nyx|Server")
	FString ZoneId;

	/** This server's unique ID. */
	UPROPERTY(BlueprintReadOnly, Category = "Nyx|Server")
	FString ServerId;

private:
	// ──── SpacetimeDB Callbacks ────

	UFUNCTION()
	void HandleConnect(UDbConnection* Connection, FSpacetimeDBIdentity Identity, const FString& Token);

	UFUNCTION()
	void HandleDisconnect(UDbConnection* Connection, const FString& Error);

	UFUNCTION()
	void HandleConnectError(const FString& ErrorMessage);

	UFUNCTION()
	void HandleSubscriptionApplied(FSubscriptionEventContext Context);

	UFUNCTION()
	void HandleSubscriptionError(FErrorContext Context);

	// ──── Table Callbacks ────

	UFUNCTION()
	void HandleCharacterStatsInsert(const FEventContext& Context, const FCharacterStatsType& NewRow);

	UFUNCTION()
	void HandleCharacterStatsUpdate(const FEventContext& Context, const FCharacterStatsType& OldRow, const FCharacterStatsType& NewRow);

	// ──── Heartbeat Timer ────

	void StartHeartbeat();
	void StopHeartbeat();
	void SendHeartbeat();

	// ──── Save Timer ────

	void StartAutoSave();
	void StopAutoSave();
	void AutoSaveAllCharacters();

	// ──── State ────

	UPROPERTY()
	TObjectPtr<UDbConnection> SpacetimeDBConnection;

	/** Maps SpacetimeDB identity (hex) → managed character actor */
	UPROPERTY()
	TMap<FString, TObjectPtr<ANyxCharacter>> ManagedCharacters;

	/** Pending character loads: identity (hex) → character actor waiting for stats */
	UPROPERTY()
	TMap<FString, TObjectPtr<ANyxCharacter>> PendingLoads;

	/** Players who joined before SpacetimeDB was connected (PostLogin fires before StartPlay in listen server). */
	UPROPERTY()
	TMap<FString, TObjectPtr<ANyxCharacter>> EarlyJoinQueue;

	bool bRegistered = false;
	int32 MaxEntities = 300;

	/** Timer handles */
	FTimerHandle HeartbeatTimerHandle;
	FTimerHandle AutoSaveTimerHandle;

	/** Heartbeat interval in seconds */
	static constexpr float HeartbeatInterval = 30.f;

	/** Auto-save interval in seconds (dirty flush) */
	static constexpr float AutoSaveInterval = 60.f;

	/** Helper: identity to map key */
	static FString IdentityKey(const FSpacetimeDBIdentity& Id);

	/**
	 * Generate a deterministic per-player SpacetimeDB identity from a display name.
	 * This is a placeholder until EOS auth provides real per-player identities.
	 * Uses MD5 hash of the name to produce a unique 32-byte identity.
	 */
	static FSpacetimeDBIdentity GeneratePlayerIdentity(const FString& PlayerDisplayName);
};
