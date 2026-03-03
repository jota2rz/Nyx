// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxServerSubsystem.h"
#include "Nyx/Nyx.h"
#include "Nyx/Player/NyxCharacter.h"
#include "ModuleBindings/SpacetimeDBClient.g.h"
#include "ModuleBindings/Tables/CharacterStatsTable.g.h"
#include "TimerManager.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogNyxServer, Log, All);

// ─── Subsystem Lifecycle ───────────────────────────────────────────

bool UNyxServerSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Only create on dedicated servers (or listen servers)
	return IsRunningDedicatedServer();
}

void UNyxServerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogNyxServer, Log, TEXT("NyxServerSubsystem initialized (dedicated server mode)"));
}

void UNyxServerSubsystem::Deinitialize()
{
	Shutdown();
	UE_LOG(LogNyxServer, Log, TEXT("NyxServerSubsystem deinitialized"));
	Super::Deinitialize();
}

// ─── Connection ────────────────────────────────────────────────────

void UNyxServerSubsystem::ConnectAndRegister(const FString& Host, const FString& DatabaseName,
	const FString& InZoneId, const FString& InServerId, int32 InMaxEntities)
{
	ZoneId = InZoneId;
	ServerId = InServerId;
	MaxEntities = InMaxEntities;

	UE_LOG(LogNyxServer, Log, TEXT("Connecting to SpacetimeDB ws://%s database=%s (Zone=%s, Server=%s, MaxEntities=%u)"),
		*Host, *DatabaseName, *ZoneId, *ServerId, MaxEntities);

	FOnConnectDelegate OnConnect;
	OnConnect.BindDynamic(this, &UNyxServerSubsystem::HandleConnect);

	FOnDisconnectDelegate OnDisconnect;
	OnDisconnect.BindDynamic(this, &UNyxServerSubsystem::HandleDisconnect);

	FOnConnectErrorDelegate OnConnectError;
	OnConnectError.BindDynamic(this, &UNyxServerSubsystem::HandleConnectError);

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
		SpacetimeDBConnection->SetAutoTicking(true);
	}
	else
	{
		UE_LOG(LogNyxServer, Error, TEXT("SpacetimeDB connection builder returned null!"));
	}
}

void UNyxServerSubsystem::Shutdown()
{
	StopHeartbeat();
	StopAutoSave();

	if (SpacetimeDBConnection && SpacetimeDBConnection->Reducers && bRegistered)
	{
		SpacetimeDBConnection->Reducers->DeregisterZoneServer(ServerId);
		UE_LOG(LogNyxServer, Log, TEXT("Deregistered zone server: %s"), *ServerId);
	}

	// Save all managed characters before disconnecting
	AutoSaveAllCharacters();

	if (SpacetimeDBConnection)
	{
		SpacetimeDBConnection->Disconnect();
		SpacetimeDBConnection = nullptr;
	}

	ManagedCharacters.Empty();
	PendingLoads.Empty();
	bRegistered = false;
}

// ─── SpacetimeDB Callbacks ─────────────────────────────────────────

void UNyxServerSubsystem::HandleConnect(UDbConnection* Connection, FSpacetimeDBIdentity Identity, const FString& Token)
{
	UE_LOG(LogNyxServer, Log, TEXT("SpacetimeDB connected. Subscribing to server tables..."));

	// Subscribe to character_stats and zone_server for this zone
	USubscriptionBuilder* SubBuilder = Connection->SubscriptionBuilder();

	FOnSubscriptionApplied OnApplied;
	OnApplied.BindDynamic(this, &UNyxServerSubsystem::HandleSubscriptionApplied);

	FOnSubscriptionError OnError;
	OnError.BindDynamic(this, &UNyxServerSubsystem::HandleSubscriptionError);

	SubBuilder->OnApplied(OnApplied);
	SubBuilder->OnError(OnError);

	// Subscribe to all character_stats (server needs full access) and zone config
	SubBuilder->Subscribe({
		TEXT("SELECT * FROM character_stats"),
		TEXT("SELECT * FROM combat_event"),
		FString::Printf(TEXT("SELECT * FROM zone_server WHERE zone_id = '%s'"), *ZoneId),
		FString::Printf(TEXT("SELECT * FROM zone_population WHERE zone_id = '%s'"), *ZoneId)
	});
}

