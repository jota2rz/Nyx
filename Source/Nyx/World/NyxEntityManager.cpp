// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxEntityManager.h"
#include "Nyx/Nyx.h"
#include "Nyx/Core/NyxNetworkSubsystem.h"
#include "Nyx/Player/NyxPlayerPawn.h"
#include "Nyx/Player/NyxMovementComponent.h"
#include "ModuleBindings/Tables/PlayerTable.g.h"
#include "ModuleBindings/SpacetimeDBClient.g.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

void UNyxEntityManager::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogNyxWorld, Log, TEXT("NyxEntityManager initialized"));
}

void UNyxEntityManager::Deinitialize()
{
	StopListening();
	UE_LOG(LogNyxWorld, Log, TEXT("NyxEntityManager deinitialized"));
	Super::Deinitialize();
}

bool UNyxEntityManager::ShouldCreateSubsystem(UObject* Outer) const
{
	UWorld* World = Cast<UWorld>(Outer);
	if (!World) return false;
	return World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE;
}

// ─── Listening ─────────────────────────────────────────────────────

void UNyxEntityManager::StartListening()
{
	if (bIsListening) return;

	UGameInstance* GI = GetWorld()->GetGameInstance();
	if (!GI) return;

	UNyxNetworkSubsystem* NetworkSub = GI->GetSubsystem<UNyxNetworkSubsystem>();
	if (!NetworkSub) return;

	UDbConnection* Conn = NetworkSub->GetSpacetimeDBConnection();
	if (!Conn || !Conn->Db || !Conn->Db->Player)
	{
		UE_LOG(LogNyxWorld, Error, TEXT("Cannot start listening — SpacetimeDB connection or Player table not available"));
		return;
	}

	UPlayerTable* PlayerTable = Conn->Db->Player;

	PlayerTable->OnInsert.AddDynamic(this, &UNyxEntityManager::HandlePlayerInsert);
	PlayerTable->OnUpdate.AddDynamic(this, &UNyxEntityManager::HandlePlayerUpdate);
	PlayerTable->OnDelete.AddDynamic(this, &UNyxEntityManager::HandlePlayerDelete);

	bIsListening = true;
	UE_LOG(LogNyxWorld, Log, TEXT("NyxEntityManager now listening for PlayerTable events"));

	// Spawn actors for any players already in the cache (subscription snapshot)
	TArray<FPlayerType> ExistingPlayers = PlayerTable->Iter();
	for (const FPlayerType& Player : ExistingPlayers)
	{
		bool bLocal = NetworkSub->IsLocalIdentity(Player.Identity);
		SpawnPlayerActor(Player, bLocal);
		UE_LOG(LogNyxWorld, Log, TEXT("Spawned existing player: %s (%s)"),
			*Player.DisplayName, bLocal ? TEXT("local") : TEXT("remote"));
	}
}

