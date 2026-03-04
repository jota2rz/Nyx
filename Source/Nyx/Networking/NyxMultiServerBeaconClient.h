// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MultiServerBeaconClient.h"
#include "NyxMultiServerBeaconClient.generated.h"

/**
 * Custom beacon client for Nyx cross-server communication.
 *
 * Each UE5 dedicated server in a zone mesh creates one of these per peer.
 * Implements game-level RPCs for:
 *   - Cross-server combat hit resolution forwarding
 *   - Entity authority handoff requests
 *   - Zone population broadcast
 *
 * Architecture (from RESEARCH.md, Option 4 Pattern A):
 *   When a player on Server A attacks a player owned by Server B,
 *   Server A calls a Server RPC on this beacon to notify Server B.
 *   Server B then calls SpacetimeDB's resolve_hit() reducer.
 *   The ACID result flows back via SpacetimeDB subscription.
 */
UCLASS(transient, config = Engine, notplaceable)
class NYX_API ANyxMultiServerBeaconClient : public AMultiServerBeaconClient
{
	GENERATED_BODY()

public:
	ANyxMultiServerBeaconClient();

	// ──── AMultiServerBeaconClient Interface ────

	virtual void OnConnected() override;

	// ──── Cross-Server Combat RPCs ────

	/**
	 * Notify a peer server that one of its entities was hit by an entity on this server.
	 * The receiving server is authoritative over the defender and will call resolve_hit().
	 *
	 * @param AttackerIdentityHex - SpacetimeDB identity hex of the attacker
	 * @param DefenderIdentityHex - SpacetimeDB identity hex of the defender
	 * @param SkillId - Ability/skill used
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerNotifyCrossServerHit(const FString& AttackerIdentityHex,
		const FString& DefenderIdentityHex, int32 SkillId);

	// ──── Entity Authority Handoff RPCs ────

	/**
	 * Request a peer server to take authority over an entity.
	 * Used during entity-shard rebalancing or when a player
	 * is better served by a different server in the zone.
	 *
	 * @param EntityIdentityHex - SpacetimeDB identity of the entity to hand off
	 * @param Reason - Why the handoff is happening ("rebalance", "migration", etc.)
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestEntityHandoff(const FString& EntityIdentityHex, const FString& Reason);

	/**
	 * Acknowledge that this server has accepted authority over an entity.
	 * Sent back to the originating server after a successful handoff.
	 *
	 * @param EntityIdentityHex - SpacetimeDB identity of the entity
	 */
	UFUNCTION(Client, Reliable)
	void ClientEntityHandoffAccepted(const FString& EntityIdentityHex);

	// ──── Zone Population Sync ────

	/**
	 * Broadcast this server's current entity count to a peer.
	 * Used for local load-balancing decisions without polling SpacetimeDB.
	 *
	 * @param ServerId - This server's unique ID
	 * @param EntityCount - Current entity count
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerBroadcastEntityCount(const FString& ServerId, int32 EntityCount);

	// ──── Delegates ────

	DECLARE_MULTICAST_DELEGATE_ThreeParams(FOnCrossServerHit, const FString& /* AttackerHex */,
		const FString& /* DefenderHex */, int32 /* SkillId */);
	FOnCrossServerHit OnCrossServerHitReceived;

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnEntityHandoffRequest, const FString& /* EntityHex */,
		const FString& /* Reason */);
	FOnEntityHandoffRequest OnEntityHandoffRequested;

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnPeerEntityCount, const FString& /* ServerId */,
		int32 /* EntityCount */);
	FOnPeerEntityCount OnPeerEntityCountReceived;
};
