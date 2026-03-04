// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxMultiServerBeaconClient.h"
#include "Nyx/Nyx.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(NyxMultiServerBeaconClient)

DEFINE_LOG_CATEGORY_STATIC(LogNyxMultiServer, Log, All);

// ─── Constructor ───────────────────────────────────────────────────

ANyxMultiServerBeaconClient::ANyxMultiServerBeaconClient()
{
}

// ─── Connection ────────────────────────────────────────────────────

void ANyxMultiServerBeaconClient::OnConnected()
{
	Super::OnConnected();

	UE_LOG(LogNyxMultiServer, Log, TEXT("NyxMultiServer: Connected to peer %s (local=%s)"),
		*GetRemotePeerId(), *GetLocalPeerId());
}

// ─── Cross-Server Combat RPCs ──────────────────────────────────────

bool ANyxMultiServerBeaconClient::ServerNotifyCrossServerHit_Validate(
	const FString& AttackerIdentityHex, const FString& DefenderIdentityHex, int32 SkillId)
{
	// Basic validation: identity hex strings should be non-empty
	return !AttackerIdentityHex.IsEmpty() && !DefenderIdentityHex.IsEmpty();
}

void ANyxMultiServerBeaconClient::ServerNotifyCrossServerHit_Implementation(
	const FString& AttackerIdentityHex, const FString& DefenderIdentityHex, int32 SkillId)
{
	UE_LOG(LogNyxMultiServer, Log, TEXT("Cross-server hit: attacker=%s defender=%s skill=%d"),
		*AttackerIdentityHex, *DefenderIdentityHex, SkillId);

	OnCrossServerHitReceived.Broadcast(AttackerIdentityHex, DefenderIdentityHex, SkillId);
}

// ─── Entity Authority Handoff RPCs ─────────────────────────────────

bool ANyxMultiServerBeaconClient::ServerRequestEntityHandoff_Validate(
	const FString& EntityIdentityHex, const FString& Reason)
{
	return !EntityIdentityHex.IsEmpty();
}

void ANyxMultiServerBeaconClient::ServerRequestEntityHandoff_Implementation(
	const FString& EntityIdentityHex, const FString& Reason)
{
	UE_LOG(LogNyxMultiServer, Log, TEXT("Entity handoff request: entity=%s reason=%s from peer=%s"),
		*EntityIdentityHex, *Reason, *GetRemotePeerId());

	OnEntityHandoffRequested.Broadcast(EntityIdentityHex, Reason);
}

void ANyxMultiServerBeaconClient::ClientEntityHandoffAccepted_Implementation(
	const FString& EntityIdentityHex)
{
	UE_LOG(LogNyxMultiServer, Log, TEXT("Entity handoff accepted: entity=%s by peer=%s"),
		*EntityIdentityHex, *GetRemotePeerId());
}

// ─── Zone Population Sync ──────────────────────────────────────────

bool ANyxMultiServerBeaconClient::ServerBroadcastEntityCount_Validate(
	const FString& ServerId, int32 EntityCount)
{
	return !ServerId.IsEmpty() && EntityCount >= 0;
}

void ANyxMultiServerBeaconClient::ServerBroadcastEntityCount_Implementation(
	const FString& ServerId, int32 EntityCount)
{
	UE_LOG(LogNyxMultiServer, Verbose, TEXT("Peer entity count: server=%s count=%d"),
		*ServerId, EntityCount);

	OnPeerEntityCountReceived.Broadcast(ServerId, EntityCount);
}
