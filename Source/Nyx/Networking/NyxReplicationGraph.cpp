// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxReplicationGraph.h"
#include "Nyx/Nyx.h"
#include "Nyx/Player/NyxCharacter.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/ChildConnection.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/HUD.h"
#include "UObject/UObjectIterator.h"

#if WITH_GAMEPLAY_DEBUGGER
#include "GameplayDebuggerCategoryReplicator.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(NyxReplicationGraph)

DEFINE_LOG_CATEGORY_STATIC(LogNyxRepGraph, Log, All);

// ─── Constructor ───────────────────────────────────────────────────

UNyxReplicationGraph::UNyxReplicationGraph()
{
}

// ─── Class Settings ────────────────────────────────────────────────

void UNyxReplicationGraph::InitGlobalActorClassSettings()
{
	Super::InitGlobalActorClassSettings();

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		AActor* ActorCDO = Cast<AActor>(Class->GetDefaultObject());
		if (!ActorCDO)
		{
			continue;
		}

		// Non-replicated actors still need ClassInfo registered to prevent
		// "No ClassInfo found" assert crashes when the proxy encounters them.
		if (!ActorCDO->GetIsReplicated())
		{
			FClassReplicationInfo NonReplicatedInfo;
			NonReplicatedInfo.SetCullDistanceSquared(0.f);
			GlobalActorReplicationInfoMap.SetClassInfo(Class, NonReplicatedInfo);
			continue;
		}

		// Skip transient blueprint classes
		if (Class->GetName().StartsWith(TEXT("SKEL_")) || Class->GetName().StartsWith(TEXT("REINST_")))
		{
			continue;
		}

		FClassReplicationInfo ClassInfo;
		ClassInfo.ReplicationPeriodFrame = GetReplicationPeriodFrameForFrequency(ActorCDO->GetNetUpdateFrequency());

		// ── Routing decisions ──

		// GameMode: server-only, never replicated to clients. Skip entirely.
		if (Class->IsChildOf(AGameModeBase::StaticClass()))
		{
			continue;
		}

		// GameState: always relevant to all connections
		if (Class->IsChildOf(AGameStateBase::StaticClass()))
		{
			ClassInfo.SetCullDistanceSquared(0.f);
			ClassRoutingMap.Set(Class, EClassRouting::AlwaysRelevant);
		}
		// PlayerState: always relevant to owning connection via per-connection node
		else if (Class->IsChildOf(APlayerState::StaticClass()))
		{
			ClassInfo.SetCullDistanceSquared(0.f);
			// PlayerState needs to be spatialised too so other clients see names/scores
			// But we mark as AlwaysRelevant so it's guaranteed to the owner
			ClassRoutingMap.Set(Class, EClassRouting::Spatialize);
		}
		// PlayerController: only relevant to owner
		else if (Class->IsChildOf(APlayerController::StaticClass()))
		{
			ClassInfo.SetCullDistanceSquared(0.f);
			ClassRoutingMap.Set(Class, EClassRouting::RelevantToOwner);
		}
		// HUD: only relevant to owner
		else if (Class->IsChildOf(AHUD::StaticClass()))
		{
			ClassInfo.SetCullDistanceSquared(0.f);
			ClassRoutingMap.Set(Class, EClassRouting::RelevantToOwner);
		}
		// LevelScriptActor: always relevant
		else if (Class->IsChildOf(ALevelScriptActor::StaticClass()))
		{
			ClassInfo.SetCullDistanceSquared(0.f);
			ClassRoutingMap.Set(Class, EClassRouting::AlwaysRelevant);
		}
#if WITH_GAMEPLAY_DEBUGGER
		// GameplayDebugger: handled specially, skip routing
		else if (Class->IsChildOf(AGameplayDebuggerCategoryReplicator::StaticClass()))
		{
			ClassRoutingMap.Set(Class, EClassRouting::NotRouted);
		}
#endif
		// NyxCharacter: spatial with larger cull distance for MMO visibility
		else if (Class->IsChildOf(ANyxCharacter::StaticClass()))
		{
			ClassInfo.SetCullDistanceSquared(CharacterCullDistanceSquared);
			ClassRoutingMap.Set(Class, EClassRouting::Spatialize);
		}
		// bAlwaysRelevant actors
		else if (ActorCDO->bAlwaysRelevant)
		{
			ClassInfo.SetCullDistanceSquared(0.f);
			ClassRoutingMap.Set(Class, EClassRouting::AlwaysRelevant);
		}
		// bOnlyRelevantToOwner actors
		else if (ActorCDO->bOnlyRelevantToOwner)
		{
			ClassInfo.SetCullDistanceSquared(0.f);
			ClassRoutingMap.Set(Class, EClassRouting::RelevantToOwner);
		}
		// Everything else: spatial relevancy
		else
		{
			ClassInfo.SetCullDistanceSquared(ActorCDO->GetNetCullDistanceSquared());
			ClassRoutingMap.Set(Class, EClassRouting::Spatialize);
		}

		GlobalActorReplicationInfoMap.SetClassInfo(Class, ClassInfo);
	}

	UE_LOG(LogNyxRepGraph, Log, TEXT("InitGlobalActorClassSettings complete."));
}

