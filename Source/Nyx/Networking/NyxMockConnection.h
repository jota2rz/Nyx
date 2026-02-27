// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Nyx/Networking/NyxDatabaseInterface.h"
#include "NyxMockConnection.generated.h"

/**
 * Mock implementation of INyxDatabaseInterface for local development.
 *
 * Simulates SpacetimeDB behavior in-memory:
 *  - Stores player data in TMap
 *  - Fires Insert/Update/Delete delegates when data changes
 *  - Reducer calls modify local state directly
 *  - No network connection needed
 *
 * Use this to develop and test all game systems before
 * SpacetimeDB plugin integration is complete (Research Spike 1).
 */
UCLASS()
class NYX_API UNyxMockConnection : public UObject, public INyxDatabaseInterface
{
	GENERATED_BODY()

public:
	UNyxMockConnection();

	// ──── INyxDatabaseInterface ────
	virtual void Connect(const FString& Host, const FString& DatabaseName) override;
	virtual void Disconnect() override;
	virtual ENyxConnectionState GetConnectionState() const override;
	virtual int32 Subscribe(const TArray<FString>& SqlQueries) override;
	virtual void Unsubscribe(int32 SubscriptionHandle) override;
	virtual void CallAuthenticateWithEOS(const FString& EOSIdToken, const FString& EOSProductUserId, const FString& DisplayName, const FString& Platform) override;
	virtual void CallCreatePlayer(const FString& Name) override;
	virtual void CallMovePlayer(const FVector& Position, const FRotator& Rotation) override;

	virtual FOnConnectionStateChanged& OnConnectionStateChanged() override { return ConnectionStateChangedDelegate; }
	virtual FOnPlayerInserted& OnPlayerInserted() override { return PlayerInsertedDelegate; }
	virtual FOnPlayerUpdated& OnPlayerUpdated() override { return PlayerUpdatedDelegate; }
	virtual FOnPlayerDeleted& OnPlayerDeleted() override { return PlayerDeletedDelegate; }
	virtual FOnSubscriptionApplied& OnSubscriptionApplied() override { return SubscriptionAppliedDelegate; }
	virtual FOnAuthComplete& OnAuthComplete() override { return AuthCompleteDelegate; }

private:
	void SetConnectionState(ENyxConnectionState NewState);

	ENyxConnectionState CurrentState;
	int32 NextSubscriptionHandle;
	int64 NextEntityId;

	/** In-memory player table */
	TMap<int64, FNyxPlayerData> MockPlayerTable;

	/** The "local" player entity ID (simulates ctx.sender) */
	int64 LocalPlayerEntityId;

	// Delegates
	FOnConnectionStateChanged ConnectionStateChangedDelegate;
	FOnPlayerInserted PlayerInsertedDelegate;
	FOnPlayerUpdated PlayerUpdatedDelegate;
	FOnPlayerDeleted PlayerDeletedDelegate;
	FOnSubscriptionApplied SubscriptionAppliedDelegate;
	FOnAuthComplete AuthCompleteDelegate;
};
