// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxMultiServerSubsystem.h"
#include "NyxMultiServerBeaconClient.h"
#include "MultiServerNode.h"
#include "MultiServerBeaconClient.h"
#include "Nyx/Nyx.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NyxMultiServerSubsystem)

DEFINE_LOG_CATEGORY_STATIC(LogNyxMultiServerSub, Log, All);

// ─── Lifecycle ─────────────────────────────────────────────────────

bool UNyxMultiServerSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	// Always create — inert until InitializeMultiServerMesh() is called.
	// The subsystem works on dedicated servers, listen servers, and standalone (with flags).
	return true;
}

void UNyxMultiServerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogNyxMultiServerSub, Log, TEXT("NyxMultiServerSubsystem initialized (inert until mesh setup)"));
}

void UNyxMultiServerSubsystem::Deinitialize()
{
	ShutdownMesh();
	Super::Deinitialize();
}

// ─── Mesh Management ──────────────────────────────────────────────

void UNyxMultiServerSubsystem::InitializeMultiServerMesh(
	const FString& LocalPeerId,
	const FString& ListenIp,
	int32 ListenPort,
	int32 NumServers,
	const TArray<FString>& PeerAddresses)
{
	if (MultiServerNode)
	{
		UE_LOG(LogNyxMultiServerSub, Warning, TEXT("MultiServer mesh already active. Ignoring re-init."));
		return;
	}

	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogNyxMultiServerSub, Error, TEXT("Cannot initialize MultiServer mesh — no World"));
		return;
	}

	FMultiServerNodeCreateParams Params;
	Params.World = World;
	Params.LocalPeerId = LocalPeerId;
	Params.ListenIp = ListenIp;
	Params.ListenPort = static_cast<uint16>(ListenPort);
	Params.NumServers = static_cast<uint32>(NumServers);
	Params.PeerAddresses = PeerAddresses;
	Params.UserBeaconClass = ANyxMultiServerBeaconClient::StaticClass();
	Params.OnMultiServerConnected.BindUObject(this, &UNyxMultiServerSubsystem::HandlePeerConnected);

	MultiServerNode = UMultiServerNode::Create(Params);

	if (MultiServerNode)
	{
		UE_LOG(LogNyxMultiServerSub, Log,
			TEXT("MultiServer mesh created: LocalPeerId=%s, ListenPort=%d, NumServers=%d, Peers=%d"),
			*LocalPeerId, ListenPort, NumServers, PeerAddresses.Num());
	}
	else
	{
		UE_LOG(LogNyxMultiServerSub, Error, TEXT("Failed to create MultiServer mesh"));
	}
}

bool UNyxMultiServerSubsystem::InitializeFromCommandLine()
{
	// Check if multi-server mode is requested via command line
	FString LocalPeerId;
	if (!FParse::Value(FCommandLine::Get(), TEXT("-NyxMultiServerLocalId="), LocalPeerId, false))
	{
		// Not in multi-server mode
		return false;
	}

	FString ListenIp = TEXT("0.0.0.0");
	FParse::Value(FCommandLine::Get(), TEXT("-NyxMultiServerListenIp="), ListenIp, false);

	int32 ListenPort = 15000;
	FParse::Value(FCommandLine::Get(), TEXT("-NyxMultiServerListenPort="), ListenPort);

	int32 NumServers = 1;
	FParse::Value(FCommandLine::Get(), TEXT("-MultiServerNumServers="), NumServers);

	FString PeersArg;
	TArray<FString> PeerAddresses;
	if (FParse::Value(FCommandLine::Get(), TEXT("-MultiServerPeers="), PeersArg, false))
	{
		PeersArg.ParseIntoArray(PeerAddresses, TEXT(","), true);
	}

	UE_LOG(LogNyxMultiServerSub, Log,
		TEXT("MultiServer mode detected from command line: LocalId=%s, ListenPort=%d, NumServers=%d"),
		*LocalPeerId, ListenPort, NumServers);

	InitializeMultiServerMesh(LocalPeerId, ListenIp, ListenPort, NumServers, PeerAddresses);
	return true;
}