// ─── Global Graph Nodes ────────────────────────────────────────────

void UNyxReplicationGraph::InitGlobalGraphNodes()
{
	// ── Spatial Grid ──
	// All distance-based actors go here. The grid handles spatial partitioning
	// and cull distance checks automatically.
	GridNode = CreateNewNode<UReplicationGraphNode_GridSpatialization2D>();
	GridNode->CellSize = GridCellSize;
	GridNode->SpatialBias = FVector2D(-UE_OLD_WORLD_MAX, -UE_OLD_WORLD_MAX);

	// Configure dormancy behavior
	GridNode->bDestroyDormantDynamicActors = true;
	GridNode->DestroyDormantDynamicActorsCellTTL = 4; // Keep dormant actors for a few frames after leaving relevancy

	AddGlobalGraphNode(GridNode);

	// ── Always-Relevant (global) ──
	// GameState, level script actors, etc.
	AlwaysRelevantNode = CreateNewNode<UReplicationGraphNode_ActorList>();
	AddGlobalGraphNode(AlwaysRelevantNode);

	UE_LOG(LogNyxRepGraph, Log, TEXT("InitGlobalGraphNodes: GridNode (CellSize=%.0f), AlwaysRelevantNode created."),
		GridCellSize);
}

// ─── Per-Connection Nodes ──────────────────────────────────────────

void UNyxReplicationGraph::InitConnectionGraphNodes(UNetReplicationGraphConnection* RepGraphConnection)
{
	Super::InitConnectionGraphNodes(RepGraphConnection);

	// Each connection gets its own always-relevant node (for PlayerController, own pawn, etc.)
	UReplicationGraphNode_AlwaysRelevant_ForConnection* AlwaysRelevantForConnection =
		CreateNewNode<UReplicationGraphNode_AlwaysRelevant_ForConnection>();
	AddConnectionGraphNode(AlwaysRelevantForConnection, RepGraphConnection);

	// Track the mapping
	ConnectionKeys.Add(RepGraphConnection->NetConnection);
	ConnectionNodes.Add(AlwaysRelevantForConnection);

	UE_LOG(LogNyxRepGraph, Log, TEXT("InitConnectionGraphNodes: Created AlwaysRelevant_ForConnection for %s"),
		*GetNameSafe(RepGraphConnection->NetConnection));
}

// ─── Actor Routing ─────────────────────────────────────────────────

UNyxReplicationGraph::EClassRouting UNyxReplicationGraph::GetClassRouting(const AActor* Actor) const
{
	// Direct class checks as primary routing — TClassMap lookup was unreliable
	// for engine classes that may not be iterated during InitGlobalActorClassSettings
	if (!Actor) return EClassRouting::NotRouted;

	const UClass* Class = Actor->GetClass();

	if (Class->IsChildOf(APlayerController::StaticClass()))
		return EClassRouting::RelevantToOwner;

	if (Class->IsChildOf(AHUD::StaticClass()))
		return EClassRouting::RelevantToOwner;

	if (Class->IsChildOf(AGameStateBase::StaticClass()))
		return EClassRouting::AlwaysRelevant;

	if (Class->IsChildOf(ALevelScriptActor::StaticClass()))
		return EClassRouting::AlwaysRelevant;

#if WITH_GAMEPLAY_DEBUGGER
	if (Class->IsChildOf(AGameplayDebuggerCategoryReplicator::StaticClass()))
		return EClassRouting::NotRouted;
#endif

	// CDO-based fallback
	if (Actor->bOnlyRelevantToOwner)
		return EClassRouting::RelevantToOwner;

	if (Actor->bAlwaysRelevant)
		return EClassRouting::AlwaysRelevant;

	// Everything else: spatial
	return EClassRouting::Spatialize;
}

