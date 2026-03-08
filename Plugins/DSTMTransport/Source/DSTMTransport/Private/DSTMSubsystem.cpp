// Copyright Epic Games, Inc. All Rights Reserved.

#include "DSTMSubsystem.h"
#include "DSTMBeaconClient.h"
#include "MultiServerNode.h"
#include "MultiServerBeaconClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/MemoryReader.h"

#if UE_WITH_REMOTE_OBJECT_HANDLE
#include "UObject/RemoteObjectTransfer.h"
#include "UObject/RemoteObjectTypes.h"
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(DSTMSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogDSTMSub, Log, All);

// ─── Lifecycle ────────────────────────────────────────────────────

bool UDSTMSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Always create — inert until InitializeDSTMMesh() or InitializeFromCommandLine()
	return true;
}

void UDSTMSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTMSubsystem initialized (inert until DSTM mesh setup)"));

	// Auto-initialize from command line if multi-server mode is configured
	InitializeFromCommandLine();
}

void UDSTMSubsystem::Deinitialize()
{
	ShutdownMesh();
	Super::Deinitialize();
}

// ─── Mesh Management ─────────────────────────────────────────────

bool UDSTMSubsystem::InitializeFromCommandLine()
{
#if !UE_WITH_REMOTE_OBJECT_HANDLE
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM mesh not available: UE_WITH_REMOTE_OBJECT_HANDLE is disabled"));
	return false;
#else
	// Check if multi-server mode is requested via command line
	FString LocalPeerId;
	if (!FParse::Value(FCommandLine::Get(), TEXT("-MultiServerLocalId="), LocalPeerId, false))
	{
		// Not in multi-server mode — no mesh needed
		return false;
	}

	FString ListenIp = TEXT("0.0.0.0");
	FParse::Value(FCommandLine::Get(), TEXT("-MultiServerListenIp="), ListenIp, false);

	int32 BaseListenPort = 15000;
	FParse::Value(FCommandLine::Get(), TEXT("-MultiServerListenPort="), BaseListenPort);

	// Apply DSTM port offset
	const int32 DSTMListenPort = BaseListenPort + DSTMPortOffset;

	int32 NumServers = 1;
	FParse::Value(FCommandLine::Get(), TEXT("-MultiServerNumServers="), NumServers);

	FString PeersArg;
	TArray<FString> BasePeerAddresses;
	if (FParse::Value(FCommandLine::Get(), TEXT("-MultiServerPeers="), PeersArg, false))
	{
		PeersArg.ParseIntoArray(BasePeerAddresses, TEXT(","), true);
	}

	// Rewrite peer addresses with DSTM port offset
	TArray<FString> DSTMPeerAddresses = OffsetPeerPorts(BasePeerAddresses, DSTMPortOffset);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM mesh auto-init: LocalId=%s, DSTMPort=%d (base %d + offset %d), NumServers=%d, Peers=%d"),
		*LocalPeerId, DSTMListenPort, BaseListenPort, DSTMPortOffset, NumServers, DSTMPeerAddresses.Num());

	InitializeDSTMMesh(LocalPeerId, ListenIp, DSTMListenPort, NumServers, DSTMPeerAddresses);
	return DSTMNode != nullptr;
#endif
}

void UDSTMSubsystem::InitializeDSTMMesh(
	const FString& LocalPeerId,
	const FString& ListenIp,
	int32 ListenPort,
	int32 NumServers,
	const TArray<FString>& PeerAddresses)
{
	if (DSTMNode)
	{
		UE_LOG(LogDSTMSub, Warning,
			TEXT("DSTM mesh already active. Ignoring re-initialization."));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("Cannot initialize DSTM mesh — no World"));
		return;
	}

	FMultiServerNodeCreateParams Params;
	Params.World = World;
	Params.LocalPeerId = LocalPeerId;
	Params.ListenIp = ListenIp;
	Params.ListenPort = static_cast<uint16>(ListenPort);
	Params.NumServers = static_cast<uint32>(NumServers);
	Params.PeerAddresses = PeerAddresses;
	Params.UserBeaconClass = ADSTMBeaconClient::StaticClass();
	Params.OnMultiServerConnected.BindUObject(
		this, &UDSTMSubsystem::HandlePeerConnected);

	DSTMNode = UMultiServerNode::Create(Params);

	if (DSTMNode)
	{
		UE_LOG(LogDSTMSub, Log,
			TEXT("DSTM mesh created: LocalPeerId=%s, ListenPort=%d, NumServers=%d, Peers=%d"),
			*LocalPeerId, ListenPort, NumServers, PeerAddresses.Num());

		for (const FString& Addr : PeerAddresses)
		{
			UE_LOG(LogDSTMSub, Log, TEXT("  DSTM Peer: %s"), *Addr);
		}
	}
	else
	{
		UE_LOG(LogDSTMSub, Error, TEXT("Failed to create DSTM mesh"));
	}
}

