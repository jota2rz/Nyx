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

void UNyxServerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogNyxServer, Log, TEXT("NyxServerSubsystem initialized — standby mode (inert until ConnectAndRegister)"));
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
	EarlyJoinQueue.Empty();
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

	// ── Process players who joined before SpacetimeDB was ready ──
	// In PIE listen server, PostLogin for the local player fires BEFORE
	// StartPlay, so their OnPlayerJoined call gets queued.
	if (EarlyJoinQueue.Num() > 0)
	{
		UE_LOG(LogNyxServer, Log, TEXT("Processing %d early-join queued player(s)..."), EarlyJoinQueue.Num());
		TMap<FString, TObjectPtr<ANyxCharacter>> QueueCopy = EarlyJoinQueue;
		EarlyJoinQueue.Empty();
		for (auto& Pair : QueueCopy)
		{
			OnPlayerJoined(Pair.Value.Get(), Pair.Key);
		}
	}
}

void UNyxServerSubsystem::HandleSubscriptionError(FErrorContext Context)
{
	UE_LOG(LogNyxServer, Error, TEXT("SpacetimeDB subscription error: %s"), *Context.Error);
}

// ─── Character Lifecycle ───────────────────────────────────────────

void UNyxServerSubsystem::OnPlayerJoined(ANyxCharacter* Character, const FString& PlayerDisplayName)
{
	if (!SpacetimeDBConnection || !SpacetimeDBConnection->Reducers || !bRegistered)
	{
		// PostLogin fires before StartPlay in listen server mode,
		// so the SpacetimeDB connection may not exist yet. Queue for later.
		EarlyJoinQueue.Add(PlayerDisplayName, Character);
		UE_LOG(LogNyxServer, Log, TEXT("Player joined before SpacetimeDB ready, queued: %s"), *PlayerDisplayName);
		return;
	}

	// Generate a deterministic per-player identity from the display name.
	// In production, this would come from EOS auth (client authenticates →
	// server receives verified identity). For now, we deterministically
	// derive a unique identity from the player's name.
	const FSpacetimeDBIdentity PlayerIdentity = GeneratePlayerIdentity(PlayerDisplayName);
	const FString Key = PlayerDisplayName;

	UE_LOG(LogNyxServer, Log, TEXT("Player joined: %s (identity=%s...)"),
		*PlayerDisplayName, *PlayerIdentity.ToHex().Left(16));

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
				UE_LOG(LogNyxServer, Log, TEXT("Character loaded from cache: %s (HP=%d/%d)"),
					*Stats.DisplayName, Stats.CurrentHp, Stats.MaxHp);
				return;
			}
		}
	}

	// Not in cache — request load and wait for OnInsert callback
	PendingLoads.Add(Key, Character);
	ManagedCharacters.Add(Key, Character);

	SpacetimeDBConnection->Reducers->LoadCharacter(PlayerIdentity, PlayerDisplayName);
	UE_LOG(LogNyxServer, Log, TEXT("Requested character load: %s"), *PlayerDisplayName);
}

void UNyxServerSubsystem::OnPlayerLeft(ANyxCharacter* Character)
{
	if (!Character) return;

	// Save character state before removing
	SaveCharacterState(Character);

	// Use DisplayName as key (matches OnPlayerJoined key)
	const FString Key = Character->GetDisplayName();
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
	else
	{
		// Character not managed by us — another server saved this character.
		// Could be a migration signal: the other server released the player
		// and saved their last position before handing off.
		if (OldRow.SavedPosX != NewRow.SavedPosX || OldRow.SavedPosY != NewRow.SavedPosY || OldRow.SavedPosZ != NewRow.SavedPosZ)
		{
			UE_LOG(LogNyxServer, Log, TEXT("External character save detected: %s pos=(%.0f,%.0f,%.0f) zone=%s"),
				*NewRow.DisplayName, NewRow.SavedPosX, NewRow.SavedPosY, NewRow.SavedPosZ, *NewRow.SavedZoneId);
			OnExternalCharacterSaved.Broadcast(NewRow);
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
	const float Yaw = Character->GetActorRotation().Yaw;

	SpacetimeDBConnection->Reducers->SaveCharacter(
		Character->SpacetimeIdentity,
		Character->GetCurrentHP(),
		Character->GetCurrentMP(),
		Pos.X, Pos.Y, Pos.Z,
		Yaw,
		ZoneId);

	UE_LOG(LogNyxServer, Verbose, TEXT("Saved %s pos=(%.0f,%.0f,%.0f) yaw=%.1f HP=%d"),
		*Character->GetDisplayName(), Pos.X, Pos.Y, Pos.Z, Yaw, Character->GetCurrentHP());
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

FSpacetimeDBIdentity UNyxServerSubsystem::GeneratePlayerIdentity(const FString& PlayerDisplayName)
{
	// Deterministic per-player identity: MD5 hash of display name, doubled to fill 32 bytes.
	// In production, this is replaced by the real SpacetimeDB identity from EOS auth.
	FMD5 Md5Gen;
	FTCHARToUTF8 NameUtf8(*PlayerDisplayName);
	Md5Gen.Update(reinterpret_cast<const uint8*>(NameUtf8.Get()), NameUtf8.Length());
	uint8 Digest[16];
	Md5Gen.Final(Digest);

	TArray<uint8> IdentityBytes;
	IdentityBytes.SetNumUninitialized(32);
	FMemory::Memcpy(IdentityBytes.GetData(), Digest, 16);
	// XOR with index to make second half different from first
	for (int32 i = 0; i < 16; i++)
	{
		IdentityBytes[16 + i] = Digest[i] ^ static_cast<uint8>(i + 0xA5);
	}

	return FSpacetimeDBIdentity::FromBigEndian(IdentityBytes);
}
