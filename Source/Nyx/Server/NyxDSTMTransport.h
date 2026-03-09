// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#if UE_WITH_REMOTE_OBJECT_HANDLE

#include "UObject/RemoteObjectTransfer.h"
#include "UObject/RemoteObjectTypes.h"

/**
 * NyxDSTMTransport — Shared-disk DSTM transport for cross-server object migration.
 *
 * Both game servers run on the same machine and share the Nyx project's Saved/ directory.
 * This transport overrides the default DSTM delegate bindings to use a common shared
 * path (instead of per-server paths) so Server-A's serialized objects can be read by Server-B.
 *
 * Usage:
 *   1. Call InitializeDSTMTransport(ServerId) early in server startup (NyxGameMode::StartPlay)
 *   2. Server-A: TransferObjectOwnershipToRemoteServer(PC, DestServerId) — auto-serializes
 *   3. Server-B: MigrateObjectFromRemoteServer(ObjectId, OwnerServerId) — auto-deserializes
 *
 * The transport signals Server-B via SpacetimeDB (existing migration signal mechanism).
 */
namespace NyxDSTMTransport
{
	/**
	 * Initialize the DSTM transport system for this server.
	 * Sets this server's FRemoteServerId and binds transport delegates.
	 * Must be called once per server, ideally in NyxGameMode::StartPlay().
	 *
	 * @param ServerIdString Unique string ID for this server (e.g., "server-1", "server-2")
	 */
	void InitializeDSTMTransport(const FString& ServerIdString);

	/**
	 * Get the FRemoteServerId for a named server.
	 * Uses FRemoteServerId::FromString().
	 */
	FRemoteServerId GetServerIdFromString(const FString& ServerIdString);

	/**
	 * Trigger a migration receive from disk.
	 * Called when SpacetimeDB signals that data is available.
	 *
	 * @param ObjectId The remote object ID to load
	 * @param OwnerServerId The server that serialized the object
	 */
	void ReceiveMigratedObject(FRemoteObjectId ObjectId, FRemoteServerId OwnerServerId);

	/** Returns the shared directory path for DSTM serialized objects. */
	FString GetSharedTransportDir();

	/** Generate a shared filename for a remote object (common to all servers). */
	FString GenerateSharedFilename(FRemoteObjectId ObjectId, FRemoteServerId OwnerServerId);
}

#endif // UE_WITH_REMOTE_OBJECT_HANDLE