bool UDSTMSubsystem::AreAllPeersConnected() const
{
	return DSTMNode && DSTMNode->AreAllServersConnected();
}

void UDSTMSubsystem::ShutdownMesh()
{
	if (DSTMNode)
	{
		UE_LOG(LogDSTMSub, Log, TEXT("Shutting down DSTM mesh"));
		// UMultiServerNode handles cleanup in BeginDestroy
		DSTMNode = nullptr;
		PeerBeacons.Empty();
		ServerIdHashToPeerId.Empty();
	}
}

// ─── Migration API ───────────────────────────────────────────────

#if UE_WITH_REMOTE_OBJECT_HANDLE

void UDSTMSubsystem::TransferActorToServer(AActor* Actor, FRemoteServerId DestServerId)
{
	if (!Actor)
	{
		UE_LOG(LogDSTMSub, Error, TEXT("TransferActorToServer: Actor is null"));
		return;
	}

	if (!DSTMNode)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("TransferActorToServer: DSTM mesh not active — cannot transfer %s"),
			*Actor->GetName());
		return;
	}

	UE_LOG(LogDSTMSub, Log,
		TEXT("TransferActorToServer: Initiating DSTM transfer of %s to server %u"),
		*Actor->GetName(), DestServerId.GetValue());

	// This one call does everything:
	// 1. Serializes the actor + all subobjects
	// 2. Calls AActor::PostMigrate(Send) — world removal, channel close with Migrated
	// 3. Calls APlayerController::PostMigrate(Send) — NoPawnPC swap, connection save
	// 4. Invokes RemoteObjectTransferDelegate → our HandleOutgoingMigration() sends via beacon
	UE::RemoteObject::Transfer::TransferObjectOwnershipToRemoteServer(Actor, DestServerId);
}

FRemoteServerId UDSTMSubsystem::GetRemoteServerIdFromString(const FString& DedicatedServerId)
{
	return FRemoteServerId(GetTypeHash(DedicatedServerId));
}

bool UDSTMSubsystem::GetFirstPeerServerId(FRemoteServerId& OutServerId) const
{
	for (const auto& Pair : ServerIdHashToPeerId)
	{
		OutServerId = FRemoteServerId(Pair.Key);
		return true;
	}
	return false;
}

// ─── Transport Delegate Handlers ──────────────────────────────────

