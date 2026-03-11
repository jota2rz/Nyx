// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxGameMode.h"
#include "Nyx/Nyx.h"
#include "Nyx/Core/NyxGameInstance.h"
#include "Nyx/Online/NyxAuthSubsystem.h"
#include "Nyx/Player/NyxCharacter.h"
#include "Nyx/Player/NyxPlayerController.h"
#include "Nyx/Server/NyxServerSubsystem.h"
#include "Nyx/Networking/NyxMultiServerSubsystem.h"
#include "Nyx/World/NyxZoneBoundary.h"
#include "Nyx/UI/NyxHUD.h"
#include "DSTMSubsystem.h"
#include "ProxyRegistrationBeaconClient.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/ChildConnection.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "EngineUtils.h"
#include "Engine/NetConnection.h"
#include "Misc/CommandLine.h"

ANyxGameMode::ANyxGameMode()
{
	// Default pawn is NyxCharacter — standard ACharacter with CMC
	DefaultPawnClass = ANyxCharacter::StaticClass();
	PlayerControllerClass = ANyxPlayerController::StaticClass();
	HUDClass = ANyxHUD::StaticClass();
}

void ANyxGameMode::StartPlay()
{
	Super::StartPlay();

	// GuidSeed is not needed with DSTM (UE_WITH_REMOTE_OBJECT_HANDLE=1).
	// FNetworkGUIDs are derived from FRemoteObjectId which embeds each
	// server's 10-bit ServerId — no seed-based collision avoidance required.

	const bool bServer = IsNyxServer();
	UE_LOG(LogNyx, Log, TEXT("NyxGameMode::StartPlay (IsNyxServer=%s  NetMode=%d)"),
		bServer ? TEXT("true") : TEXT("false"), static_cast<int32>(GetNetMode()));

	// ── Proxy Server: skip all game logic (proxy only forwards replication) ──
	if (bServer && IsProxyServer())
	{
		UE_LOG(LogNyx, Log, TEXT("Running as PROXY server — skipping SpacetimeDB, zone transfer, and game logic"));
		return;
	}

	if (bServer)
	{
		// ── Dedicated Server: connect to SpacetimeDB ──
		UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
		if (ServerSub)
		{
			// Parse config from command-line overrides or use defaults
			// bShouldStopOnSeparator=false so host:port values aren't truncated at the colon
			FString CmdHost, CmdDB, CmdZone, CmdServerId;
			if (FParse::Value(FCommandLine::Get(), TEXT("-SpacetimeHost="), CmdHost, false))
			{
				SpacetimeDBHost = CmdHost;
			}
			if (FParse::Value(FCommandLine::Get(), TEXT("-SpacetimeDB="), CmdDB, false))
			{
				DatabaseName = CmdDB;
			}
			if (FParse::Value(FCommandLine::Get(), TEXT("-ZoneId="), CmdZone, false))
			{
				ZoneId = CmdZone;
			}
			if (FParse::Value(FCommandLine::Get(), TEXT("-DedicatedServerId="), CmdServerId, false))
			{
				DedicatedServerId = CmdServerId;
			}

			UE_LOG(LogNyx, Log, TEXT("DediServer connecting to SpacetimeDB: Host=%s DB=%s Zone=%s Server=%s"),
				*SpacetimeDBHost, *DatabaseName, *ZoneId, *DedicatedServerId);

			ServerSub->ConnectAndRegister(SpacetimeDBHost, DatabaseName, ZoneId, DedicatedServerId, 500);

			// ── Optional: auto-join proxy on startup ──
			// If -JoinProxy= is provided, connect immediately. Otherwise the
			// server starts standalone and can be told to join a proxy later.
			FString JoinProxyArg;
			if (FParse::Value(FCommandLine::Get(), TEXT("-JoinProxy="), JoinProxyArg, false))
			{
				UE_LOG(LogNyx, Log, TEXT("JoinProxy: auto-connecting to proxy at %s"), *JoinProxyArg);
				ConnectToProxy(JoinProxyArg);
			}

			// ── Zone Transfer config ──
			FString CmdTransferAddr;
			if (FParse::Value(FCommandLine::Get(), TEXT("-TransferAddress="), CmdTransferAddr, false))
			{
				TransferAddress = CmdTransferAddr;
			}
			FString CmdOwnsSide;
			if (FParse::Value(FCommandLine::Get(), TEXT("-ZoneSide="), CmdOwnsSide, false))
			{
				bOwnsNegativeSide = CmdOwnsSide.Equals(TEXT("west"), ESearchCase::IgnoreCase);
			}

			UE_LOG(LogNyx, Log, TEXT("Zone config: BoundaryX=%.0f, OwnsSide=%s, TransferAddr=%s"),
				ZoneBoundaryX,
				bOwnsNegativeSide ? TEXT("west (X<0)") : TEXT("east (X>=0)"),
				TransferAddress.IsEmpty() ? TEXT("(none)") : *TransferAddress);

			// Spawn zone boundary visual markers
			FActorSpawnParameters SpawnParams;
			SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ANyxZoneBoundary* Boundary = GetWorld()->SpawnActor<ANyxZoneBoundary>(
				ANyxZoneBoundary::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
			if (Boundary)
			{
				Boundary->BoundaryX = ZoneBoundaryX;
				// Color pillars by zone side so players can tell which server they're on
				Boundary->PillarColor = bOwnsNegativeSide
					? FLinearColor(0.f, 0.5f, 1.f, 1.f)   // West/server-1: Cyan
					: FLinearColor(1.f, 0.4f, 0.f, 1.f);  // East/server-2: Orange
				Boundary->ZoneLabel = bOwnsNegativeSide
					? FString::Printf(TEXT("\u2190 WEST ZONE (Server-1)"))
					: FString::Printf(TEXT("EAST ZONE (Server-2) \u2192"));
				UE_LOG(LogNyx, Log, TEXT("Zone boundary markers spawned at X=%.0f (%s pillars)"),
					ZoneBoundaryX, bOwnsNegativeSide ? TEXT("cyan") : TEXT("orange"));
			}

			// Start zone boundary checking timer (every 0.5s)
			// Always runs: updates zone info for proxy players + handles transfers for direct players
			GetWorld()->GetTimerManager().SetTimer(ZoneCheckTimerHandle,
				FTimerDelegate::CreateUObject(this, &ANyxGameMode::CheckZoneBoundaries),
				0.5f, true);
			UE_LOG(LogNyx, Log, TEXT("Zone boundary checking started (0.5s interval). TransferAddr=%s"),
				TransferAddress.IsEmpty() ? TEXT("(none - proxy mode)") : *TransferAddress);

#if UE_WITH_REMOTE_OBJECT_HANDLE
			// Subscribe to DSTM arrival delegate so HandleMigratedPlayerArrival
			// fires immediately when the PC lands — no 0.5s polling delay.
			UDSTMSubsystem* DSTMSub = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();
			if (DSTMSub && DSTMSub->IsMeshActive())
			{
				DSTMSub->OnActorArrived.AddUObject(this, &ANyxGameMode::OnDSTMActorArrived);
				UE_LOG(LogNyx, Log, TEXT("Subscribed to DSTM OnActorArrived delegate"));
			}
#endif
		}
		else
		{
			UE_LOG(LogNyx, Error, TEXT("NyxServerSubsystem not found! ShouldCreateSubsystem returned false?"));
		}
	}
	else if (bAutoLoginMock)
	{
		// ── Standalone/PIE: mock auto-login for development ──
		UE_LOG(LogNyx, Log, TEXT("Auto-login with mock backend enabled"));

		UNyxGameInstance* GI = Cast<UNyxGameInstance>(GetGameInstance());
		if (GI)
		{
			GI->StartGame(/*bUseMock=*/true);
		}
		else
		{
			UE_LOG(LogNyx, Warning,
				TEXT("GameInstance is not UNyxGameInstance. Set GameInstanceClass in Project Settings."));
		}
	}
}

// ─── Proxy Registration ───────────────────────────────────────────

void ANyxGameMode::ConnectToProxy(const FString& ProxyAddress)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		UE_LOG(LogNyx, Error, TEXT("ConnectToProxy: no World"));
		return;
	}

	// Build registration from command-line args
	FProxyServerRegistration Registration;
	FParse::Value(FCommandLine::Get(), TEXT("-DedicatedServerId="), Registration.ServerId, false);

	// GameHost: the IP peers should use to reach us
	Registration.GameHost = TEXT("127.0.0.1");
	FParse::Value(FCommandLine::Get(), TEXT("-GameHost="), Registration.GameHost, false);

	// Game port: from -port= (UE default parsing)
	Registration.GamePort = World->URL.Port;

	// Mesh listen ports
	FParse::Value(FCommandLine::Get(), TEXT("-NyxMultiServerListenPort="), Registration.MultiServerListenPort);
	FParse::Value(FCommandLine::Get(), TEXT("-DSTMListenPort="), Registration.DSTMListenPort);

	UE_LOG(LogNyx, Warning,
		TEXT("ConnectToProxy: registering '%s' @ %s:%d (DSTM=%d, MS=%d) with proxy %s"),
		*Registration.ServerId, *Registration.GameHost, Registration.GamePort,
		Registration.DSTMListenPort, Registration.MultiServerListenPort,
		*ProxyAddress);

	// Spawn the beacon client and connect
	ProxyRegistrationBeacon = World->SpawnActor<AProxyRegistrationBeaconClient>(
		AProxyRegistrationBeaconClient::StaticClass());

	if (!ProxyRegistrationBeacon)
	{
		UE_LOG(LogNyx, Error, TEXT("ConnectToProxy: failed to spawn AProxyRegistrationBeaconClient"));
		return;
	}

	ProxyRegistrationBeacon->SetRegistration(Registration);

	// Subscribe to peer list updates
	ProxyRegistrationBeacon->OnPeerListReceived.AddUObject(
		this, &ANyxGameMode::HandleProxyPeerListReceived);

	ProxyRegistrationBeacon->ConnectToProxy(ProxyAddress);
}