void UNyxReplicationGraph::RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo,
	FGlobalActorReplicationInfo& GlobalInfo)
{
	const EClassRouting Routing = GetClassRouting(ActorInfo.Actor);

	UE_LOG(LogNyxRepGraph, Log, TEXT("RouteAddNetworkActor: %s → %s"),
		*GetNameSafe(ActorInfo.Actor),
		Routing == EClassRouting::AlwaysRelevant ? TEXT("AlwaysRelevant") :
		Routing == EClassRouting::RelevantToOwner ? TEXT("RelevantToOwner") :
		Routing == EClassRouting::NotRouted ? TEXT("NotRouted") : TEXT("Spatialize"));

	switch (Routing)
	{
	case EClassRouting::AlwaysRelevant:
		AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
		break;

	case EClassRouting::RelevantToOwner:
		// Route to per-connection node if we have a connection, otherwise queue
		if (UNetConnection* Connection = ActorInfo.Actor->GetNetConnection())
		{
			if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = GetAlwaysRelevantNodeForConnection(Connection))
			{
				Node->NotifyAddNetworkActor(ActorInfo);
			}
			else
			{
				// Connection exists but doesn't match any per-connection node.
				// This happens on the multi-server proxy: the actor's connection is
				// a backend connection (ProxyBackend*) while per-connection nodes
				// track frontend client connections (ProxyNetConnection*).
				// Fall back to global AlwaysRelevant so the proxy can create actor
				// channels and forward the actor. The proxy's own CanDowngradeActorRole
				// mechanism handles per-client role assignment (Autonomous vs Simulated).
				UE_LOG(LogNyxRepGraph, Log,
					TEXT("RelevantToOwner actor %s has no matching node for connection %s — promoting to AlwaysRelevant"),
					*GetNameSafe(ActorInfo.Actor), *GetNameSafe(Connection));
				AlwaysRelevantNode->NotifyAddNetworkActor(ActorInfo);
			}
		}
		else
		{
			ActorsWithoutNetConnection.Add(ActorInfo.Actor);
		}
		break;

	case EClassRouting::NotRouted:
		// Do nothing — handled elsewhere (e.g., GameplayDebugger)
		break;

	case EClassRouting::Spatialize:
	default:
		// All spatially-relevant actors use dormancy-aware routing
		GridNode->AddActor_Dormancy(ActorInfo, GlobalInfo);
		break;
	}
}

void UNyxReplicationGraph::RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo)
{
	const EClassRouting Routing = GetClassRouting(ActorInfo.Actor);

	switch (Routing)
	{
	case EClassRouting::AlwaysRelevant:
		AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);
		SetActorDestructionInfoToIgnoreDistanceCulling(ActorInfo.GetActor());
		break;

	case EClassRouting::RelevantToOwner:
		if (UNetConnection* Connection = ActorInfo.Actor->GetNetConnection())
		{
			if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = GetAlwaysRelevantNodeForConnection(Connection))
			{
				Node->NotifyRemoveNetworkActor(ActorInfo);
			}
			else
			{
				// Proxy fallback — was promoted to AlwaysRelevant during Add
				AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);
			}
		}
		else
		{
			// Connection already torn down (e.g., proxy child connection closing).
			// The actor was added to a per-connection node but we can't look it up
			// by connection anymore. Search all nodes to prevent stale references
			// that trigger "IsActorValidForReplication" ensures.
			bool bFoundInNode = false;
			for (int32 Idx = 0; Idx < ConnectionNodes.Num(); ++Idx)
			{
				if (ConnectionNodes[Idx])
				{
					ConnectionNodes[Idx]->NotifyRemoveNetworkActor(ActorInfo);
					bFoundInNode = true;
				}
			}
			// Also try global AlwaysRelevant in case it was promoted there
			AlwaysRelevantNode->NotifyRemoveNetworkActor(ActorInfo);

			UE_LOG(LogNyxRepGraph, Log,
				TEXT("RouteRemoveNetworkActor: %s had no connection — searched all nodes (found=%d)"),
				*GetNameSafe(ActorInfo.Actor), bFoundInNode ? 1 : 0);
		}
		ActorsWithoutNetConnection.Remove(ActorInfo.Actor);
		break;

	case EClassRouting::NotRouted:
		break;

	case EClassRouting::Spatialize:
	default:
		GridNode->RemoveActor_Dormancy(ActorInfo);
		break;
	}
}