void UDSTMSubsystem::HandleOutgoingMigration(
	const UE::RemoteObject::Transfer::FMigrateSendParams& Params)
{
	// Extract routing info from the opaque send params
	UE::RemoteObject::Transfer::FMigrationRoutingInfo Info =
		UE::RemoteObject::Transfer::GetMigrationRoutingInfo(Params);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Send: ObjectId=%llu → DestServer=%u (Owner=%u, Physics=%u)"),
		Info.ObjectId.GetValue(),
		Info.DestinationServerId.GetValue(),
		Info.OwnerServerId.GetValue(),
		Info.PhysicsServerId.GetValue());

	// Serialize FRemoteObjectData to a byte array for network transfer.
	// Make a copy since archive serialization requires a non-const reference.
	TArray<uint8> SerializedData;
	FMemoryWriter Writer(SerializedData);
	FRemoteObjectData ObjectDataCopy = Params.ObjectData;
	Writer << ObjectDataCopy;

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Send: Serialized %d bytes of object data"),
		SerializedData.Num());

	// Find the beacon connected to the destination server
	ADSTMBeaconClient* Beacon = FindBeaconForServer(
		Info.DestinationServerId.GetValue());

	if (!Beacon)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM Send: No beacon connection to destination server %u! Migration data lost."),
			Info.DestinationServerId.GetValue());
		return;
	}

	// Send via the appropriate RPC direction based on beacon authority
	const uint32 LocalServerId = FRemoteServerId::GetLocalServerId().GetValue();

	if (Beacon->HasAuthority())
	{
		// We are the server side of this beacon connection → use Client RPC
		Beacon->ClientReceiveMigratedObject(
			Info.ObjectId.GetValue(),
			Info.OwnerServerId.GetValue(),
			Info.PhysicsServerId.GetValue(),
			Info.PhysicsLocalIslandId,
			LocalServerId,
			SerializedData);
	}
	else
	{
		// We are the client side → use Server RPC
		Beacon->ServerReceiveMigratedObject(
			Info.ObjectId.GetValue(),
			Info.OwnerServerId.GetValue(),
			Info.PhysicsServerId.GetValue(),
			Info.PhysicsLocalIslandId,
			LocalServerId,
			SerializedData);
	}

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Send: Migration data dispatched via %s RPC (%d bytes)"),
		Beacon->HasAuthority() ? TEXT("Client") : TEXT("Server"),
		SerializedData.Num());
}

void UDSTMSubsystem::HandleObjectRequest(
	FRemoteWorkPriority Priority,
	FRemoteObjectId ObjectId,
	FRemoteServerId LastKnownServerId,
	FRemoteServerId DestServerId)
{
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Request: Object %llu — requesting from server %u for destination %u"),
		ObjectId.GetValue(),
		LastKnownServerId.GetValue(),
		DestServerId.GetValue());

	ADSTMBeaconClient* Beacon = FindBeaconForServer(LastKnownServerId.GetValue());
	if (Beacon)
	{
		// Send via the appropriate RPC direction based on beacon authority
		if (Beacon->HasAuthority())
		{
			// We are the server side of this beacon connection → use Client RPC
			Beacon->ClientRequestMigrateObject(
				ObjectId.GetValue(),
				DestServerId.GetValue());
		}
		else
		{
			// We are the client side → use Server RPC
			Beacon->ServerRequestMigrateObject(
				ObjectId.GetValue(),
				DestServerId.GetValue());
		}

		UE_LOG(LogDSTMSub, Log,
			TEXT("DSTM Request: Dispatched via %s RPC"),
			Beacon->HasAuthority() ? TEXT("Client") : TEXT("Server"));
	}
	else
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM Request: No beacon connection to server %u"),
			LastKnownServerId.GetValue());
	}
}

#endif // UE_WITH_REMOTE_OBJECT_HANDLE

// ─── Peer Connection Handling ─────────────────────────────────────

void UDSTMSubsystem::HandlePeerConnected(
	const FString& LocalPeerId,
	const FString& RemotePeerId,
	AMultiServerBeaconClient* Beacon)
{
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM peer connected: %s → %s"), *LocalPeerId, *RemotePeerId);

	ADSTMBeaconClient* DSTMBeacon = Cast<ADSTMBeaconClient>(Beacon);
	if (!DSTMBeacon)
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM peer connected but beacon is not ADSTMBeaconClient!"));
		return;
	}

	PeerBeacons.Add(RemotePeerId, DSTMBeacon);

	// Store reverse lookup: hash → peer ID string
	const uint32 PeerHash = GetTypeHash(RemotePeerId);
	ServerIdHashToPeerId.Add(PeerHash, RemotePeerId);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM peer registered: '%s' → hash %u"), *RemotePeerId, PeerHash);

#if UE_WITH_REMOTE_OBJECT_HANDLE
	// Wire up delegates for incoming migration data from this peer
	DSTMBeacon->OnMigrationDataReceived.AddUObject(
		this, &UDSTMSubsystem::HandleIncomingMigrationData);
	DSTMBeacon->OnMigrationRequested.AddUObject(
		this, &UDSTMSubsystem::HandleIncomingMigrationRequest);
#endif
}

#if UE_WITH_REMOTE_OBJECT_HANDLE