void ANyxGameMode::HandleProxyPeerListReceived(const TArray<FProxyPeerInfo>& Peers)
{
	UE_LOG(LogNyx, Warning, TEXT("HandleProxyPeerListReceived: %d peer(s)"), Peers.Num());

	if (Peers.Num() == 0)
	{
		UE_LOG(LogNyx, Log, TEXT("HandleProxyPeerListReceived: empty peer list — waiting for peers"));
		return;
	}

	if (bMeshesInitializedFromProxy)
	{
		// TODO: dynamic AddPeer/RemovePeer for scaling beyond initial peer set
		UE_LOG(LogNyx, Warning,
			TEXT("HandleProxyPeerListReceived: meshes already initialized — dynamic peer add not yet implemented"));
		return;
	}

	bMeshesInitializedFromProxy = true;

	// Parse our own identity
	FString LocalPeerId;
	FParse::Value(FCommandLine::Get(), TEXT("-DedicatedServerId="), LocalPeerId, false);

	FString ListenIp = TEXT("0.0.0.0");
	FParse::Value(FCommandLine::Get(), TEXT("-NyxMultiServerListenIp="), ListenIp, false);

	// ── Initialize DSTM mesh from peer list ──
	int32 DSTMListenPort = 16000;
	FParse::Value(FCommandLine::Get(), TEXT("-DSTMListenPort="), DSTMListenPort);

	TArray<FString> DSTMPeerAddresses;
	for (const FProxyPeerInfo& Peer : Peers)
	{
		if (Peer.DSTMListenPort > 0)
		{
			DSTMPeerAddresses.Add(FString::Printf(TEXT("%s:%d"), *Peer.Host, Peer.DSTMListenPort));
		}
	}

	UDSTMSubsystem* DSTMSub = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();
	if (DSTMSub && DSTMPeerAddresses.Num() > 0)
	{
		DSTMSub->InitializeDSTMMesh(LocalPeerId, ListenIp, DSTMListenPort, DSTMPeerAddresses);
		UE_LOG(LogNyx, Warning, TEXT("DSTM mesh initialized from proxy peer list (%d peers)"), DSTMPeerAddresses.Num());

#if UE_WITH_REMOTE_OBJECT_HANDLE
		// Subscribe to DSTM arrival delegate now that mesh is active.
		// In JoinProxy mode, the mesh wasn't ready during StartPlay().
		if (DSTMSub->IsMeshActive())
		{
			DSTMSub->OnActorArrived.AddUObject(this, &ANyxGameMode::OnDSTMActorArrived);
			UE_LOG(LogNyx, Warning, TEXT("Subscribed to DSTM OnActorArrived delegate (from proxy peer list)"));
		}
#endif
	}

	// ── Initialize MultiServer mesh from peer list ──
	int32 MSListenPort = 15000;
	FParse::Value(FCommandLine::Get(), TEXT("-NyxMultiServerListenPort="), MSListenPort);

	TArray<FString> MSPeerAddresses;
	for (const FProxyPeerInfo& Peer : Peers)
	{
		if (Peer.MultiServerListenPort > 0)
		{
			MSPeerAddresses.Add(FString::Printf(TEXT("%s:%d"), *Peer.Host, Peer.MultiServerListenPort));
		}
	}

	UNyxMultiServerSubsystem* MultiSub = GetGameInstance()->GetSubsystem<UNyxMultiServerSubsystem>();
	if (MultiSub && MSPeerAddresses.Num() > 0)
	{
		MultiSub->InitializeMultiServerMesh(LocalPeerId, ListenIp, MSListenPort, MSPeerAddresses);
		UE_LOG(LogNyx, Warning, TEXT("MultiServer mesh initialized from proxy peer list (%d peers)"), MSPeerAddresses.Num());
	}
}