void UNyxServerSubsystem::HandleDisconnect(UDbConnection* Connection, const FString& Error)
{
	if (Error.IsEmpty())
	{
		UE_LOG(LogNyxServer, Log, TEXT("SpacetimeDB disconnected cleanly"));
	}
	else
	{
		UE_LOG(LogNyxServer, Error, TEXT("SpacetimeDB disconnected with error: %s"), *Error);
	}

	StopHeartbeat();
	StopAutoSave();
	bRegistered = false;
}

void UNyxServerSubsystem::HandleConnectError(const FString& ErrorMessage)
{
	UE_LOG(LogNyxServer, Error, TEXT("SpacetimeDB connection error: %s"), *ErrorMessage);
}

void UNyxServerSubsystem::HandleSubscriptionApplied(FSubscriptionEventContext Context)
{
	UE_LOG(LogNyxServer, Log, TEXT("SpacetimeDB subscription applied. Registering zone server..."));

	// Register this zone server with SpacetimeDB
	if (SpacetimeDBConnection && SpacetimeDBConnection->Reducers)
	{
		// Get container IP — use the local hostname for now
		FString ContainerIP = TEXT("127.0.0.1");

		SpacetimeDBConnection->Reducers->RegisterZoneServer(
			ServerId, ZoneId, ContainerIP, 7777, MaxEntities);

		bRegistered = true;
		UE_LOG(LogNyxServer, Log, TEXT("Zone server registered: %s (zone=%s, maxEntities=%u)"),
			*ServerId, *ZoneId, MaxEntities);
	}

	// Bind to character_stats table events
	if (SpacetimeDBConnection && SpacetimeDBConnection->Db && SpacetimeDBConnection->Db->CharacterStats)
	{
		SpacetimeDBConnection->Db->CharacterStats->OnInsert.AddDynamic(
			this, &UNyxServerSubsystem::HandleCharacterStatsInsert);
		SpacetimeDBConnection->Db->CharacterStats->OnUpdate.AddDynamic(
			this, &UNyxServerSubsystem::HandleCharacterStatsUpdate);
	}

	// Start heartbeat and auto-save timers
	StartHeartbeat();
	StartAutoSave();
}

void UNyxServerSubsystem::HandleSubscriptionError(FErrorContext Context)
{
	UE_LOG(LogNyxServer, Error, TEXT("SpacetimeDB subscription error: %s"), *Context.Error);
}

// ─── Character Lifecycle ───────────────────────────────────────────