void UNyxReplicationGraph::RouteRenameNetworkActorToNodes(const FRenamedReplicatedActorInfo& ActorInfo)
{
	const EClassRouting Routing = GetClassRouting(ActorInfo.NewActorInfo.Actor);

	switch (Routing)
	{
	case EClassRouting::AlwaysRelevant:
		AlwaysRelevantNode->NotifyActorRenamed(ActorInfo);
		break;

	case EClassRouting::RelevantToOwner:
		if (UNetConnection* Connection = ActorInfo.NewActorInfo.Actor->GetNetConnection())
		{
			if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = GetAlwaysRelevantNodeForConnection(Connection))
			{
				Node->NotifyActorRenamed(ActorInfo);
			}
			else
			{
				// Proxy fallback
				AlwaysRelevantNode->NotifyActorRenamed(ActorInfo);
			}
		}
		break;

	case EClassRouting::NotRouted:
		break;

	case EClassRouting::Spatialize:
	default:
		GridNode->RenameActor_Dormancy(ActorInfo);
		break;
	}
}

// ─── Connection Cleanup ────────────────────────────────────────────

void UNyxReplicationGraph::RemoveClientConnection(UNetConnection* NetConnection)
{
	// Clean up our per-connection tracking arrays BEFORE the base class tears
	// down the graph connection. This prevents stale references that cause
	// "IsActorValidForReplication" ensures when the proxy closes child connections.
	if (NetConnection)
	{
		for (int32 Idx = ConnectionKeys.Num() - 1; Idx >= 0; --Idx)
		{
			if (ConnectionKeys[Idx] == NetConnection)
			{
				UE_LOG(LogNyxRepGraph, Log, TEXT("RemoveClientConnection: Cleaning up connection node for %s"),
					*GetNameSafe(NetConnection));
				ConnectionKeys.RemoveAt(Idx);
				ConnectionNodes.RemoveAt(Idx);
				break;
			}
		}
	}

	Super::RemoveClientConnection(NetConnection);
}

// ─── ServerReplicateActors ─────────────────────────────────────────

int32 UNyxReplicationGraph::ServerReplicateActors(float DeltaSeconds)
{
	// Route owner-only actors that didn't have a connection at spawn time
	for (int32 Idx = ActorsWithoutNetConnection.Num() - 1; Idx >= 0; --Idx)
	{
		bool bRemove = false;
		if (AActor* Actor = ActorsWithoutNetConnection[Idx])
		{
			if (UNetConnection* Connection = Actor->GetNetConnection())
			{
				bRemove = true;
				if (UReplicationGraphNode_AlwaysRelevant_ForConnection* Node = GetAlwaysRelevantNodeForConnection(Connection))
				{
					Node->NotifyAddNetworkActor(FNewReplicatedActorInfo(Actor));
				}
				else
				{
					// Proxy fallback — promote to AlwaysRelevant
					AlwaysRelevantNode->NotifyAddNetworkActor(FNewReplicatedActorInfo(Actor));
				}
			}
		}
		else
		{
			bRemove = true;
		}

		if (bRemove)
		{
			ActorsWithoutNetConnection.RemoveAtSwap(Idx, EAllowShrinking::No);
		}
	}

	return Super::ServerReplicateActors(DeltaSeconds);
}

// ─── Helpers ───────────────────────────────────────────────────────

UReplicationGraphNode_AlwaysRelevant_ForConnection* UNyxReplicationGraph::GetAlwaysRelevantNodeForConnection(
	UNetConnection* Connection)
{
	if (!Connection)
	{
		return nullptr;
	}

	// Child connections (split-screen / multi-server proxy children) don't have their own
	// connection managers — they share their parent's per-connection nodes.
	// This matches UReplicationGraph::FindConnectionManager which redirects children to parents.
	if (UChildConnection* ChildConn = Cast<UChildConnection>(Connection))
	{
		Connection = ChildConn->Parent;
	}

	for (int32 Idx = 0; Idx < ConnectionKeys.Num(); ++Idx)
	{
		if (ConnectionKeys[Idx] == Connection)
		{
			return ConnectionNodes[Idx];
		}
	}

	UE_LOG(LogNyxRepGraph, Warning, TEXT("No AlwaysRelevant_ForConnection node for %s"), *GetNameSafe(Connection));
	return nullptr;
}