// ─── Lifecycle ────────────────────────────────────────────────────

void ANyxGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IsNyxServer())
	{
		GetWorld()->GetTimerManager().ClearTimer(ZoneCheckTimerHandle);

		// Clean up proxy registration beacon
		if (ProxyRegistrationBeacon)
		{
			ProxyRegistrationBeacon->DestroyBeacon();
			ProxyRegistrationBeacon = nullptr;
		}

		UNyxMultiServerSubsystem* MultiSub = GetGameInstance()->GetSubsystem<UNyxMultiServerSubsystem>();
		if (MultiSub)
		{
			MultiSub->ShutdownMesh();
		}

		UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
		if (ServerSub)
		{
			ServerSub->Shutdown();
		}
	}

	Super::EndPlay(EndPlayReason);
}

void ANyxGameMode::PostLogin(APlayerController* NewPlayer)
{
	// Proxy server must NOT call Super::PostLogin — that triggers RestartPlayer() /
	// SpawnDefaultPawnFor(), which creates a local pawn that conflicts with the
	// game server's pawn forwarded through the MultiServer proxy.
	if (IsProxyServer())
	{
		UE_LOG(LogNyx, Log, TEXT("PostLogin (PROXY): %s — skipping pawn spawn"),
			NewPlayer ? *NewPlayer->GetName() : TEXT("NULL"));
		return;
	}

	Super::PostLogin(NewPlayer);

	if (!NewPlayer) return;

	UE_LOG(LogNyx, Log, TEXT("PostLogin: %s (PlayerName=%s)"),
		*NewPlayer->GetName(),
		NewPlayer->PlayerState ? *NewPlayer->PlayerState->GetPlayerName() : TEXT("(no PlayerState)"));

	if (IsNyxServer())
	{
		// Record arrival time for transfer grace period
		TransferArrivalTimes.Add(NewPlayer, GetWorld()->GetTimeSeconds());

		// Get the spawned NyxCharacter pawn
		ANyxCharacter* NyxChar = Cast<ANyxCharacter>(NewPlayer->GetPawn());
		if (NyxChar)
		{
			// Set server/zone info for HUD display (replicated to client)
			// Server = always the authority server's ID (the one that owns the pawn)
			// Zone = determined by spawn position relative to boundary
			NyxChar->ServerName = DedicatedServerId;
			const float SpawnX = NyxChar->GetActorLocation().X;
			NyxChar->ZoneName = (SpawnX < ZoneBoundaryX)
				? TEXT("Zone-1 (West)")
				: TEXT("Zone-2 (East)");

			// Register with SpacetimeDB for persistence
			UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
			if (ServerSub)
			{
				// Use the stable login name from the connection (e.g. "DESKTOP-K4TB77K-AB796F4A...")
				// NOT GetName() which returns the UObject name ("PlayerController_XXXXXXXX")
				// and changes on every server connection, creating duplicate SpacetimeDB rows.
				FString PlayerName = NewPlayer->PlayerState
					? NewPlayer->PlayerState->GetPlayerName()
					: NewPlayer->GetName();
				ServerSub->OnPlayerJoined(NyxChar, PlayerName);
			}

			// ── Clamp restored position to owning zone ──
			// SpacetimeDB may restore a position from a previous session that's
			// in the OTHER server's zone. Clamp to prevent immediate transfer cascade.
			const FVector RestoredPos = NyxChar->GetActorLocation();
			const bool bRestoredInOurZone = bOwnsNegativeSide
				? (RestoredPos.X < ZoneBoundaryX)
				: (RestoredPos.X >= ZoneBoundaryX);
			if (!bRestoredInOurZone)
			{
				const float SafeX = bOwnsNegativeSide
					? (ZoneBoundaryX - 100.f)
					: (ZoneBoundaryX + 100.f);
				const FVector SafePos(SafeX, RestoredPos.Y, RestoredPos.Z);
				NyxChar->SetActorLocation(SafePos);
				UE_LOG(LogNyx, Warning, TEXT("PostLogin: Clamped %s from X=%.0f to X=%.0f — saved position was outside our zone %s"),
					*NyxChar->GetName(), RestoredPos.X, SafeX,
					bOwnsNegativeSide ? TEXT("west") : TEXT("east"));
			}
		}
		else
		{
			UE_LOG(LogNyx, Warning, TEXT("PostLogin: Player pawn is not ANyxCharacter"));
		}
	}
}