void UNyxServerSubsystem::OnPlayerJoined(ANyxCharacter* Character, const FString& PlayerDisplayName)
{
	if (!SpacetimeDBConnection || !SpacetimeDBConnection->Reducers)
	{
		UE_LOG(LogNyxServer, Error, TEXT("Cannot load character — no SpacetimeDB connection"));
		return;
	}

	// TODO: In the full auth pipeline, the SpacetimeDB identity comes from
	// EOS auth (client authenticates → server receives verified identity).
	// For now, generate a deterministic placeholder identity from the display name.
	// The LoadCharacter reducer accepts an explicit identity parameter,
	// so the server can pass whatever identity it wants.

	// Use display name as a temporary key until proper auth is wired up
	const FString Key = PlayerDisplayName;

	// Check if stats are already in the local cache
	if (SpacetimeDBConnection->Db && SpacetimeDBConnection->Db->CharacterStats)
	{
		TArray<FCharacterStatsType> AllStats = SpacetimeDBConnection->Db->CharacterStats->Iter();
		for (const FCharacterStatsType& Stats : AllStats)
		{
			if (Stats.DisplayName == PlayerDisplayName)
			{
				// Already cached — apply immediately
				Character->ApplyCharacterStats(Stats);
				ManagedCharacters.Add(Key, Character);
				OnCharacterLoaded.Broadcast(Character, Stats);
				UE_LOG(LogNyxServer, Log, TEXT("Character loaded from cache: %s"), *Stats.DisplayName);
				return;
			}
		}
	}

	// Not in cache — request load and wait for OnInsert callback
	PendingLoads.Add(Key, Character);
	ManagedCharacters.Add(Key, Character);

	// LoadCharacter reducer takes (Identity, DisplayName).
	// Since the server has a single SpacetimeDB connection, we pass its own identity
	// as a placeholder. The Rust module will create/find the character_stats row.
	// TODO: Pass the actual player SpacetimeDB identity from EOS auth.
	FSpacetimeDBIdentity ServerIdentity;
	SpacetimeDBConnection->TryGetIdentity(ServerIdentity);
	SpacetimeDBConnection->Reducers->LoadCharacter(ServerIdentity, PlayerDisplayName);
	UE_LOG(LogNyxServer, Log, TEXT("Requested character load for %s"), *PlayerDisplayName);
}

void UNyxServerSubsystem::OnPlayerLeft(ANyxCharacter* Character)
{
	if (!Character) return;

	// Save character state before removing
	SaveCharacterState(Character);

	const FString Key = IdentityKey(Character->SpacetimeIdentity);
	ManagedCharacters.Remove(Key);
	PendingLoads.Remove(Key);

	UE_LOG(LogNyxServer, Log, TEXT("Player left: %s"), *Character->GetDisplayName());
}

// ─── Table Callbacks ───────────────────────────────────────────────

void UNyxServerSubsystem::HandleCharacterStatsInsert(const FEventContext& Context, const FCharacterStatsType& NewRow)
{
	// Use display name as key (matches OnPlayerJoined key)
	const FString Key = NewRow.DisplayName;

	// Check if this is a pending load
	TObjectPtr<ANyxCharacter>* PendingPtr = PendingLoads.Find(Key);
	if (PendingPtr && IsValid(*PendingPtr))
	{
		ANyxCharacter* Character = PendingPtr->Get();
		Character->ApplyCharacterStats(NewRow);
		PendingLoads.Remove(Key);
		OnCharacterLoaded.Broadcast(Character, NewRow);
		UE_LOG(LogNyxServer, Log, TEXT("Pending character loaded: %s (HP=%d/%d)"),
			*NewRow.DisplayName, NewRow.CurrentHp, NewRow.MaxHp);
	}
}

void UNyxServerSubsystem::HandleCharacterStatsUpdate(const FEventContext& Context,
	const FCharacterStatsType& OldRow, const FCharacterStatsType& NewRow)
{
	const FString Key = NewRow.DisplayName;

	// Find the managed character and apply updated stats
	TObjectPtr<ANyxCharacter>* CharPtr = ManagedCharacters.Find(Key);
	if (CharPtr && IsValid(*CharPtr))
	{
		ANyxCharacter* Character = CharPtr->Get();

		// Update HP/MP from combat resolution
		if (OldRow.CurrentHp != NewRow.CurrentHp)
		{
			Character->SetCurrentHP(NewRow.CurrentHp);
		}
		if (OldRow.CurrentMp != NewRow.CurrentMp)
		{
			Character->SetCurrentMP(NewRow.CurrentMp);
		}

		// Update level/stats from progression
		if (OldRow.Level != NewRow.Level)
		{
			Character->ApplyCharacterStats(NewRow);
			UE_LOG(LogNyxServer, Log, TEXT("%s leveled up to %u!"), *NewRow.DisplayName, NewRow.Level);
		}
	}
}

// ─── Combat ────────────────────────────────────────────────────────

