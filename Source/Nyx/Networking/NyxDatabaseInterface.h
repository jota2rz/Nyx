// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Nyx/Data/NyxTypes.h"
#include "NyxDatabaseInterface.generated.h"

/**
 * Abstract interface for the game's database/server connection.
 *
 * This interface decouples game logic from the SpacetimeDB SDK,
 * enabling:
 *  - Development with a mock backend (no SpacetimeDB instance needed)
 *  - Swapping implementations (SpacetimeDB, mock, replay)
 *  - Unit testing game systems in isolation
 *
 * After Research Spike 1 is complete, a UNyxSpacetimeDBConnection
 * implementation will be created that wraps the official
 * UDbConnection from the SpacetimeDB Unreal plugin.
 */
UINTERFACE(MinimalAPI, Blueprintable)
class UNyxDatabaseInterface : public UInterface
{
	GENERATED_BODY()
};

class NYX_API INyxDatabaseInterface
{
	GENERATED_BODY()

public:
	// ──── Connection ────

	/** Connect to the game database. */
	virtual void Connect(const FString& Host, const FString& DatabaseName) = 0;

	/** Disconnect from the game database. */
	virtual void Disconnect() = 0;

	/** Current connection state. */
	virtual ENyxConnectionState GetConnectionState() const = 0;

	// ──── Subscriptions ────

	/**
	 * Subscribe to a set of SQL queries.
	 * SpacetimeDB will push matching rows + diffs to the client cache.
	 * Returns a handle ID for managing the subscription.
	 */
	virtual int32 Subscribe(const TArray<FString>& SqlQueries) = 0;

	/** Unsubscribe using the handle ID from Subscribe(). */
	virtual void Unsubscribe(int32 SubscriptionHandle) = 0;

	// ──── Reducer Calls (RPCs to SpacetimeDB) ────

	/** Authenticate with EOS by passing the token to the server module. */
	virtual void CallAuthenticateWithEOS(const FString& EOSIdToken, const FString& EOSProductUserId, const FString& DisplayName, const FString& Platform) = 0;

	/** Request to create a player character in the world. */
	virtual void CallCreatePlayer(const FString& Name) = 0;

	/** Send the local player's position to the server for validation. */
	virtual void CallMovePlayer(const FVector& Position, const FRotator& Rotation) = 0;

	// ──── Delegates ────

	/** Fired when connection state changes. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnConnectionStateChanged, ENyxConnectionState);
	virtual FOnConnectionStateChanged& OnConnectionStateChanged() = 0;

	/** Fired when a player row is inserted into the client cache. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnPlayerInserted, const FNyxPlayerData&);
	virtual FOnPlayerInserted& OnPlayerInserted() = 0;

	/** Fired when a player row is updated in the client cache. */
	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPlayerUpdated, const FNyxPlayerData& /*OldData*/, const FNyxPlayerData& /*NewData*/);
	virtual FOnPlayerUpdated& OnPlayerUpdated() = 0;

	/** Fired when a player row is deleted from the client cache. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnPlayerDeleted, const FNyxPlayerData&);
	virtual FOnPlayerDeleted& OnPlayerDeleted() = 0;

	/** Fired when initial subscription data has been applied. */
	DECLARE_MULTICAST_DELEGATE(FOnSubscriptionApplied);
	virtual FOnSubscriptionApplied& OnSubscriptionApplied() = 0;

	/** Fired when the EOS authentication reducer completes successfully. */
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnAuthComplete, bool /*bSuccess*/);
	virtual FOnAuthComplete& OnAuthComplete() = 0;
};