APawn* ANyxGameMode::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
	// Proxy server must NOT spawn pawns — the game server spawns them and
	// the MultiServer proxy forwards replication data to clients.
	if (IsProxyServer())
	{
		return nullptr;
	}
	return Super::SpawnDefaultPawnFor_Implementation(NewPlayer, StartSpot);
}

void ANyxGameMode::Logout(AController* Exiting)
{
	if (IsNyxServer() && Exiting)
	{
		APlayerController* PC = Cast<APlayerController>(Exiting);
		if (PC)
		{
			PlayersBeingTransferred.Remove(PC);
			TransferArrivalTimes.Remove(PC);
		}

		ANyxCharacter* NyxChar = Cast<ANyxCharacter>(Exiting->GetPawn());
		if (NyxChar)
		{
			UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
			if (ServerSub)
			{
				ServerSub->OnPlayerLeft(NyxChar);
			}
		}
	}

	Super::Logout(Exiting);
}

void ANyxGameMode::EnterWorld()
{
	UE_LOG(LogNyx, Log, TEXT("EnterWorld: Legacy/standalone path"));

	// This path is for standalone/PIE development only.
	// On dedicated server, PostLogin handles everything.
	if (IsRunningDedicatedServer())
	{
		UE_LOG(LogNyx, Warning, TEXT("EnterWorld called on dedicated server — this should not happen"));
		return;
	}

	// Legacy: standalone mode placeholder
	UE_LOG(LogNyx, Log, TEXT("EnterWorld: Standalone mode — no SpacetimeDB connection from client"));
}

