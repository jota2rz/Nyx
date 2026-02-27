// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxEntityManager.h"
#include "Nyx/Nyx.h"
#include "Nyx/Core/NyxNetworkSubsystem.h"
#include "Nyx/Networking/NyxDatabaseInterface.h"
#include "Engine/World.h"

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
	// Only create in game worlds, not in editor preview worlds
	UWorld* World = Cast<UWorld>(Outer);
	if (!World)
	{
		return false;
	}
	return World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE;
}

void UNyxEntityManager::StartListening()
{
	if (bIsListening)
	{
		return;
	}

	UGameInstance* GI = GetWorld()->GetGameInstance();
	if (!GI)
	{
		UE_LOG(LogNyxWorld, Error, TEXT("No GameInstance available"));
		return;
	}

	UNyxNetworkSubsystem* NetworkSub = GI->GetSubsystem<UNyxNetworkSubsystem>();
	if (!NetworkSub || !NetworkSub->GetDatabaseInterface())
	{
		UE_LOG(LogNyxWorld, Error, TEXT("NyxNetworkSubsystem or database interface not available"));
		return;
	}

	INyxDatabaseInterface* Db = NetworkSub->GetDatabaseInterface();

	InsertHandle = Db->OnPlayerInserted().AddUObject(this, &UNyxEntityManager::HandlePlayerInserted);
	UpdateHandle = Db->OnPlayerUpdated().AddUObject(this, &UNyxEntityManager::HandlePlayerUpdated);
	DeleteHandle = Db->OnPlayerDeleted().AddUObject(this, &UNyxEntityManager::HandlePlayerDeleted);

	bIsListening = true;
	UE_LOG(LogNyxWorld, Log, TEXT("NyxEntityManager now listening for database events"));
}

void UNyxEntityManager::StopListening()
{
	if (!bIsListening)
	{
		return;
	}

	UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	if (GI)
	{
		UNyxNetworkSubsystem* NetworkSub = GI->GetSubsystem<UNyxNetworkSubsystem>();
		if (NetworkSub && NetworkSub->GetDatabaseInterface())
		{
			INyxDatabaseInterface* Db = NetworkSub->GetDatabaseInterface();
			Db->OnPlayerInserted().Remove(InsertHandle);
			Db->OnPlayerUpdated().Remove(UpdateHandle);
			Db->OnPlayerDeleted().Remove(DeleteHandle);
		}
	}

	// Destroy all managed entities
	for (auto& Pair : ManagedEntities)
	{
		if (Pair.Value && IsValid(Pair.Value))
		{
			Pair.Value->Destroy();
		}
	}
	ManagedEntities.Empty();
	LocalPlayerEntityId = FNyxEntityId();

	bIsListening = false;
	UE_LOG(LogNyxWorld, Log, TEXT("NyxEntityManager stopped listening"));
}

AActor* UNyxEntityManager::GetEntityActor(FNyxEntityId EntityId) const
{
	const TObjectPtr<AActor>* Found = ManagedEntities.Find(EntityId.Value);
	return Found ? Found->Get() : nullptr;
}

void UNyxEntityManager::HandlePlayerInserted(const FNyxPlayerData& PlayerData)
{
	UE_LOG(LogNyxWorld, Log, TEXT("Player inserted: EntityId=%s, Name=%s"),
		*PlayerData.EntityId.ToString(), *PlayerData.Identity.DisplayName);

	// Check if we already have this entity (shouldn't happen, but guard)
	if (ManagedEntities.Contains(PlayerData.EntityId.Value))
	{
		UE_LOG(LogNyxWorld, Warning, TEXT("Entity %s already exists, updating instead"),
			*PlayerData.EntityId.ToString());
		HandlePlayerUpdated(PlayerData, PlayerData);
		return;
	}

	AActor* NewActor = SpawnEntityActor(PlayerData);
	if (NewActor)
	{
		ManagedEntities.Add(PlayerData.EntityId.Value, NewActor);
		OnEntitySpawnedBP.Broadcast(PlayerData.EntityId, NewActor);
	}
}

void UNyxEntityManager::HandlePlayerUpdated(const FNyxPlayerData& OldData, const FNyxPlayerData& NewData)
{
	TObjectPtr<AActor>* ActorPtr = ManagedEntities.Find(NewData.EntityId.Value);
	if (!ActorPtr || !IsValid(*ActorPtr))
	{
		UE_LOG(LogNyxWorld, Warning, TEXT("Update for unknown entity %s, spawning"),
			*NewData.EntityId.ToString());
		HandlePlayerInserted(NewData);
		return;
	}

	AActor* Actor = ActorPtr->Get();
	UpdateEntityActor(Actor, NewData);
	OnEntityUpdatedBP.Broadcast(NewData.EntityId, Actor);
}

void UNyxEntityManager::HandlePlayerDeleted(const FNyxPlayerData& PlayerData)
{
	UE_LOG(LogNyxWorld, Log, TEXT("Player deleted: EntityId=%s"), *PlayerData.EntityId.ToString());

	TObjectPtr<AActor>* ActorPtr = ManagedEntities.Find(PlayerData.EntityId.Value);
	if (ActorPtr && IsValid(*ActorPtr))
	{
		(*ActorPtr)->Destroy();
	}

	ManagedEntities.Remove(PlayerData.EntityId.Value);
	OnEntityDestroyedBP.Broadcast(PlayerData.EntityId);
}

AActor* UNyxEntityManager::SpawnEntityActor(const FNyxPlayerData& Data)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogNyxWorld, Error, TEXT("No world available for spawning"));
		return nullptr;
	}

	// Determine the class to spawn
	UClass* SpawnClass = RemotePlayerActorClass.Get();
	if (!SpawnClass)
	{
		// Fallback: spawn a basic actor. In production, this should always be configured.
		UE_LOG(LogNyxWorld, Warning, TEXT("No RemotePlayerActorClass set, spawning default AActor"));
		SpawnClass = AActor::StaticClass();
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	FTransform SpawnTransform;
	SpawnTransform.SetLocation(Data.Position.Location);
	SpawnTransform.SetRotation(Data.Position.Rotation.Quaternion());

	AActor* SpawnedActor = World->SpawnActor<AActor>(SpawnClass, SpawnTransform, SpawnParams);
	if (SpawnedActor)
	{
		UE_LOG(LogNyxWorld, Log, TEXT("Spawned entity %s at %s"),
			*Data.EntityId.ToString(), *Data.Position.Location.ToString());
	}

	return SpawnedActor;
}

void UNyxEntityManager::UpdateEntityActor(AActor* Actor, const FNyxPlayerData& NewData)
{
	if (!Actor)
	{
		return;
	}

	// For remote players: interpolate to new position
	// For local player: reconcile with server state (prediction)
	// TODO (Spike 6): Implement proper interpolation and prediction

	if (IsLocalPlayer(NewData.EntityId))
	{
		// Server reconciliation for local player
		// For now, just log the server position — prediction system comes in Spike 6
		UE_LOG(LogNyxWorld, Verbose, TEXT("Server position for local player: %s"),
			*NewData.Position.Location.ToString());
	}
	else
	{
		// Remote player: set position directly for now (interpolation comes in Spike 6)
		Actor->SetActorLocationAndRotation(
			NewData.Position.Location,
			NewData.Position.Rotation);
	}
}