void UDSTMSubsystem::HandleIncomingMigrationData(
	uint64 ObjectIdRaw,
	uint32 OwnerServerIdRaw,
	uint32 PhysicsServerIdRaw,
	uint32 PhysicsLocalIslandId,
	uint32 SenderServerIdRaw,
	const TArray<uint8>& SerializedData)
{
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Recv: ObjectId=%llu, Owner=%u, Sender=%u, DataSize=%d bytes — feeding to engine"),
		ObjectIdRaw, OwnerServerIdRaw, SenderServerIdRaw, SerializedData.Num());

	// Deserialize FRemoteObjectData from the byte array
	FRemoteObjectData ObjectData;
	FMemoryReader Reader(SerializedData);
	Reader << ObjectData;

	if (Reader.IsError())
	{
		UE_LOG(LogDSTMSub, Error,
			TEXT("DSTM Recv: Failed to deserialize FRemoteObjectData — "
				"ObjectId=%llu, Owner=%u, Sender=%u, DataSize=%d bytes. "
				"Possible version mismatch or corrupted payload."),
			ObjectIdRaw, OwnerServerIdRaw, SenderServerIdRaw, SerializedData.Num());
		return;
	}

	// Feed the deserialized data into the engine's DSTM receive pipeline.
	// This triggers:
	//   1. FRemoteObjectTransferQueue::FulfillReceiveRequest()
	//   2. Deserializes the actor (same C++ object, same FRemoteObjectId)
	//   3. AActor::PostMigrate(Receive) → adds to world, starts replicating
	//   4. APlayerController::PostMigrate(Receive) → finds connection, binds PC
	UE::RemoteObject::Transfer::OnObjectDataReceived(
		FRemoteServerId(OwnerServerIdRaw),
		FRemoteServerId(PhysicsServerIdRaw),
		PhysicsLocalIslandId,
		FRemoteObjectId(ObjectIdRaw),
		FRemoteServerId(SenderServerIdRaw),
		ObjectData);

	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Recv: ObjectId=%llu delivered to engine receive pipeline"), ObjectIdRaw);
}

void UDSTMSubsystem::HandleIncomingMigrationRequest(
	uint64 ObjectIdRaw,
	uint32 RequestingServerIdRaw)
{
	UE_LOG(LogDSTMSub, Log,
		TEXT("DSTM Pull-Request: Object %llu requested by server %u — "
			"engine will initiate transfer if object is local"),
		ObjectIdRaw, RequestingServerIdRaw);

	// The engine's remote object system handles pull-request resolution internally.
	// When OnObjectDataReceived or a request comes in, the transfer queue
	// checks if the object exists locally and initiates a send if appropriate.
	// No additional handling needed here — the request delegate is primarily
	// for logging and monitoring.
}

#endif // UE_WITH_REMOTE_OBJECT_HANDLE

// ─── Utility ─────────────────────────────────────────────────────

TArray<FString> UDSTMSubsystem::OffsetPeerPorts(
	const TArray<FString>& PeerAddresses, int32 Offset)
{
	TArray<FString> Result;
	Result.Reserve(PeerAddresses.Num());

	for (const FString& Addr : PeerAddresses)
	{
		// Parse "host:port" format
		FString Host;
		FString PortStr;
		if (Addr.Split(TEXT(":"), &Host, &PortStr, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
		{
			const int32 BasePort = FCString::Atoi(*PortStr);
			const int32 OffsetPort = BasePort + Offset;
			Result.Add(FString::Printf(TEXT("%s:%d"), *Host, OffsetPort));
		}
		else
		{
			// No port specified — cannot compute offset, peer will not be reachable
			UE_LOG(LogDSTMSub, Warning,
				TEXT("DSTM OffsetPeerPorts: Address '%s' has no port separator ':' — "
					"this peer will NOT have a DSTM beacon connection. "
					"Use 'host:port' format in -MultiServerPeers="),
				*Addr);
		}
	}

	return Result;
}

ADSTMBeaconClient* UDSTMSubsystem::FindBeaconForServer(uint32 ServerIdHash) const
{
	const FString* PeerId = ServerIdHashToPeerId.Find(ServerIdHash);
	if (!PeerId)
	{
		return nullptr;
	}

	const TObjectPtr<ADSTMBeaconClient>* Beacon = PeerBeacons.Find(*PeerId);
	return Beacon ? Beacon->Get() : nullptr;
}
