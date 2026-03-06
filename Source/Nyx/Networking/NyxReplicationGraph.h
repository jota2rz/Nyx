// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ReplicationGraph.h"
#include "NyxReplicationGraph.generated.h"

class ANyxCharacter;
class APlayerController;
class APlayerState;
class AGameStateBase;

/**
 * Custom ReplicationGraph for Nyx MMO.
 *
 * Implements the L2-style optimizations documented in RESEARCH.md (Option 4, Pattern D):
 *
 *   - Spatial grid (GridSpatialization2D) for distance-based relevancy
 *   - Distance-tiered update rates:
 *       Close  (< GridNearDistance):   full CMC, NetUpdateFrequency = 10 Hz
 *       Medium (< GridMediumDistance): simplified, NetUpdateFrequency = 5 Hz
 *       Far    (< CullDistance):       position-only, NetUpdateFrequency = 2 Hz
 *   - Per-connection always-relevant nodes (own pawn, party members)
 *   - Dormancy support (stationary actors stop replicating)
 *   - Actor class routing (GameState always relevant, PlayerState to owner, etc.)
 *
 * Configured via DefaultEngine.ini:
 *   [/Script/OnlineSubsystemUtils.IpNetDriver]
 *   ReplicationDriverClassName="/Script/Nyx.NyxReplicationGraph"
 */
UCLASS(transient, config = Engine)
class NYX_API UNyxReplicationGraph : public UReplicationGraph
{
	GENERATED_BODY()

public:
	UNyxReplicationGraph();

	// ──── UReplicationGraph Interface ────

	virtual void InitGlobalActorClassSettings() override;
	virtual void InitGlobalGraphNodes() override;
	virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* RepGraphConnection) override;
	virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo) override;
	virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual void RemoveClientConnection(UNetConnection* NetConnection) override;
	virtual int32 ServerReplicateActors(float DeltaSeconds) override;

	// ──── Configuration ────

	/** Grid cell size in UU. Larger cells → fewer grid lookups but coarser relevancy. */
	UPROPERTY(Config)
	float GridCellSize = 10000.f;

	/** Near distance (UU). Actors within this range get full update rate. */
	UPROPERTY(Config)
	float GridNearDistance = 3000.f;

	/** Medium distance (UU). Actors within this range get medium update rate. */
	UPROPERTY(Config)
	float GridMediumDistance = 8000.f;

	/** Default net cull distance squared for spatially-relevant actors. */
	UPROPERTY(Config)
	float DefaultCullDistanceSquared = 200000000.f; // ~14142 UU = ~141m

	/** Net cull distance squared for NyxCharacter pawns (larger to see more players). */
	UPROPERTY(Config)
	float CharacterCullDistanceSquared = 400000000.f; // ~20000 UU = ~200m

protected:
	virtual void RouteRenameNetworkActorToNodes(const FRenamedReplicatedActorInfo& ActorInfo) override;

private:
	// ──── Graph Nodes ────

	/** Spatial grid node — distance-based relevancy for most actors. */
	UPROPERTY()
	TObjectPtr<UReplicationGraphNode_GridSpatialization2D> GridNode;

	/** Always-relevant to ALL connections (GameState, etc.) */
	UPROPERTY()
	TObjectPtr<UReplicationGraphNode_ActorList> AlwaysRelevantNode;

	// ──── Per-Connection Tracking ────

	/** Per-connection always-relevant node pairs. */
	UPROPERTY()
	TArray<TObjectPtr<UNetConnection>> ConnectionKeys;

	UPROPERTY()
	TArray<TObjectPtr<UReplicationGraphNode_AlwaysRelevant_ForConnection>> ConnectionNodes;

	/** Actors that have bOnlyRelevantToOwner but didn't have a connection at spawn time. */
	UPROPERTY()
	TArray<TObjectPtr<AActor>> ActorsWithoutNetConnection;

	// ──── Helpers ────

	UReplicationGraphNode_AlwaysRelevant_ForConnection* GetAlwaysRelevantNodeForConnection(UNetConnection* Connection);

	/** Classify an actor for routing purposes. */
	enum class EClassRouting : uint8
	{
		Spatialize,       // Goes into GridNode (distance-based)
		AlwaysRelevant,   // Goes into global AlwaysRelevantNode
		RelevantToOwner,  // Goes into per-connection node
		NotRouted,        // Handled specially (e.g., GameplayDebugger)
	};

	/** Cached class → routing map, populated in InitGlobalActorClassSettings. */
	TClassMap<EClassRouting> ClassRoutingMap;

	/** Direct IsChildOf-based routing that bypasses TClassMap (which can miss classes). */
	EClassRouting GetClassRouting(const AActor* Actor) const;
};