bool ANyxGameMode::IsNyxServer() const
{
	// True for dedicated servers, listen servers, and PIE "Play As Listen Server"
	return IsRunningDedicatedServer()
		|| GetNetMode() == NM_ListenServer
		|| GetNetMode() == NM_DedicatedServer;
}

void ANyxGameMode::CheckZoneBoundaries()
{
	UWorld* World = GetWorld();
	if (!World) return;

	const double CurrentTime = World->GetTimeSeconds();

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		APlayerController* PC = It->Get();
		if (!PC || PlayersBeingTransferred.Contains(PC)) continue;

		// Skip players still in transfer grace period (prevents ping-pong)
		if (const double* ArrivalTime = TransferArrivalTimes.Find(PC))
		{
			if (CurrentTime - *ArrivalTime < TransferGracePeriodSeconds)
			{
				continue;
			}
		}

		const bool bIsChildConnection = Cast<UChildConnection>(PC->GetNetConnection()) != nullptr;

		// ── Regular PlayerController with a pawn ──
		ANyxCharacter* NyxChar = Cast<ANyxCharacter>(PC->GetPawn());

#if UE_WITH_REMOTE_OBJECT_HANDLE
		// Fallback: if the delegate-based arrival didn't fully process this PC
		// (e.g. pawn wasn't ready yet), retry here.
		if (!NyxChar && PC->Player && !PC->IsA(ANoPawnPlayerController::StaticClass())
			&& bIsChildConnection && !TransferArrivalTimes.Contains(PC))
		{
			HandleMigratedPlayerArrival(PC);
			NyxChar = Cast<ANyxCharacter>(PC->GetPawn());
		}
#endif

		if (!NyxChar) continue;

		const float PlayerX = NyxChar->GetActorLocation().X;
		const bool bPlayerInNegativeSide = (PlayerX < ZoneBoundaryX);

		// Update replicated HUD info (zone is spatial, server is process identity)
		FString CorrectServer = DedicatedServerId;
		FString CorrectZone = bPlayerInNegativeSide
			? TEXT("Zone-1 (West)")
			: TEXT("Zone-2 (East)");

		if (NyxChar->ServerName != CorrectServer || NyxChar->ZoneName != CorrectZone)
		{
			NyxChar->ServerName = CorrectServer;
			NyxChar->ZoneName = CorrectZone;
			UE_LOG(LogNyx, Log, TEXT("Zone update: %s now in %s on %s (X=%.0f, child=%s)"),
				*PC->GetName(), *CorrectZone, *CorrectServer, PlayerX,
				bIsChildConnection ? TEXT("yes") : TEXT("no"));
		}

		// ── Proxy players: check if they should migrate to another server ──
		if (bIsChildConnection)
		{
#if UE_WITH_REMOTE_OBJECT_HANDLE
			// Has the player crossed OUT of this server's zone?
			const bool bLeftOurZone =
				(bOwnsNegativeSide && PlayerX >= ZoneBoundaryX) ||
				(!bOwnsNegativeSide && PlayerX < ZoneBoundaryX);

			if (bLeftOurZone)
			{
				UE_LOG(LogNyx, Log, TEXT("Migration: %s crossed boundary at X=%.0f — transferring via DSTM"),
					*PC->GetName(), PlayerX);

				MigratePlayerDSTM(PC, NyxChar);
			}
#endif
			continue;
		}

		// ── Direct-connect players: ClientTravel transfer (non-proxy fallback) ──
		if (TransferAddress.IsEmpty()) continue;

		bool bShouldTransfer = false;
		if (bOwnsNegativeSide && PlayerX >= ZoneBoundaryX)
		{
			bShouldTransfer = true;
		}
		else if (!bOwnsNegativeSide && PlayerX < ZoneBoundaryX)
		{
			bShouldTransfer = true;
		}

		if (bShouldTransfer)
		{
			PlayersBeingTransferred.Add(PC);

			UE_LOG(LogNyx, Log, TEXT("Zone transfer: %s crossed boundary at X=%.0f → transferring to %s"),
				*PC->GetName(), PlayerX, *TransferAddress);

			UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
			if (ServerSub)
			{
				ServerSub->SaveCharacterState(NyxChar);
			}

			NyxChar->ClientRPC_TransferToServer(TransferAddress);
		}
	}
}

