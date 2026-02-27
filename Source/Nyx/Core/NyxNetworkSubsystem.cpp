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

		// Bind to connection state changes
		DatabaseInterface->OnConnectionStateChanged().AddUObject(this, &UNyxNetworkSubsystem::HandleConnectionStateChanged);

		// Connect
		DatabaseInterface->Connect(Host, DatabaseName);
	}
	else
	{
		UE_LOG(LogNyxNet, Log, TEXT("Creating SpacetimeDB connection to ws://%s database=%s"), *Host, *DatabaseName);

		// Build up connect/disconnect delegates
		FOnConnectDelegate OnConnect;
		OnConnect.BindDynamic(this, &UNyxNetworkSubsystem::HandleSpacetimeDBConnect);

		FOnDisconnectDelegate OnDisconnect;
		OnDisconnect.BindDynamic(this, &UNyxNetworkSubsystem::HandleSpacetimeDBDisconnect);

		FOnConnectErrorDelegate OnConnectError;
		OnConnectError.BindDynamic(this, &UNyxNetworkSubsystem::HandleSpacetimeDBConnectError);

		// Build connection
		UDbConnectionBuilder* Builder = UDbConnection::Builder();
		SpacetimeDBConnection = Builder
			->WithUri(FString::Printf(TEXT("ws://%s"), *Host))
			->WithDatabaseName(DatabaseName)
			->OnConnect(OnConnect)
			->OnConnectError(OnConnectError)
			->OnDisconnect(OnDisconnect)
			->Build();

		if (SpacetimeDBConnection)
		{
			// Enable auto-ticking so the SDK processes incoming WebSocket messages each frame
			SpacetimeDBConnection->SetAutoTicking(true);

			HandleConnectionStateChanged(ENyxConnectionState::Connecting);
			UE_LOG(LogNyxNet, Log, TEXT("SpacetimeDB connection builder complete — auto-tick enabled, waiting for connect callback"));
		}
		else
		{
			UE_LOG(LogNyxNet, Error, TEXT("SpacetimeDB connection builder returned null!"));
			HandleConnectionStateChanged(ENyxConnectionState::Disconnected);
		}
	}
}

void UNyxNetworkSubsystem::DisconnectFromServer()
{
	if (SpacetimeDBConnection)
	{
		SpacetimeDBConnection->Disconnect();
		SpacetimeDBConnection = nullptr;
		HandleConnectionStateChanged(ENyxConnectionState::Disconnected);
	}

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
	// Check real SpacetimeDB connection first
	if (SpacetimeDBConnection && SpacetimeDBConnection->IsActive())
	{
		return ENyxConnectionState::Connected;
	}
	// Fall back to mock interface
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

void UNyxNetworkSubsystem::HandleSpacetimeDBConnect(UDbConnection* Connection, FSpacetimeDBIdentity Identity, const FString& Token)
{
	UE_LOG(LogNyxNet, Log, TEXT("SpacetimeDB connected! Token length=%d"), Token.Len());
	SpacetimeDBToken = Token;

	HandleConnectionStateChanged(ENyxConnectionState::Connected);

	// Subscribe to all players
	USubscriptionBuilder* SubBuilder = Connection->SubscriptionBuilder();

	FOnSubscriptionApplied OnApplied;
	OnApplied.BindDynamic(this, &UNyxNetworkSubsystem::HandleSubscriptionApplied);

	FOnSubscriptionError OnError;
	OnError.BindDynamic(this, &UNyxNetworkSubsystem::HandleSubscriptionError);

	SubBuilder->OnApplied(OnApplied);
	SubBuilder->OnError(OnError);
	SubBuilder->Subscribe({TEXT("SELECT * FROM player")});

	UE_LOG(LogNyxNet, Log, TEXT("Subscribed to player table"));
}

void UNyxNetworkSubsystem::HandleSpacetimeDBDisconnect(UDbConnection* Connection, const FString& Error)
{
	if (Error.IsEmpty())
	{
		UE_LOG(LogNyxNet, Log, TEXT("SpacetimeDB disconnected cleanly"));
	}
	else
	{
		UE_LOG(LogNyxNet, Warning, TEXT("SpacetimeDB disconnected with error: %s"), *Error);
	}

	SpacetimeDBConnection = nullptr;
	HandleConnectionStateChanged(ENyxConnectionState::Disconnected);
}

void UNyxNetworkSubsystem::HandleSpacetimeDBConnectError(const FString& ErrorMessage)
{
	UE_LOG(LogNyxNet, Error, TEXT("SpacetimeDB connection error: %s"), *ErrorMessage);
	SpacetimeDBConnection = nullptr;
	HandleConnectionStateChanged(ENyxConnectionState::Disconnected);
}

void UNyxNetworkSubsystem::HandleSubscriptionApplied(FSubscriptionEventContext Context)
{
	UE_LOG(LogNyxNet, Log, TEXT("SpacetimeDB subscription applied successfully!"));

	// Test: call create_player reducer to verify round-trip
	if (Context.Reducers)
	{
		UE_LOG(LogNyxNet, Log, TEXT("Calling create_player reducer..."));
		Context.Reducers->CreatePlayer(TEXT("NyxTestPlayer"));
	}
}

void UNyxNetworkSubsystem::HandleSubscriptionError(FErrorContext Context)
{
	UE_LOG(LogNyxNet, Error, TEXT("SpacetimeDB subscription error: %s"), *Context.Error);
}
