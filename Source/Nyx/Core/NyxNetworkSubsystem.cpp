// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxNetworkSubsystem.h"
#include "Nyx/Nyx.h"
#include "Nyx/Networking/NyxDatabaseInterface.h"
#include "Nyx/Networking/NyxMockConnection.h"

void UNyxNetworkSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogNyxNet, Log, TEXT("NyxNetworkSubsystem initialized"));
}

void UNyxNetworkSubsystem::Deinitialize()
{
	DisconnectFromServer();
	UE_LOG(LogNyxNet, Log, TEXT("NyxNetworkSubsystem deinitialized"));
	Super::Deinitialize();
}

void UNyxNetworkSubsystem::ConnectToServer(const FString& Host, const FString& DatabaseName, bool bUseMock)
{
	// Disconnect existing connection if any
	if (DatabaseInterface)
	{
		DisconnectFromServer();
	}

	if (bUseMock)
	{
		UE_LOG(LogNyxNet, Log, TEXT("Creating mock database connection"));
		UNyxMockConnection* MockConn = NewObject<UNyxMockConnection>(this);
		DatabaseConnectionObject = MockConn;
		DatabaseInterface = MockConn;
	}
	else
	{
		// TODO (Spike 1): Create UNyxSpacetimeDBConnection wrapping UDbConnection
		// For now, fall back to mock with a warning
		UE_LOG(LogNyxNet, Warning, TEXT("SpacetimeDB connection not yet implemented — falling back to mock. Complete Research Spike 1."));
		UNyxMockConnection* MockConn = NewObject<UNyxMockConnection>(this);
		DatabaseConnectionObject = MockConn;
		DatabaseInterface = MockConn;
	}

	// Bind to connection state changes
	DatabaseInterface->OnConnectionStateChanged().AddUObject(this, &UNyxNetworkSubsystem::HandleConnectionStateChanged);

	// Connect
	DatabaseInterface->Connect(Host, DatabaseName);
}

void UNyxNetworkSubsystem::DisconnectFromServer()
{
	if (DatabaseInterface)
	{
		DatabaseInterface->OnConnectionStateChanged().RemoveAll(this);
		DatabaseInterface->Disconnect();
		DatabaseInterface = nullptr;
	}

	DatabaseConnectionObject = nullptr;
	CurrentSpatialSubscriptionHandle = INDEX_NONE;
	LastChunkCoord = FIntPoint(TNumericLimits<int32>::Max(), TNumericLimits<int32>::Max());
}

ENyxConnectionState UNyxNetworkSubsystem::GetConnectionState() const
{
	if (DatabaseInterface)
	{
		return DatabaseInterface->GetConnectionState();
	}
	return ENyxConnectionState::Disconnected;
}

void UNyxNetworkSubsystem::UpdateSpatialSubscription(const FVector& PlayerPosition)
{
	if (!DatabaseInterface || DatabaseInterface->GetConnectionState() != ENyxConnectionState::Connected)
	{
		return;
	}

	// Calculate current chunk
	FIntPoint CurrentChunk;
	CurrentChunk.X = FMath::FloorToInt(PlayerPosition.X / ChunkSizeUnreal);
	CurrentChunk.Y = FMath::FloorToInt(PlayerPosition.Y / ChunkSizeUnreal);

	// Only update subscription if chunk changed
	if (CurrentChunk == LastChunkCoord)
	{
		return;
	}

	UE_LOG(LogNyxNet, Log, TEXT("Player moved to chunk (%d, %d), updating spatial subscription"),
		CurrentChunk.X, CurrentChunk.Y);

	LastChunkCoord = CurrentChunk;

	// Unsubscribe from previous spatial query
	if (CurrentSpatialSubscriptionHandle != INDEX_NONE)
	{
		DatabaseInterface->Unsubscribe(CurrentSpatialSubscriptionHandle);
	}

	// Build spatial subscription queries
	// Subscribe to player table for nearby chunks
	const int32 MinChunkX = CurrentChunk.X - SubscriptionRadius;
	const int32 MaxChunkX = CurrentChunk.X + SubscriptionRadius;
	const int32 MinChunkY = CurrentChunk.Y - SubscriptionRadius;
	const int32 MaxChunkY = CurrentChunk.Y + SubscriptionRadius;

	TArray<FString> Queries;

	// Player positions in nearby chunks
	Queries.Add(FString::Printf(
		TEXT("SELECT * FROM player WHERE chunk_x >= %d AND chunk_x <= %d AND chunk_y >= %d AND chunk_y <= %d"),
		MinChunkX, MaxChunkX, MinChunkY, MaxChunkY));

	// TODO: Add more table subscriptions as needed (NPCs, items, etc.)

	CurrentSpatialSubscriptionHandle = DatabaseInterface->Subscribe(Queries);
}

void UNyxNetworkSubsystem::HandleConnectionStateChanged(ENyxConnectionState NewState)
{
	UE_LOG(LogNyxNet, Log, TEXT("Connection state changed to: %d"), static_cast<int32>(NewState));
	OnConnectionStateChangedBP.Broadcast(NewState);
}
