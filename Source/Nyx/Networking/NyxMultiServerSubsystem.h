// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "NyxMultiServerSubsystem.generated.h"

class UMultiServerNode;
class AMultiServerBeaconClient;
class ANyxMultiServerBeaconClient;

/**
 * Manages the MultiServer Replication mesh for entity-sharded zones.
 *
 * Architecture (RESEARCH.md, Option 4, Pattern A):
 *   When a zone has > 300 players, multiple UE5 dedicated servers share the
 *   same zone. Each server is authoritative over a subset of entities.
 *   This subsystem creates and manages the UMultiServerNode + beacon mesh
 *   that enables server-to-server communication.
 *
 * Lifecycle:
 *   1. NyxGameMode detects multi-server mode (cmd-line args or SpacetimeDB config)
 *   2. Calls InitializeMultiServerMesh() with peer addresses from SpacetimeDB zone_server table
 *   3. Subsystem creates UMultiServerNode, connects to peers
 *   4. On each peer connection, wires up cross-server combat/handoff delegates
 *   5. On shutdown, tears down mesh gracefully
 *
 * Command-line args (parsed by UMultiServerNode):
 *   -MultiServerPeers=IP1:Port1,IP2:Port2,...
 *   -MultiServerNumServers=N
 *   -NyxMultiServerLocalId=UniqueServerId
 *   -NyxMultiServerListenPort=PortNum
 */
UCLASS()
class NYX_API UNyxMultiServerSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ──── Mesh Management ────

	/**
	 * Initialize the MultiServer mesh with explicit peer info.
	 * Called by NyxGameMode when SpacetimeDB zone_server table
	 * indicates this zone has multiple servers.
	 *
	 * @param LocalPeerId - This server's unique ID (usually the DedicatedServerId)
	 * @param ListenIp - IP to listen on for beacon connections
	 * @param ListenPort - Port for beacon listener
	 * @param NumServers - Total expected server count in this zone
	 * @param PeerAddresses - Array of "IP:Port" for other servers in the mesh
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|MultiServer")
	void InitializeMultiServerMesh(const FString& LocalPeerId,
		const FString& ListenIp, int32 ListenPort,
		int32 NumServers, const TArray<FString>& PeerAddresses);

	/**
	 * Initialize from command-line arguments.
	 * Reads -MultiServerPeers, -MultiServerNumServers, etc.
	 * Returns true if multi-server mode was configured.
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|MultiServer")
	bool InitializeFromCommandLine();

	/** Is the multi-server mesh active? */
	UFUNCTION(BlueprintCallable, Category = "Nyx|MultiServer")
	bool IsMeshActive() const { return MultiServerNode != nullptr; }

	/** Are all expected peer servers connected? */
	UFUNCTION(BlueprintCallable, Category = "Nyx|MultiServer")
	bool AreAllPeersConnected() const;

	/** Shut down the mesh. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|MultiServer")
	void ShutdownMesh();

	// ──── Cross-Server Operations ────

	/**
	 * Send a cross-server hit notification to the server owning the defender.
	 * @param PeerServerId - The peer server's ID that owns the defender entity
	 * @param AttackerIdentityHex - Attacker's SpacetimeDB identity
	 * @param DefenderIdentityHex - Defender's SpacetimeDB identity
	 * @param SkillId - Ability/skill used
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|MultiServer")
	void SendCrossServerHit(const FString& PeerServerId,
		const FString& AttackerIdentityHex,
		const FString& DefenderIdentityHex, int32 SkillId);

	/**
	 * Request entity handoff to a peer server.
	 * @param PeerServerId - The peer server's ID
	 * @param EntityIdentityHex - Entity's SpacetimeDB identity
	 * @param Reason - Why the handoff is happening
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|MultiServer")
	void RequestEntityHandoff(const FString& PeerServerId,
		const FString& EntityIdentityHex, const FString& Reason);

	/**
	 * Broadcast this server's entity count to all peers.
	 * Called periodically alongside the SpacetimeDB heartbeat.
	 * @param ServerId - This server's ID
	 * @param EntityCount - Current entity count
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|MultiServer")
	void BroadcastEntityCount(const FString& ServerId, int32 EntityCount);

	// ──── Delegates ────

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCrossServerHitDelegate,
		const FString&, AttackerHex, const FString&, DefenderHex, int32, SkillId);

	/** Fired when this server receives a cross-server hit from a peer. */
	UPROPERTY(BlueprintAssignable, Category = "Nyx|MultiServer")
	FOnCrossServerHitDelegate OnCrossServerHitReceived;

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnEntityHandoffDelegate,
		const FString&, EntityHex, const FString&, Reason);

	/** Fired when a peer requests entity handoff to this server. */
	UPROPERTY(BlueprintAssignable, Category = "Nyx|MultiServer")
	FOnEntityHandoffDelegate OnEntityHandoffRequested;

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnPeerConnectedDelegate, const FString&, PeerId);

	/** Fired when a new peer connects to the mesh. */
	UPROPERTY(BlueprintAssignable, Category = "Nyx|MultiServer")
	FOnPeerConnectedDelegate OnPeerConnected;

private:
	void HandlePeerConnected(const FString& LocalPeerId, const FString& RemotePeerId,
		AMultiServerBeaconClient* Beacon);

	void HandleCrossServerHit(const FString& AttackerHex, const FString& DefenderHex, int32 SkillId);
	void HandleEntityHandoff(const FString& EntityHex, const FString& Reason);

	UPROPERTY()
	TObjectPtr<UMultiServerNode> MultiServerNode;

	/** Track connected peer beacons for sending RPCs. */
	UPROPERTY()
	TMap<FString, TObjectPtr<ANyxMultiServerBeaconClient>> PeerBeacons;
};
