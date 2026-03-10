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
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/ChildConnection.h"
#include "GameFramework/CharacterMovementComponent.h"
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

			// ── MultiServer mesh: if cmd-line specifies peers, join the mesh ──
			UNyxMultiServerSubsystem* MultiSub = GetGameInstance()->GetSubsystem<UNyxMultiServerSubsystem>();
			if (MultiSub && MultiSub->InitializeFromCommandLine())
			{
				UE_LOG(LogNyx, Log, TEXT("MultiServer mesh initialized from command line"));
			}

			// ── DSTM mesh: initialize the DSTM beacon transport for seamless migration ──
			// DSTMSubsystem cannot auto-init in Initialize() because GetWorld() is
			// null during GameInstance subsystem creation. Init here when World is ready.
			UDSTMSubsystem* DSTMSub = GetGameInstance()->GetSubsystem<UDSTMSubsystem>();
			if (DSTMSub && DSTMSub->InitializeFromCommandLine())
			{
				UE_LOG(LogNyx, Log, TEXT("DSTM mesh initialized from command line"));
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

void ANyxGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (IsNyxServer())
	{
		GetWorld()->GetTimerManager().ClearTimer(ZoneCheckTimerHandle);

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
		// Detect a migrated PC that needs setup on this server.
		// Two cases:
		//   A) PC has no pawn — pawn ref didn't survive DSTM serialization
		//   B) PC has a pawn but it's still frozen — pawn ref DID survive but
		//      HandleMigratedPlayerArrival hasn't run yet to re-enable movement
		const bool bNeedsArrivalSetup =
			(PC->Player && !PC->IsA(ANoPawnPlayerController::StaticClass())) &&
			(!NyxChar || !NyxChar->GetActorEnableCollision());

		if (bNeedsArrivalSetup)
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

	// ── 3. Freeze the pawn (but keep it possessed) ──
	// Freeze movement and disable collision so the pawn is inert during transit.
	// We do NOT call UnPossess — keeping the PC→Pawn relationship intact means
	// the DSTM serialization carries the Pawn reference across. The receiving
	// server's PC already has GetPawn() == NyxChar, so no re-possession (and
	// crucially no ClientRestart RPC) is needed. The transfer is invisible
	// to the client — zero flicker, zero animation reset.
	if (UCharacterMovementComponent* MoveComp = NyxChar->GetCharacterMovement())
	{
		MoveComp->StopMovementImmediately();
		MoveComp->DisableMovement();
	}
	NyxChar->SetActorEnableCollision(false);

	UE_LOG(LogNyx, Log,
		TEXT("Migration DSTM: Froze pawn at (%.0f, %.0f, %.0f) — transferring pawn + PC via DSTM (possession kept)"),
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
		// Pawn hasn't arrived yet (still in transit) — CheckZoneBoundaries
		// will call us again next tick. This is normal for the first 1-2 ticks.
		UE_LOG(LogNyx, Log,
			TEXT("HandleMigratedPlayerArrival: %s — transferred pawn not yet arrived, will retry next tick"),
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

	// ── 3. Re-enable movement and collision (frozen on sending server) ──
	if (UCharacterMovementComponent* MoveComp = NyxChar->GetCharacterMovement())
	{
		MoveComp->SetMovementMode(MOVE_Walking);
	}
	NyxChar->SetActorEnableCollision(true);

	// ── 4. Set server/zone info for HUD ──
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
	// The proxy process is launched with -ProxyGameServers= which tells
	// UProxyNetDriver which backend game servers to connect to.
	// If this flag is present, we're a proxy — not a real game server.
	return FParse::Param(FCommandLine::Get(), TEXT("ProxyGameServers"))
		|| FString(FCommandLine::Get()).Contains(TEXT("-ProxyGameServers="));
}
