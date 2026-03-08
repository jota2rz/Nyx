// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxDSTMBeaconClient.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NyxDSTMBeaconClient)

DEFINE_LOG_CATEGORY_STATIC(LogNyxDSTMBeacon, Log, All);

// ─── Constructor ──────────────────────────────────────────────────

ANyxDSTMBeaconClient::ANyxDSTMBeaconClient()
{
}

// ─── Connection ───────────────────────────────────────────────────

void ANyxDSTMBeaconClient::OnConnected()
{
	Super::OnConnected();

	UE_LOG(LogNyxDSTMBeacon, Log,
		TEXT("NyxDSTMBeacon: Connected to peer %s (local=%s) — DSTM transport ready"),
		*GetRemotePeerId(), *GetLocalPeerId());
}

// ─── Migration Data Transfer RPCs ─────────────────────────────────

void ANyxDSTMBeaconClient::ServerReceiveMigratedObject_Implementation(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	const TArray<uint8>& SerializedData)
{
	UE_LOG(LogNyxDSTMBeacon, Log,
		TEXT("DSTM Recv [Server RPC]: ObjectId=%llu, Owner=%u, Physics=%u, Sender=%u, DataSize=%d bytes"),
		ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw, SenderServerIdRaw, SerializedData.Num());

	OnMigrationDataReceived.Broadcast(
		ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw,
		PhysicsLocalIslandId, SenderServerIdRaw, SerializedData);
}

void ANyxDSTMBeaconClient::ClientReceiveMigratedObject_Implementation(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	const TArray<uint8>& SerializedData)
{
	UE_LOG(LogNyxDSTMBeacon, Log,
		TEXT("DSTM Recv [Client RPC]: ObjectId=%llu, Owner=%u, Physics=%u, Sender=%u, DataSize=%d bytes"),
		ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw, SenderServerIdRaw, SerializedData.Num());

	OnMigrationDataReceived.Broadcast(
		ObjectIdRaw, OwnerServerIdRaw, PhysicsServerIdRaw,
		PhysicsLocalIslandId, SenderServerIdRaw, SerializedData);
}

// ─── Pull-Migration Request RPCs ──────────────────────────────────

bool ANyxDSTMBeaconClient::ServerRequestMigrateObject_Validate(
	uint64 ObjectIdRaw,
	uint32 RequestingServerIdRaw)
{
	// Basic validation: object ID should be non-zero
	return ObjectIdRaw != 0;
}

void ANyxDSTMBeaconClient::ServerRequestMigrateObject_Implementation(
	uint64 ObjectIdRaw,
	uint32 RequestingServerIdRaw)
{
	UE_LOG(LogNyxDSTMBeacon, Log,
		TEXT("DSTM Request: ObjectId=%llu requested by server %u"),
		ObjectIdRaw, RequestingServerIdRaw);

	OnMigrationRequested.Broadcast(ObjectIdRaw, RequestingServerIdRaw);
}