bool UNyxMultiServerSubsystem::AreAllPeersConnected() const
{
	return MultiServerNode && MultiServerNode->AreAllServersConnected();
}

void UNyxMultiServerSubsystem::ShutdownMesh()
{
	if (MultiServerNode)
	{
		UE_LOG(LogNyxMultiServerSub, Log, TEXT("Shutting down MultiServer mesh"));
		// UMultiServerNode handles cleanup in BeginDestroy
		MultiServerNode = nullptr;
		PeerBeacons.Empty();
	}
}

// ─── Cross-Server Operations ──────────────────────────────────────

void UNyxMultiServerSubsystem::SendCrossServerHit(
	const FString& PeerServerId,
	const FString& AttackerIdentityHex,
	const FString& DefenderIdentityHex,
	int32 SkillId)
{
	TObjectPtr<ANyxMultiServerBeaconClient>* Beacon = PeerBeacons.Find(PeerServerId);
	if (Beacon && *Beacon)
	{
		(*Beacon)->ServerNotifyCrossServerHit(AttackerIdentityHex, DefenderIdentityHex, SkillId);
	}
	else
	{
		UE_LOG(LogNyxMultiServerSub, Warning,
			TEXT("Cannot send cross-server hit to %s — no beacon connection"), *PeerServerId);
	}
}

void UNyxMultiServerSubsystem::RequestEntityHandoff(
	const FString& PeerServerId,
	const FString& EntityIdentityHex,
	const FString& Reason)
{
	TObjectPtr<ANyxMultiServerBeaconClient>* Beacon = PeerBeacons.Find(PeerServerId);
	if (Beacon && *Beacon)
	{
		(*Beacon)->ServerRequestEntityHandoff(EntityIdentityHex, Reason);
	}
	else
	{
		UE_LOG(LogNyxMultiServerSub, Warning,
			TEXT("Cannot request entity handoff to %s — no beacon connection"), *PeerServerId);
	}
}

void UNyxMultiServerSubsystem::BroadcastEntityCount(const FString& ServerId, int32 EntityCount)
{
	if (!MultiServerNode)
	{
		return;
	}

	for (auto& Pair : PeerBeacons)
	{
		if (Pair.Value)
		{
			Pair.Value->ServerBroadcastEntityCount(ServerId, EntityCount);
		}
	}
}

// ─── Callbacks ────────────────────────────────────────────────────

void UNyxMultiServerSubsystem::HandlePeerConnected(
	const FString& LocalPeerId, const FString& RemotePeerId,
	AMultiServerBeaconClient* Beacon)
{
	UE_LOG(LogNyxMultiServerSub, Log,
		TEXT("Peer connected: %s -> %s"), *LocalPeerId, *RemotePeerId);

	ANyxMultiServerBeaconClient* NyxBeacon = Cast<ANyxMultiServerBeaconClient>(Beacon);
	if (NyxBeacon)
	{
		PeerBeacons.Add(RemotePeerId, NyxBeacon);

		// Wire up delegates for incoming RPCs from this peer
		NyxBeacon->OnCrossServerHitReceived.AddUObject(
			this, &UNyxMultiServerSubsystem::HandleCrossServerHit);
		NyxBeacon->OnEntityHandoffRequested.AddUObject(
			this, &UNyxMultiServerSubsystem::HandleEntityHandoff);
	}

	OnPeerConnected.Broadcast(RemotePeerId);
}

void UNyxMultiServerSubsystem::HandleCrossServerHit(
	const FString& AttackerHex, const FString& DefenderHex, int32 SkillId)
{
	// Forward to Blueprint/subsystem consumers
	OnCrossServerHitReceived.Broadcast(AttackerHex, DefenderHex, SkillId);
}

void UNyxMultiServerSubsystem::HandleEntityHandoff(
	const FString& EntityHex, const FString& Reason)
{
	OnEntityHandoffRequested.Broadcast(EntityHex, Reason);
}