#if UE_WITH_REMOTE_OBJECT_HANDLE
void ANyxGameMode::MigratePlayerDSTM(APlayerController* PC, ANyxCharacter* NyxChar)
{
	UDSTMSubsystem* DSTMSub = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();
	if (!DSTMSub || !DSTMSub->IsMeshActive())
	{
		UE_LOG(LogNyx, Warning,
			TEXT("Migration DSTM: Mesh not active — cannot transfer %s. Is -DedicatedServerId set?"),
			*PC->GetName());
		return;
	}

	FRemoteServerId DestServerId;
	if (!DSTMSub->GetFirstPeerServerId(DestServerId))
	{
		UE_LOG(LogNyx, Warning,
			TEXT("Migration DSTM: No peer server connected — cannot transfer %s"),
			*PC->GetName());
		return;
	}

	UE_LOG(LogNyx, Log,
		TEXT("Migration DSTM: Transferring %s + pawn %s to server %u via DSTM"),
		*PC->GetName(), *NyxChar->GetName(), DestServerId.GetIdNumber());

	// Save character state to SpacetimeDB before migration (persistence)
	UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
	if (ServerSub)
	{
		ServerSub->SaveCharacterState(NyxChar);
	}

	PlayersBeingTransferred.Add(PC);

	// ── 1. Capture the pawn's exact world transform ──
	const FVector PawnLocation = NyxChar->GetActorLocation();
	const FRotator PawnRotation = NyxChar->GetActorRotation();

	// ── 2. Clean up SpacetimeDB tracking while we still have the character ──
	if (ServerSub)
	{
		ServerSub->OnPlayerLeft(NyxChar);
	}

	// ── 3. Keep possession intact — do NOT freeze ──
	// We transfer the pawn as-is (MOVE_Walking, collision on, current velocity).
	// Freezing via DisableMovement() would set MOVE_None which REPLICATES
	// to the client, causing the character to stop responding to input.
	// By keeping MOVE_Walking, the DSTM serializes the pawn in its natural
	// state — the client's CMC keeps predicting locally during the brief
	// transit gap, so the player sees zero interruption.

	UE_LOG(LogNyx, Log,
		TEXT("Migration DSTM: Pawn at (%.0f, %.0f, %.0f) — transferring pawn + PC via DSTM (no freeze, possession kept)"),
		PawnLocation.X, PawnLocation.Y, PawnLocation.Z);

	// ── 4. Stamp the pawn's position onto the PC's root component ──
	// The DSTM serializes the PC and its components — this carries the
	// exact position across to the receiving server as a fallback.
	if (USceneComponent* Root = PC->GetRootComponent())
	{
		Root->SetWorldLocationAndRotation(PawnLocation, PawnRotation);
	}

	UE_LOG(LogNyx, Log,
		TEXT("Migration DSTM: Stamped pawn position (%.0f, %.0f, %.0f) onto PC root component"),
		PawnLocation.X, PawnLocation.Y, PawnLocation.Z);

	// ── 5. Transfer the PAWN via DSTM (before the PC) ──
	// The pawn is a separate actor — transferring it preserves its RemoteObjectId.
	// The proxy tracks actors by RemoteObjectId, so the client sees the SAME
	// pawn on the new server — no destroy/recreate cycle, zero flicker.
	DSTMSub->TransferActorToServer(NyxChar, DestServerId);

	UE_LOG(LogNyx, Log,
		TEXT("Migration DSTM: Pawn %s transferred via DSTM — now transferring PC"),
		*NyxChar->GetName());

	// ── 6. Transfer the PC via DSTM ──
	// PostMigrate(Send) removes the PC from the world and swaps in a
	// NoPawnPlayerController. The pawn is already gone (transferred above).
	DSTMSub->TransferActorToServer(PC, DestServerId);

	UE_LOG(LogNyx, Log,
		TEXT("Migration DSTM: PC + Pawn transfer initiated — both sent via DSTM"));

	// PostMigrate(Send) has already run — the engine replaced the PC with
	// a NoPawnPlayerController for this connection. Clean up tracking.
	PlayersBeingTransferred.Remove(PC);
	TransferArrivalTimes.Remove(PC);
}

