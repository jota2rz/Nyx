// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MultiServerBeaconClient.h"
#include "NyxDSTMBeaconClient.generated.h"

/**
 * DSTM-aware beacon client for inter-server migration data transfer.
 *
 * Extends AMultiServerBeaconClient with RPCs that carry serialized
 * FRemoteObjectData between game servers. This is the network transport
 * layer that replaces DSTM's default disk I/O with beacon-based delivery.
 *
 * Architecture (SEAMLESS.md, Approach 2, Step 3):
 *   When Server-A calls TransferObjectOwnershipToRemoteServer(), the engine
 *   serializes the actor and invokes RemoteObjectTransferDelegate. Our
 *   NyxDSTMSubsystem catches this, serializes FRemoteObjectData to bytes,
 *   and sends it to Server-B via this beacon's ServerReceiveMigratedObject().
 *
 *   Server-B receives the data, deserializes it, and feeds it into
 *   OnObjectDataReceived() — the engine's DSTM receive pipeline handles
 *   the rest (PostMigrate, connection rebind, etc.).
 *
 * The beacon mesh uses SetUnlimitedBunchSizeAllowed(true) (inherited from
 * AMultiServerBeaconClient), so serialized payloads can exceed the default
 * 64KB reliable RPC limit.
 */
UCLASS(Transient, Config = Engine, NotPlaceable)
class NYXDSTMTRANSPORT_API ANyxDSTMBeaconClient : public AMultiServerBeaconClient
{
	GENERATED_BODY()

public:
	ANyxDSTMBeaconClient();

	virtual void OnConnected() override;

	// ──── Migration Data Transfer RPCs ────

	/**
	 * Send serialized migration data to the receiving server.
	 * Called when this beacon is the authority (we initiated the connection).
	 *
	 * @param ObjectIdRaw       - FRemoteObjectId serialized as uint64
	 * @param OwnerServerIdRaw  - FRemoteServerId of the object's owner
	 * @param PhysicsServerIdRaw - FRemoteServerId of the physics simulation owner
	 * @param PhysicsLocalIslandId - Physics island ID on the physics server
	 * @param SenderServerIdRaw - FRemoteServerId of the sending server (us)
	 * @param SerializedData    - FRemoteObjectData serialized to bytes
	 */
	UFUNCTION(Server, Reliable)
	void ServerReceiveMigratedObject(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		const TArray<uint8>& SerializedData);

	/**
	 * Send serialized migration data in the reverse direction.
	 * Called when we are the client side of the beacon connection.
	 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveMigratedObject(
		uint64 ObjectIdRaw,
		uint32 OwnerServerIdRaw,
		uint32 PhysicsServerIdRaw,
		uint32 PhysicsLocalIslandId,
		uint32 SenderServerIdRaw,
		const TArray<uint8>& SerializedData);

	// ──── Pull-Migration Request RPCs ────

	/**
	 * Request a remote server to send us a specific object.
	 * Used for pull-migration: Server-B asks Server-A to migrate an object.
	 *
	 * @param ObjectIdRaw          - FRemoteObjectId of the requested object
	 * @param RequestingServerIdRaw - FRemoteServerId of the requesting server (us)
	 */
	UFUNCTION(Server, Reliable, WithValidation)
	void ServerRequestMigrateObject(
		uint64 ObjectIdRaw,
		uint32 RequestingServerIdRaw);

	// ──── Delegates ────

	DECLARE_MULTICAST_DELEGATE_SixParams(FOnMigrationDataReceived,
		uint64 /* ObjectIdRaw */,
		uint32 /* OwnerServerIdRaw */,
		uint32 /* PhysicsServerIdRaw */,
		uint32 /* PhysicsLocalIslandId */,
		uint32 /* SenderServerIdRaw */,
		const TArray<uint8>& /* SerializedData */);

	/** Fired when migration data arrives on this beacon (from either direction). */
	FOnMigrationDataReceived OnMigrationDataReceived;

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnMigrationRequested,
		uint64 /* ObjectIdRaw */,
		uint32 /* RequestingServerIdRaw */);

	/** Fired when a peer requests us to send them an object. */
	FOnMigrationRequested OnMigrationRequested;
};
