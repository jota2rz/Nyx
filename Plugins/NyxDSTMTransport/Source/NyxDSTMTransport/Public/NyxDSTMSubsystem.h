// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"

#if UE_WITH_REMOTE_OBJECT_HANDLE
#include "UObject/RemoteObjectTransfer.h"
#include "UObject/RemoteObjectTypes.h"
#endif

#include "NyxDSTMSubsystem.generated.h"

class UMultiServerNode;
class AMultiServerBeaconClient;
class ANyxDSTMBeaconClient;

/**
 * Manages the DSTM beacon transport mesh for seamless cross-server migration.
 *
 * This subsystem is the runtime counterpart of the NyxDSTMTransportModule.
 * The module binds static delegate callbacks at startup; those callbacks
 * forward to this subsystem for actual routing through the beacon mesh.
 *
 * Architecture (SEAMLESS.md, Approach 2):
 *   ┌──────────────┐       ┌──────────────────────┐       ┌──────────────┐
 *   │   Server-A   │       │   DSTM Beacon Mesh   │       │   Server-B   │
 *   │              │       │                      │       │              │
 *   │ TransferOwn  │──────►│  Serialize + Send    │──────►│  Receive +   │
 *   │  ership()    │       │  via BeaconClient    │       │  Deserialize │
 *   │              │       │  RPC                 │       │              │
 *   │ PostMigrate  │       │                      │       │ OnObjectData │
 *   │  (Send)      │       │                      │       │  Received()  │
 *   │              │       │                      │       │              │
 *   │ [Migrated    │       │                      │       │ PostMigrate  │
 *   │  close]      │       │                      │       │  (Receive)   │
 *   └──────────────┘       └──────────────────────┘       └──────────────┘
 *
 * The mesh is separate from the combat/handoff mesh (NyxMultiServerSubsystem)
 * to maintain plugin isolation. It reads the same command-line peer config
 * with an offset port (+1000).
 *
 * Lifecycle:
 *   1. NyxDSTMTransportModule::StartupModule() binds transport delegates
 *   2. This subsystem initializes and creates the beacon mesh
 *   3. On peer connection, migration data can flow
 *   4. On shutdown, mesh is torn down gracefully
 */
UCLASS()
class NYXDSTMTRANSPORT_API UNyxDSTMSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// ──── Mesh Management ────

	/**
	 * Initialize the DSTM beacon mesh from command-line arguments.
	 * Reads the same -NyxMultiServerLocalId=, -NyxMultiServerListenPort=, -MultiServerPeers=
	 * as NyxMultiServerSubsystem, but uses an offset port for the DSTM mesh.
	 *
	 * @return true if DSTM mesh was configured and created
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|DSTM")
	bool InitializeFromCommandLine();

	/**
	 * Initialize the DSTM beacon mesh with explicit parameters.
	 *
	 * @param LocalPeerId    - This server's unique ID string
	 * @param ListenIp       - IP to listen on for beacon connections
	 * @param ListenPort     - Port for DSTM beacon listener
	 * @param NumServers     - Total expected server count
	 * @param PeerAddresses  - Array of "IP:Port" for DSTM beacons on other servers
	 */
	UFUNCTION(BlueprintCallable, Category = "Nyx|DSTM")
	void InitializeDSTMMesh(
		const FString& LocalPeerId,
		const FString& ListenIp,
		int32 ListenPort,
		int32 NumServers,
		const TArray<FString>& PeerAddresses);

	/** Is the DSTM mesh active and ready for migration? */
	UFUNCTION(BlueprintCallable, Category = "Nyx|DSTM")
	bool IsMeshActive() const { return DSTMNode != nullptr; }

	/** Are all expected DSTM peer connections established? */
	UFUNCTION(BlueprintCallable, Category = "Nyx|DSTM")
	bool AreAllPeersConnected() const;

	/** Shut down the DSTM mesh. */
	UFUNCTION(BlueprintCallable, Category = "Nyx|DSTM")
	void ShutdownMesh();

	// ──── Migration API ────

#if UE_WITH_REMOTE_OBJECT_HANDLE
	/**
	 * Transfer an actor to a remote server using the DSTM pipeline.
	 * Calls UE::RemoteObject::Transfer::TransferObjectOwnershipToRemoteServer().
	 *
	 * @param Actor          - The actor to transfer (typically PlayerController or Pawn)
	 * @param DestServerId   - Destination server's FRemoteServerId
	 */
	void TransferActorToServer(AActor* Actor, FRemoteServerId DestServerId);

	/**
	 * Get the FRemoteServerId for a peer by its DedicatedServerId string.
	 * Uses the same hash as InitializeServerIdentity() for consistency.
	 */
	static FRemoteServerId GetRemoteServerIdFromString(const FString& DedicatedServerId);

	/**
	 * Get the FRemoteServerId for the first connected peer.
	 * Convenience for 2-server setups where there's only one peer.
	 *
	 * @param OutServerId - Receives the peer's FRemoteServerId
	 * @return true if a peer is connected
	 */
	bool GetFirstPeerServerId(FRemoteServerId& OutServerId) const;

	// ──── Transport Delegate Handlers ────
	// Called by the module's static callbacks.

	/** Handle outgoing migration: serialize FRemoteObjectData and send via beacon. */
	void HandleOutgoingMigration(const UE::RemoteObject::Transfer::FMigrateSendParams& Params);

	/** Handle incoming pull-request: forward to the appropriate peer. */
	void HandleObjectRequest(
		FRemoteWorkPriority Priority,
		FRemoteObjectId ObjectId,
		FRemoteServerId LastKnownServerId,
		FRemoteServerId DestServerId);
#endif

	// ──── Port Offset ────

	/** Port offset from the main MultiServer mesh port. Default: 1000. */
	static constexpr int32 DSTMPortOffset = 1000;

private:
	void HandlePeerConnected(
		const FString& LocalPeerId,
		const FString& RemotePeerId,
		AMultiServerBeaconClient* Beacon);

#if UE_WITH_REMOTE_OBJECT_HANDLE
	/** Handle incoming migration data from a peer beacon. */
	void HandleIncomingMigrationData(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		const TArray<uint8>& SerializedData);

	/** Handle incoming pull-migration request from a peer beacon. */
	void HandleIncomingMigrationRequest(
		uint64 ObjectIdRaw,
		uint32 RequestingServerIdRaw);
#endif

	/**
	 * Rewrite peer addresses to use the DSTM port offset.
	 * "192.168.1.10:15000" → "192.168.1.10:16000" (with offset 1000)
	 */
	static TArray<FString> OffsetPeerPorts(const TArray<FString>& PeerAddresses, int32 Offset);

	/** Find the beacon client connected to a specific server. */
	ANyxDSTMBeaconClient* FindBeaconForServer(uint32 ServerIdHash) const;

	UPROPERTY()
	TObjectPtr<UMultiServerNode> DSTMNode;

	/** Map from peer DedicatedServerId string → beacon client. */
	UPROPERTY()
	TMap<FString, TObjectPtr<ANyxDSTMBeaconClient>> PeerBeacons;

	/** Map from hashed server ID → peer DedicatedServerId string (reverse lookup). */
	TMap<uint32, FString> ServerIdHashToPeerId;
};