void UNyxServerSubsystem::RequestResolveHit(const FSpacetimeDBIdentity& AttackerId,
	const FSpacetimeDBIdentity& DefenderId, uint32 SkillId)
{
	if (!SpacetimeDBConnection || !SpacetimeDBConnection->Reducers) return;

	SpacetimeDBConnection->Reducers->ResolveHit(AttackerId, DefenderId, SkillId);
}

void UNyxServerSubsystem::RequestResolveHeal(const FSpacetimeDBIdentity& HealerId,
	const FSpacetimeDBIdentity& TargetId, uint32 SkillId)
{
	if (!SpacetimeDBConnection || !SpacetimeDBConnection->Reducers) return;

	SpacetimeDBConnection->Reducers->ResolveHeal(HealerId, TargetId, SkillId);
}

// ─── Save ──────────────────────────────────────────────────────────

void UNyxServerSubsystem::SaveCharacterState(ANyxCharacter* Character)
{
	if (!SpacetimeDBConnection || !SpacetimeDBConnection->Reducers || !Character) return;

	const FVector Pos = Character->GetActorLocation();

	SpacetimeDBConnection->Reducers->SaveCharacter(
		Character->SpacetimeIdentity,
		Character->GetCurrentHP(),
		Character->GetCurrentMP(),
		Pos.X, Pos.Y, Pos.Z,
		ZoneId);

	UE_LOG(LogNyxServer, Verbose, TEXT("Saved %s pos=(%.0f,%.0f,%.0f) HP=%d"),
		*Character->GetDisplayName(), Pos.X, Pos.Y, Pos.Z, Character->GetCurrentHP());
}

// ─── Heartbeat ─────────────────────────────────────────────────────

void UNyxServerSubsystem::StartHeartbeat()
{
	UWorld* World = GetGameInstance()->GetWorld();
	if (World)
	{
		World->GetTimerManager().SetTimer(HeartbeatTimerHandle,
			FTimerDelegate::CreateUObject(this, &UNyxServerSubsystem::SendHeartbeat),
			HeartbeatInterval, true);
		UE_LOG(LogNyxServer, Log, TEXT("Heartbeat started (%.0fs interval)"), HeartbeatInterval);
	}
}

void UNyxServerSubsystem::StopHeartbeat()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (World)
	{
		World->GetTimerManager().ClearTimer(HeartbeatTimerHandle);
	}
}

void UNyxServerSubsystem::SendHeartbeat()
{
	if (!SpacetimeDBConnection || !SpacetimeDBConnection->Reducers) return;

	const uint32 EntityCount = ManagedCharacters.Num();
	SpacetimeDBConnection->Reducers->ServerHeartbeat(ServerId, EntityCount);
}

// ─── Auto-Save ─────────────────────────────────────────────────────

void UNyxServerSubsystem::StartAutoSave()
{
	UWorld* World = GetGameInstance()->GetWorld();
	if (World)
	{
		World->GetTimerManager().SetTimer(AutoSaveTimerHandle,
			FTimerDelegate::CreateUObject(this, &UNyxServerSubsystem::AutoSaveAllCharacters),
			AutoSaveInterval, true);
		UE_LOG(LogNyxServer, Log, TEXT("Auto-save started (%.0fs interval)"), AutoSaveInterval);
	}
}

void UNyxServerSubsystem::StopAutoSave()
{
	UWorld* World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
	if (World)
	{
		World->GetTimerManager().ClearTimer(AutoSaveTimerHandle);
	}
}

void UNyxServerSubsystem::AutoSaveAllCharacters()
{
	int32 SavedCount = 0;
	for (auto& Pair : ManagedCharacters)
	{
		if (IsValid(Pair.Value))
		{
			SaveCharacterState(Pair.Value);
			SavedCount++;
		}
	}

	if (SavedCount > 0)
	{
		UE_LOG(LogNyxServer, Log, TEXT("Auto-saved %d characters"), SavedCount);
	}
}

// ─── Helpers ───────────────────────────────────────────────────────

FString UNyxServerSubsystem::IdentityKey(const FSpacetimeDBIdentity& Id)
{
	return Id.ToHex();
}