void UNyxEntityManager::StopListening()
{
	if (!bIsListening) return;

	// Unbind table events
	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (GI)
	{
		UNyxNetworkSubsystem* NetworkSub = GI->GetSubsystem<UNyxNetworkSubsystem>();
		if (NetworkSub)
		{
			UDbConnection* Conn = NetworkSub->GetSpacetimeDBConnection();
			if (Conn && Conn->Db && Conn->Db->Player)
			{
				Conn->Db->Player->OnInsert.RemoveDynamic(this, &UNyxEntityManager::HandlePlayerInsert);
				Conn->Db->Player->OnUpdate.RemoveDynamic(this, &UNyxEntityManager::HandlePlayerUpdate);
				Conn->Db->Player->OnDelete.RemoveDynamic(this, &UNyxEntityManager::HandlePlayerDelete);
			}
		}
	}

	// Destroy all managed actors
	for (auto& Pair : ManagedPlayers)
	{
		if (Pair.Value && IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	ManagedPlayers.Empty();
	LocalPlayerPawn = nullptr;

	bIsListening = false;
	UE_LOG(LogNyxWorld, Log, TEXT("NyxEntityManager stopped listening"));
}

// ─── Accessors ─────────────────────────────────────────────────────

AActor* UNyxEntityManager::GetPlayerActor(const FSpacetimeDBIdentity& Identity) const
{
	const FString Key = IdentityKey(Identity);
	const TObjectPtr<AActor>* Found = ManagedPlayers.Find(Key);
	return Found ? Found->Get() : nullptr;
}

FString UNyxEntityManager::IdentityKey(const FSpacetimeDBIdentity& Id)
{
	return Id.ToHex();
}

// ─── Table Event Handlers ──────────────────────────────────────────

void UNyxEntityManager::HandlePlayerInsert(const FEventContext& Context, const FPlayerType& NewRow)
{
	const FString Key = IdentityKey(NewRow.Identity);

	// Guard against duplicates (can happen on subscription re-apply)
	if (ManagedPlayers.Contains(Key))
	{
		UE_LOG(LogNyxWorld, Verbose, TEXT("Player %s already managed, skipping insert"), *NewRow.DisplayName);
		return;
	}

	UGameInstance* GI = GetWorld()->GetGameInstance();
	UNyxNetworkSubsystem* NetworkSub = GI ? GI->GetSubsystem<UNyxNetworkSubsystem>() : nullptr;
	bool bIsLocal = NetworkSub && NetworkSub->IsLocalIdentity(NewRow.Identity);

	AActor* Actor = SpawnPlayerActor(NewRow, bIsLocal);
	if (Actor)
	{
		UE_LOG(LogNyxWorld, Log, TEXT("Player inserted: %s (%s) at (%.0f, %.0f, %.0f)"),
			*NewRow.DisplayName, bIsLocal ? TEXT("LOCAL") : TEXT("REMOTE"),
			NewRow.PosX, NewRow.PosY, NewRow.PosZ);
	}
}

void UNyxEntityManager::HandlePlayerUpdate(const FEventContext& Context, const FPlayerType& OldRow, const FPlayerType& NewRow)
{
	const FString Key = IdentityKey(NewRow.Identity);
	TObjectPtr<AActor>* ActorPtr = ManagedPlayers.Find(Key);

	if (!ActorPtr || !IsValid(*ActorPtr))
	{
		// Actor doesn't exist — spawn it (can happen if insert was missed)
		UE_LOG(LogNyxWorld, Warning, TEXT("Update for unknown player %s, spawning"), *NewRow.DisplayName);

		UGameInstance* GI = GetWorld()->GetGameInstance();
		UNyxNetworkSubsystem* NetworkSub = GI ? GI->GetSubsystem<UNyxNetworkSubsystem>() : nullptr;
		bool bIsLocal = NetworkSub && NetworkSub->IsLocalIdentity(NewRow.Identity);
		SpawnPlayerActor(NewRow, bIsLocal);
		return;
	}

	// Route the update to the movement component
	UNyxMovementComponent* MoveComp = (*ActorPtr)->FindComponentByClass<UNyxMovementComponent>();
	if (MoveComp)
	{
		MoveComp->OnServerUpdate(OldRow, NewRow);
	}
	else
	{
		// No movement component — direct position set (fallback)
		(*ActorPtr)->SetActorLocation(FVector(NewRow.PosX, NewRow.PosY, NewRow.PosZ));
		(*ActorPtr)->SetActorRotation(FRotator(0.f, NewRow.RotYaw, 0.f));
	}
}

void UNyxEntityManager::HandlePlayerDelete(const FEventContext& Context, const FPlayerType& DeletedRow)
{
	const FString Key = IdentityKey(DeletedRow.Identity);

	TObjectPtr<AActor>* ActorPtr = ManagedPlayers.Find(Key);
	if (ActorPtr && IsValid(*ActorPtr))
	{
		AActor* Actor = ActorPtr->Get();
		OnPlayerRemovedBP.Broadcast(Actor);

		// If this is the local player pawn, unpossess first
		if (Actor == LocalPlayerPawn)
		{
			LocalPlayerPawn = nullptr;
		}

		Actor->Destroy();
		UE_LOG(LogNyxWorld, Log, TEXT("Player deleted: %s"), *DeletedRow.DisplayName);
	}

	ManagedPlayers.Remove(Key);
}

// ─── Spawning ──────────────────────────────────────────────────────

AActor* UNyxEntityManager::SpawnPlayerActor(const FPlayerType& PlayerData, bool bIsLocal)
{
	UWorld* World = GetWorld();
	if (!World) return nullptr;

	const FString Key = IdentityKey(PlayerData.Identity);

	FVector SpawnPos(PlayerData.PosX, PlayerData.PosY, PlayerData.PosZ);
	FRotator SpawnRot(0.f, PlayerData.RotYaw, 0.f);

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	AActor* SpawnedActor = nullptr;

	if (bIsLocal)
	{
		// Spawn the local player pawn
		UClass* PawnClass = LocalPlayerPawnClass.Get();
		if (!PawnClass)
		{
			PawnClass = ANyxPlayerPawn::StaticClass();
		}

		ANyxPlayerPawn* Pawn = World->SpawnActor<ANyxPlayerPawn>(PawnClass, SpawnPos, SpawnRot, SpawnParams);
		if (Pawn)
		{
			LocalPlayerPawn = Pawn;

			// Init movement component for local prediction
			UGameInstance* GI = World->GetGameInstance();
			UNyxNetworkSubsystem* NetworkSub = GI ? GI->GetSubsystem<UNyxNetworkSubsystem>() : nullptr;
			UDbConnection* Conn = NetworkSub ? NetworkSub->GetSpacetimeDBConnection() : nullptr;

			if (UNyxMovementComponent* MoveComp = Pawn->GetNyxMovement())
			{
				MoveComp->InitAsLocalPlayer(Conn);
			}

			// Have the first player controller possess this pawn
			APlayerController* PC = World->GetFirstPlayerController();
			if (PC)
			{
				PC->Possess(Pawn);
				UE_LOG(LogNyxWorld, Log, TEXT("Local player possessed NyxPlayerPawn"));
			}

			SpawnedActor = Pawn;
		}
	}
	else
	{
		// Spawn a remote player actor — use NyxPlayerPawn with remote movement
		UClass* PawnClass = ANyxPlayerPawn::StaticClass();
		AActor* RemoteActor = World->SpawnActor<AActor>(PawnClass, SpawnPos, SpawnRot, SpawnParams);

		if (RemoteActor)
		{
			if (UNyxMovementComponent* MoveComp = RemoteActor->FindComponentByClass<UNyxMovementComponent>())
			{
				MoveComp->InitAsRemotePlayer();
			}
			SpawnedActor = RemoteActor;
		}
	}

	if (SpawnedActor)
	{
		ManagedPlayers.Add(Key, SpawnedActor);
		OnPlayerSpawnedBP.Broadcast(SpawnedActor, bIsLocal);
	}

	return SpawnedActor;
}
