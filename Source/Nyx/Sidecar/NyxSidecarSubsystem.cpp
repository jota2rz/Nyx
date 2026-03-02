// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxSidecarSubsystem.h"
#include "Nyx/Nyx.h"
#include "ModuleBindings/SpacetimeDBClient.g.h"
#include "ModuleBindings/Tables/PhysicsBodyTable.g.h"

DECLARE_STATS_GROUP(TEXT("NyxSidecar"), STATGROUP_NyxSidecar, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("Sidecar Tick"), STAT_SidecarTick, STATGROUP_NyxSidecar);

// ─── Subsystem Lifecycle ───────────────────────────────────────────

void UNyxSidecarSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogNyx, Log, TEXT("NyxSidecarSubsystem initialized"));
}

void UNyxSidecarSubsystem::Deinitialize()
{
	StopSidecar();
	UE_LOG(LogNyx, Log, TEXT("NyxSidecarSubsystem deinitialized"));
	Super::Deinitialize();
}

TStatId UNyxSidecarSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UNyxSidecarSubsystem, STATGROUP_NyxSidecar);
}

// ─── Sidecar Control ───────────────────────────────────────────────

void UNyxSidecarSubsystem::StartSidecar(const FString& Host, const FString& DatabaseName)
{
	if (bSidecarActive)
	{
		UE_LOG(LogNyx, Warning, TEXT("Sidecar already active, call StopSidecar first"));
		return;
	}

	UE_LOG(LogNyx, Log, TEXT("Starting sidecar — connecting to ws://%s database=%s"), *Host, *DatabaseName);

	// Build a second SpacetimeDB connection (separate identity from the game client)
	FOnConnectDelegate OnConnect;
	OnConnect.BindDynamic(this, &UNyxSidecarSubsystem::HandleConnect);

	FOnDisconnectDelegate OnDisconnect;
	OnDisconnect.BindDynamic(this, &UNyxSidecarSubsystem::HandleDisconnect);

	FOnConnectErrorDelegate OnConnectError;
	OnConnectError.BindDynamic(this, &UNyxSidecarSubsystem::HandleConnectError);

	UDbConnectionBuilder* Builder = UDbConnection::Builder();
	SidecarConnection = Builder
		->WithUri(FString::Printf(TEXT("ws://%s"), *Host))
		->WithDatabaseName(DatabaseName)
		->OnConnect(OnConnect)
		->OnConnectError(OnConnectError)
		->OnDisconnect(OnDisconnect)
		->Build();

	if (SidecarConnection)
	{
		SidecarConnection->SetAutoTicking(true);
		UE_LOG(LogNyx, Log, TEXT("Sidecar connection built — waiting for connect callback"));
	}
	else
	{
		UE_LOG(LogNyx, Error, TEXT("Sidecar connection builder returned null!"));
	}
}

void UNyxSidecarSubsystem::StopSidecar()
{
	if (!bSidecarActive && !SidecarConnection)
	{
		return;
	}

	// Unbind table events
	if (SidecarConnection && SidecarConnection->Db && SidecarConnection->Db->PhysicsBody)
	{
		SidecarConnection->Db->PhysicsBody->OnInsert.RemoveDynamic(this, &UNyxSidecarSubsystem::HandlePhysicsBodyInsert);
		SidecarConnection->Db->PhysicsBody->OnUpdate.RemoveDynamic(this, &UNyxSidecarSubsystem::HandlePhysicsBodyUpdate);
		SidecarConnection->Db->PhysicsBody->OnDelete.RemoveDynamic(this, &UNyxSidecarSubsystem::HandlePhysicsBodyDelete);
	}

	// Disconnect
	if (SidecarConnection)
	{
		SidecarConnection->Disconnect();
		SidecarConnection = nullptr;
	}

	TrackedBodies.Empty();
	bSidecarActive = false;

	UE_LOG(LogNyx, Log, TEXT("Sidecar stopped. Total bodies simulated: %d, Total updates sent: %d"),
		TotalBodiesSimulated, TotalUpdatesSent);
}

// ─── SpacetimeDB Callbacks ─────────────────────────────────────────

