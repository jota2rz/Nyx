// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Nyx/Data/NyxTypes.h"
#include "NyxNetworkSubsystem.generated.h"

class INyxDatabaseInterface;
class UNyxMockConnection;

/**
 * Core networking subsystem that owns the SpacetimeDB connection.
 *
 * Responsibilities:
 *  - Creates and manages the database connection (mock or SpacetimeDB)
 *  - Handles connection lifecycle (connect, disconnect, reconnect)
 *  - Manages subscriptions (spatial interest queries)
 *  - Provides the connection interface to other subsystems
 *
 * Other subsystems (Auth, World) get the connection from here.
 */
UCLASS()
class NYX_API UNyxNetworkSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/**
	 * Connect to the game server.
	 * @param Host - SpacetimeDB host (e.g. "127.0.0.1:3000")
	 * @param DatabaseName - SpacetimeDB database name (e.g. "nyx-world")
	 * @param bUseMock - If true, use mock connection instead of real SpacetimeDB
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Network")
	void ConnectToServer(const FString& Host, const FString& DatabaseName, bool bUseMock = false);

	/** Disconnect from the game server. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Network")
	void DisconnectFromServer();

	/** Get the current connection state. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Network")
	ENyxConnectionState GetConnectionState() const;

	/** Get the database interface (for other subsystems to use). */
	INyxDatabaseInterface* GetDatabaseInterface() const { return DatabaseInterface; }

	/**
	 * Subscribe to spatial queries for the area around a position.
	 * Manages chunk-based interest: subscribes to nearby chunks,
	 * unsubscribes from far-away chunks.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Network")
	void UpdateSpatialSubscription(const FVector& PlayerPosition);

	// ──── Delegates ────

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnConnectionStateChangedBP, ENyxConnectionState, NewState);

	UPROPERTY(BlueprintAssignable, Category = "Nyx|Network")
	FOnConnectionStateChangedBP OnConnectionStateChangedBP;

private:
	void HandleConnectionStateChanged(ENyxConnectionState NewState);

	/** The active database connection. Always accessed through INyxDatabaseInterface. */
	UPROPERTY()
	TObjectPtr<UObject> DatabaseConnectionObject;

	/** Typed pointer to the interface (non-owning, points to DatabaseConnectionObject). */
	INyxDatabaseInterface* DatabaseInterface = nullptr;

	/** Current spatial subscription handle. */
	int32 CurrentSpatialSubscriptionHandle = INDEX_NONE;

	/** Last chunk the player was in (to detect chunk changes). */
	FIntPoint LastChunkCoord = FIntPoint(TNumericLimits<int32>::Max(), TNumericLimits<int32>::Max());

	/** Chunk size in Unreal units (cm). 10000 = 100 meters. */
	static constexpr float ChunkSizeUnreal = 10000.0f;

	/** How many chunks around the player to subscribe to. */
	static constexpr int32 SubscriptionRadius = 2;
};
