// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxMockConnection.h"
#include "Nyx/Nyx.h"

UNyxMockConnection::UNyxMockConnection()
	: CurrentState(ENyxConnectionState::Disconnected)
	, NextSubscriptionHandle(1)
	, NextEntityId(1)
	, LocalPlayerEntityId(0)
{
}

void UNyxMockConnection::Connect(const FString& Host, const FString& DatabaseName)
{
	UE_LOG(LogNyxNet, Log, TEXT("[Mock] Connecting to %s/%s"), *Host, *DatabaseName);
	SetConnectionState(ENyxConnectionState::Connecting);

	// Simulate async connection — in mock, connect instantly
	SetConnectionState(ENyxConnectionState::Connected);
	UE_LOG(LogNyxNet, Log, TEXT("[Mock] Connected successfully (mock)"));
}

void UNyxMockConnection::Disconnect()
{
	UE_LOG(LogNyxNet, Log, TEXT("[Mock] Disconnecting"));
	MockPlayerTable.Empty();
	LocalPlayerEntityId = 0;
	SetConnectionState(ENyxConnectionState::Disconnected);
}

ENyxConnectionState UNyxMockConnection::GetConnectionState() const
{
	return CurrentState;
}

int32 UNyxMockConnection::Subscribe(const TArray<FString>& SqlQueries)
{
	int32 Handle = NextSubscriptionHandle++;
	UE_LOG(LogNyxNet, Log, TEXT("[Mock] Subscribe (handle=%d) with %d queries"), Handle, SqlQueries.Num());

	for (const FString& Query : SqlQueries)
	{
		UE_LOG(LogNyxNet, Verbose, TEXT("[Mock]   Query: %s"), *Query);
	}

	// Simulate subscription applied — fire existing rows as inserts
	for (const auto& Pair : MockPlayerTable)
	{
		PlayerInsertedDelegate.Broadcast(Pair.Value);
	}

	SubscriptionAppliedDelegate.Broadcast();
	return Handle;
}

void UNyxMockConnection::Unsubscribe(int32 SubscriptionHandle)
{
	UE_LOG(LogNyxNet, Log, TEXT("[Mock] Unsubscribe handle=%d"), SubscriptionHandle);
}

void UNyxMockConnection::CallAuthenticateWithEOS(
	const FString& EOSIdToken,
	const FString& EOSProductUserId,
	const FString& DisplayName,
	const FString& Platform)
{
	UE_LOG(LogNyxAuth, Log, TEXT("[Mock] AuthenticateWithEOS: PUID=%s, Name=%s, Platform=%s"),
		*EOSProductUserId, *DisplayName, *Platform);

	// In mock, auth always succeeds
	AuthCompleteDelegate.Broadcast(true);
}

void UNyxMockConnection::CallCreatePlayer(const FString& Name)
{
	UE_LOG(LogNyxNet, Log, TEXT("[Mock] CreatePlayer: %s"), *Name);

	FNyxPlayerData NewPlayer;
	NewPlayer.EntityId = FNyxEntityId(NextEntityId++);
	NewPlayer.Identity.DisplayName = Name;
	NewPlayer.Identity.SpacetimeIdentity = FString::Printf(TEXT("mock_identity_%lld"), NewPlayer.EntityId.Value);
	NewPlayer.Position.Location = FVector::ZeroVector;
	NewPlayer.Position.Rotation = FRotator::ZeroRotator;
	NewPlayer.Health = 100;
	NewPlayer.MaxHealth = 100;
	NewPlayer.Level = 1;

	LocalPlayerEntityId = NewPlayer.EntityId.Value;
	MockPlayerTable.Add(NewPlayer.EntityId.Value, NewPlayer);
	PlayerInsertedDelegate.Broadcast(NewPlayer);
}

void UNyxMockConnection::CallMovePlayer(const FVector& Position, const FRotator& Rotation)
{
	if (LocalPlayerEntityId == 0)
	{
		UE_LOG(LogNyxNet, Warning, TEXT("[Mock] MovePlayer called but no local player exists"));
		return;
	}

	FNyxPlayerData* Player = MockPlayerTable.Find(LocalPlayerEntityId);
	if (!Player)
	{
		UE_LOG(LogNyxNet, Warning, TEXT("[Mock] MovePlayer: local player %lld not in table"), LocalPlayerEntityId);
		return;
	}

	FNyxPlayerData OldData = *Player;

	Player->Position.Location = Position;
	Player->Position.Rotation = Rotation;

	PlayerUpdatedDelegate.Broadcast(OldData, *Player);
}

void UNyxMockConnection::SetConnectionState(ENyxConnectionState NewState)
{
	if (CurrentState != NewState)
	{
		CurrentState = NewState;
		ConnectionStateChangedDelegate.Broadcast(CurrentState);
	}
}