void UNyxSidecarSubsystem::HandleConnect(UDbConnection* Connection, FSpacetimeDBIdentity Identity, const FString& Token)
{
	UE_LOG(LogNyx, Log, TEXT("Sidecar connected to SpacetimeDB (identity: %s)"), *Identity.ToHex());

	// Subscribe to the physics_body table
	FOnSubscriptionApplied OnApplied;
	OnApplied.BindDynamic(this, &UNyxSidecarSubsystem::HandleSubscriptionApplied);

	FOnSubscriptionError OnError;
	OnError.BindDynamic(this, &UNyxSidecarSubsystem::HandleSubscriptionError);

	SidecarConnection->SubscriptionBuilder()
		->OnApplied(OnApplied)
		->OnError(OnError)
		->Subscribe({ TEXT("SELECT * FROM physics_body WHERE active = true") });

	UE_LOG(LogNyx, Log, TEXT("Sidecar subscribed to physics_body table"));
}

void UNyxSidecarSubsystem::HandleDisconnect(UDbConnection* Connection, const FString& Error)
{
	UE_LOG(LogNyx, Warning, TEXT("Sidecar disconnected: %s"), *Error);
	bSidecarActive = false;
}

void UNyxSidecarSubsystem::HandleConnectError(const FString& ErrorMessage)
{
	UE_LOG(LogNyx, Error, TEXT("Sidecar connection error: %s"), *ErrorMessage);
}

void UNyxSidecarSubsystem::HandleSubscriptionApplied(FSubscriptionEventContext Context)
{
	UE_LOG(LogNyx, Log, TEXT("Sidecar subscription applied — binding table events"));

	// Bind to physics_body table events
	UPhysicsBodyTable* PhysicsTable = SidecarConnection->Db->PhysicsBody;
	if (!PhysicsTable)
	{
		UE_LOG(LogNyx, Error, TEXT("Sidecar: PhysicsBody table not available!"));
		return;
	}

	PhysicsTable->OnInsert.AddDynamic(this, &UNyxSidecarSubsystem::HandlePhysicsBodyInsert);
	PhysicsTable->OnUpdate.AddDynamic(this, &UNyxSidecarSubsystem::HandlePhysicsBodyUpdate);
	PhysicsTable->OnDelete.AddDynamic(this, &UNyxSidecarSubsystem::HandlePhysicsBodyDelete);

	// Pick up any existing active bodies from the subscription snapshot
	TArray<FPhysicsBodyType> Existing = PhysicsTable->Iter();
	for (const FPhysicsBodyType& Body : Existing)
	{
		if (Body.Active)
		{
			FTrackedBody Tracked;
			Tracked.EntityId = Body.EntityId;
			Tracked.Position = FVector(Body.PosX, Body.PosY, Body.PosZ);
			Tracked.Velocity = FVector(Body.VelX, Body.VelY, Body.VelZ);
			Tracked.bActive = true;
			TrackedBodies.Add(Body.EntityId, Tracked);
			TotalBodiesSimulated++;
			UE_LOG(LogNyx, Log, TEXT("Sidecar: Adopted existing body %llu at (%.0f, %.0f, %.0f)"),
				Body.EntityId, Body.PosX, Body.PosY, Body.PosZ);
		}
	}

	bSidecarActive = true;
	UE_LOG(LogNyx, Log, TEXT("Sidecar active — simulating %d bodies at %.0f Hz send rate"),
		TrackedBodies.Num(), SendRateHz);
}

void UNyxSidecarSubsystem::HandleSubscriptionError(FErrorContext Context)
{
	UE_LOG(LogNyx, Error, TEXT("Sidecar subscription error"));
}

// ─── Physics Body Table Callbacks ──────────────────────────────────

void UNyxSidecarSubsystem::HandlePhysicsBodyInsert(const FEventContext& Context, const FPhysicsBodyType& NewRow)
{
	// Only track active bodies that we're not already simulating
	if (!NewRow.Active || TrackedBodies.Contains(NewRow.EntityId))
	{
		return;
	}

	FTrackedBody Tracked;
	Tracked.EntityId = NewRow.EntityId;
	Tracked.Position = FVector(NewRow.PosX, NewRow.PosY, NewRow.PosZ);
	Tracked.Velocity = FVector(NewRow.VelX, NewRow.VelY, NewRow.VelZ);
	Tracked.bActive = true;
	TrackedBodies.Add(NewRow.EntityId, Tracked);
	TotalBodiesSimulated++;

	UE_LOG(LogNyx, Log, TEXT("Sidecar: New body %llu (%s) at (%.0f, %.0f, %.0f) vel=(%.0f, %.0f, %.0f)"),
		NewRow.EntityId, *NewRow.BodyType,
		NewRow.PosX, NewRow.PosY, NewRow.PosZ,
		NewRow.VelX, NewRow.VelY, NewRow.VelZ);
}