void ANyxGameMode::OnDSTMActorArrived(AActor* ArrivedActor)
{
	// Only interested in PlayerControllers (not pawns or other actors).
	// The pawn arrives first, then the PC — we act when the PC lands.
	APlayerController* PC = Cast<APlayerController>(ArrivedActor);
	if (!PC || PC->IsA(ANoPawnPlayerController::StaticClass()))
	{
		return;
	}

	UE_LOG(LogNyx, Log,
		TEXT("OnDSTMActorArrived: PC %s arrived via DSTM — triggering immediate arrival setup"),
		*PC->GetName());

	HandleMigratedPlayerArrival(PC);
}

void ANyxGameMode::HandleMigratedPlayerArrival(APlayerController* PC)
{
	UWorld* World = GetWorld();
	if (!World) return;

	// ── 1. Find the transferred pawn ──
	// The PC's Pawn reference may already be set from DSTM serialization
	// (the send side kept possession intact). Check that first.
	ANyxCharacter* NyxChar = Cast<ANyxCharacter>(PC->GetPawn());

	if (!NyxChar)
	{
		// Pawn reference didn't survive serialization — fall back to searching
		// for an unpossessed ANyxCharacter (the pawn transferred before the PC).
		for (TActorIterator<ANyxCharacter> It(World); It; ++It)
		{
			if (!It->GetController())
			{
				NyxChar = *It;
				break;
			}
		}
	}

	if (!NyxChar)
	{
		// Pawn hasn't arrived yet — it transfers before the PC but network
		// timing can vary. The delegate will fire again when the pawn arrives,
		// but since we only trigger on PC arrival, schedule a deferred retry.
		UE_LOG(LogNyx, Warning,
			TEXT("HandleMigratedPlayerArrival: %s — pawn not found yet, will retry via CheckZoneBoundaries"),
			*PC->GetName());
		return;
	}

	const FVector PawnPos = NyxChar->GetActorLocation();

	UE_LOG(LogNyx, Log,
		TEXT("HandleMigratedPlayerArrival: %s — found pawn %s at (%.0f, %.0f, %.0f) [from %s]"),
		*PC->GetName(), *NyxChar->GetName(), PawnPos.X, PawnPos.Y, PawnPos.Z,
		PC->GetPawn() == NyxChar ? TEXT("PC->Pawn ref") : TEXT("orphan search"));

	// ── 2. Fix up possession WITHOUT calling Possess()/ClientRestart ──
	// Calling PC->Possess(NyxChar) would trigger ClientRestart(), which sends
	// a replicated RPC to the client causing a visible "re-possession" flash
	// and animation reset. Instead, we fix up the bidirectional relationship
	// directly — the client never sees any change.

	// Pawn side: fix Controller, Owner, replication flags etc.
	// PossessedBy() does NOT call ClientRestart — it's the pawn-side bookkeeping.
	if (NyxChar->GetController() != PC)
	{
		NyxChar->PossessedBy(PC);
	}

	// PC side: set pawn reference if not already set from DSTM serialization
	if (PC->GetPawn() != NyxChar)
	{
		PC->SetPawn(NyxChar);
		PC->SetControlRotation(NyxChar->GetActorRotation());
	}

	// ── 3. Set server/zone info for HUD ──
	NyxChar->ServerName = DedicatedServerId;
	NyxChar->ZoneName = (PawnPos.X < ZoneBoundaryX)
		? TEXT("Zone-1 (West)")
		: TEXT("Zone-2 (East)");

	// Grant grace period to prevent immediate bounce-back
	TransferArrivalTimes.Add(PC, World->GetTimeSeconds());

	// Tell the character to skip position restore when SpacetimeDB stats arrive —
	// the pawn already has the correct position from the DSTM transfer.
	NyxChar->bSkipPositionRestore = true;

	// Register with SpacetimeDB for stats (HP, MP, level, etc.) but NOT position.
	UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
	if (ServerSub)
	{
		FString PlayerName = PC->PlayerState
			? PC->PlayerState->GetPlayerName()
			: PC->GetName();
		ServerSub->OnPlayerJoined(NyxChar, PlayerName);
	}

	UE_LOG(LogNyx, Log,
		TEXT("HandleMigratedPlayerArrival: %s — pawn %s ready (direct fix-up, no ClientRestart)"),
		*PC->GetName(), *NyxChar->GetName());
}

#endif // UE_WITH_REMOTE_OBJECT_HANDLE

void ANyxGameMode::OnAuthStateChanged(ENyxAuthState NewState)
{
	if (NewState == ENyxAuthState::FullyAuthenticated)
	{
		EnterWorld();
	}
}

bool ANyxGameMode::IsProxyServer() const
{
	const FString CmdLine(FCommandLine::Get());
	return CmdLine.Contains(TEXT("-ProxyRegistrationPort="));
}