void UNyxSidecarSubsystem::HandlePhysicsBodyUpdate(const FEventContext& Context, const FPhysicsBodyType& OldRow, const FPhysicsBodyType& NewRow)
{
	// Ignore updates for bodies we're tracking — we're the source of those updates.
	// This prevents feedback loops.
	if (TrackedBodies.Contains(NewRow.EntityId))
	{
		return;
	}

	// If it's a new body we haven't seen, adopt it
	if (NewRow.Active)
	{
		FTrackedBody Tracked;
		Tracked.EntityId = NewRow.EntityId;
		Tracked.Position = FVector(NewRow.PosX, NewRow.PosY, NewRow.PosZ);
		Tracked.Velocity = FVector(NewRow.VelX, NewRow.VelY, NewRow.VelZ);
		Tracked.bActive = true;
		TrackedBodies.Add(NewRow.EntityId, Tracked);
		TotalBodiesSimulated++;

		UE_LOG(LogNyx, Log, TEXT("Sidecar: Adopted body %llu from update"), NewRow.EntityId);
	}
}

void UNyxSidecarSubsystem::HandlePhysicsBodyDelete(const FEventContext& Context, const FPhysicsBodyType& DeletedRow)
{
	if (TrackedBodies.Remove(DeletedRow.EntityId) > 0)
	{
		UE_LOG(LogNyx, Log, TEXT("Sidecar: Body %llu deleted"), DeletedRow.EntityId);
	}
}

// ─── Tick / Physics ────────────────────────────────────────────────

void UNyxSidecarSubsystem::Tick(float DeltaTime)
{
	SCOPE_CYCLE_COUNTER(STAT_SidecarTick);

	if (!bSidecarActive || TrackedBodies.Num() == 0)
	{
		return;
	}

	StepPhysics(DeltaTime);
	SendPhysicsUpdates();
}

void UNyxSidecarSubsystem::StepPhysics(float DeltaTime)
{
	TArray<uint64> ToDeactivate;

	for (auto& Pair : TrackedBodies)
	{
		FTrackedBody& Body = Pair.Value;
		if (!Body.bActive)
		{
			continue;
		}

		// Euler integration
		Body.Velocity.Z += GravityZ * DeltaTime;
		Body.Position += Body.Velocity * DeltaTime;

		// Accumulate send timer
		Body.TimeSinceLastSend += DeltaTime;

		// Floor collision — deactivate if below floor
		if (Body.Position.Z <= FloorZ)
		{
			Body.Position.Z = FloorZ;
			Body.Velocity = FVector::ZeroVector;
			Body.bActive = false;
			ToDeactivate.Add(Body.EntityId);

			UE_LOG(LogNyx, Log, TEXT("Sidecar: Body %llu hit floor at (%.0f, %.0f, %.0f)"),
				Body.EntityId, Body.Position.X, Body.Position.Y, Body.Position.Z);
		}
	}

	// Send deactivation updates immediately
	if (SidecarConnection && SidecarConnection->Reducers && ToDeactivate.Num() > 0)
	{
		for (uint64 Id : ToDeactivate)
		{
			FTrackedBody& Body = TrackedBodies[Id];
			SidecarConnection->Reducers->PhysicsUpdate(
				Id,
				Body.Position.X, Body.Position.Y, Body.Position.Z,
				Body.Velocity.X, Body.Velocity.Y, Body.Velocity.Z,
				false // active = false
			);
			TotalUpdatesSent++;
			UE_LOG(LogNyx, Log, TEXT("Sidecar: Sent deactivation for body %llu"), Id);
		}
	}
}

void UNyxSidecarSubsystem::SendPhysicsUpdates()
{
	if (!SidecarConnection || !SidecarConnection->Reducers)
	{
		return;
	}

	for (auto& Pair : TrackedBodies)
	{
		FTrackedBody& Body = Pair.Value;

		// Only send at the configured rate
		if (Body.TimeSinceLastSend < SendInterval || !Body.bActive)
		{
			continue;
		}

		Body.TimeSinceLastSend = 0.f;

		SidecarConnection->Reducers->PhysicsUpdate(
			Body.EntityId,
			Body.Position.X, Body.Position.Y, Body.Position.Z,
			Body.Velocity.X, Body.Velocity.Y, Body.Velocity.Z,
			true // active
		);
		TotalUpdatesSent++;

		UE_LOG(LogNyx, Verbose, TEXT("Sidecar: Body %llu → pos=(%.1f, %.1f, %.1f) vel=(%.1f, %.1f, %.1f)"),
			Body.EntityId,
			Body.Position.X, Body.Position.Y, Body.Position.Z,
			Body.Velocity.X, Body.Velocity.Y, Body.Velocity.Z);
	}
}
