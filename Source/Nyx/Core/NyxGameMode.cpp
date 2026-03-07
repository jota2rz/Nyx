// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxGameMode.h"
#include "Nyx/Nyx.h"
#include "Nyx/Core/NyxGameInstance.h"
#include "Nyx/Online/NyxAuthSubsystem.h"
#include "Nyx/Player/NyxCharacter.h"
#include "Nyx/Server/NyxServerSubsystem.h"
#include "Nyx/Networking/NyxMultiServerSubsystem.h"
#include "Nyx/World/NyxZoneBoundary.h"
#include "Nyx/UI/NyxHUD.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Engine/ChildConnection.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/PackageMapClient.h"
#include "Misc/NetworkGuid.h"
#include "Misc/CommandLine.h"

ANyxGameMode::ANyxGameMode()
{
	// Default pawn is NyxCharacter — standard ACharacter with CMC
	DefaultPawnClass = ANyxCharacter::StaticClass();
	HUDClass = ANyxHUD::StaticClass();
}

void ANyxGameMode::StartPlay()
{
	Super::StartPlay();

	// ── NetworkGuidSeed: prevent GUID collisions in multi-server setups ──
	// Each game server needs a unique seed so their GUID counters don't overlap.
	// The engine's -NetworkGuidSeed= parameter is stripped in Shipping builds,
	// so we read our own -NyxGuidSeed= and replace the GuidCache with a seeded one.
	{
		uint64 GuidSeed = 0;
		FParse::Value(FCommandLine::Get(), TEXT("-NyxGuidSeed="), GuidSeed);
		if (GuidSeed > 0)
		{
			UNetDriver* NetDriver = GetWorld()->GetNetDriver();
			if (NetDriver)
			{
				PRAGMA_DISABLE_DEPRECATION_WARNINGS
				NetDriver->GuidCache = TSharedPtr<FNetGUIDCache>(new FNetGUIDCache(NetDriver, GuidSeed));
				PRAGMA_ENABLE_DEPRECATION_WARNINGS
				UE_LOG(LogNyx, Log, TEXT("NyxGameMode: Replaced GuidCache with seed %llu (production-safe GUID partitioning)"), GuidSeed);
			}
			else
			{
				UE_LOG(LogNyx, Warning, TEXT("NyxGameMode: -NyxGuidSeed=%llu specified but NetDriver is null (not a server?)"), GuidSeed);
			}
		}
	}

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

			// Bind to external character save events for migration detection.
			// When another server saves a character (during ReleasePawnAuthority),
			// we check if the saved position is in our zone and trigger a claim.
			ServerSub->OnExternalCharacterSaved.AddDynamic(this, &ANyxGameMode::HandleExternalCharacterSaved);

			// ── MultiServer mesh: if cmd-line specifies peers, join the mesh ──
			UNyxMultiServerSubsystem* MultiSub = GetGameInstance()->GetSubsystem<UNyxMultiServerSubsystem>();
			if (MultiSub && MultiSub->InitializeFromCommandLine())
			{
				UE_LOG(LogNyx, Log, TEXT("MultiServer mesh initialized from command line"));
			}

			// ── Zone Transfer config (Spike 19) ──
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

			// NOTE: Deterministic GUID forcing is disabled. The proxy assigns different
			// HandshakeIds per backend server (e.g. 123 for Server-1, 124 for Server-2),
			// so MakeMigrationGUID produces different GUIDs on each server. Combined with
			// EChannelCloseReason::Migrated (which keeps old proxy-side actors alive),
			// this creates duplicate actors on the client. A stable cross-server client
			// identifier is needed to make this approach work.
			// See: MakeMigrationGUID / AssignMigrationGUID (kept for future use).

			// Skip SpacetimeDB registration for migration claims — ClaimPawnAuthority
			// handles it separately. OnPlayerJoined restores position from the DB cache,
			// which would override the NoPawnPC's authoritative position.
			if (!MigrationClaimPCs.Contains(NewPlayer))
			{
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
				// in the OTHER server's zone. If we keep it, the pawn immediately
				// triggers a release → claim cascade (triple pawn on client).
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
				}			}
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
			NoPawnTracking.Remove(PC);
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

void ANyxGameMode::HandleExternalCharacterSaved(const FCharacterStatsType& Stats)
{
	// Another server saved a character — check if the saved position is in OUR zone.
	// This happens when the other server releases a player during migration
	// (ReleasePawnAuthority calls SaveCharacterState before swapping to NoPawnPC).
	const double SavedX = Stats.SavedPosX;
	const bool bSavedInOurZone = bOwnsNegativeSide
		? (SavedX < ZoneBoundaryX)
		: (SavedX >= ZoneBoundaryX);

	if (bSavedInOurZone)
	{
		bMigrationClaimPending = true;
		MigrationClaimPosition = FVector(Stats.SavedPosX, Stats.SavedPosY, Stats.SavedPosZ);
		UE_LOG(LogNyx, Log, TEXT("Migration signal received: %s saved at (%.0f, %.0f, %.0f) — IN our %s zone. Claim pending."),
			*Stats.DisplayName, Stats.SavedPosX, Stats.SavedPosY, Stats.SavedPosZ,
			bOwnsNegativeSide ? TEXT("west") : TEXT("east"));
	}
	else
	{
		UE_LOG(LogNyx, Log, TEXT("Migration signal ignored: %s saved at (%.0f, %.0f, %.0f) — outside our %s zone"),
			*Stats.DisplayName, Stats.SavedPosX, Stats.SavedPosY, Stats.SavedPosZ,
			bOwnsNegativeSide ? TEXT("west") : TEXT("east"));
	}
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

		// ── NoPawnPlayerController: this server is non-primary for this client. ──
		// The proxy places NoPawnPCs at the player's world position via
		// ServerSetViewTargetPosition(). If that position transitions INTO our zone,
		// we should claim authority: spawn a pawn and swap to a real PC.
		// This is Server B's side of the migration handshake.
		//
		// We use position-transition detection to distinguish legitimate migration
		// from initial NoPawnPCs that happen to start in our zone:
		//
		//   Normal migration: NoPawnPC position was outside our zone, then moves
		//   inside as the player crosses the boundary → claim after 3s grace.
		//
		//   Spawn-in-wrong-zone: NoPawnPC starts inside our zone because the player
		//   spawned in our territory (but primary is the other server). The primary
		//   server will release after its 5s grace → we claim after 8s grace
		//   (long enough for the other server to release first).
		if (PC->IsA(ANoPawnPlayerController::StaticClass()) && bIsChildConnection)
		{
			// NoPawnPC stores the player's world position via SetActorLocation()
			// (called from ServerSetViewTargetPosition). Access it through the
			// view target (returns AActor* where GetActorLocation is public,
			// unlike AController which hides it via HIDE_ACTOR_TRANSFORM_FUNCTIONS).
			AActor* ViewTarget = PC->GetViewTarget();
			const FVector NoPawnPos = ViewTarget ? ViewTarget->GetActorLocation() : FVector::ZeroVector;

			// Is the NoPawnPC's tracked position inside our zone?
			const bool bInOurZone = bOwnsNegativeSide
				? (NoPawnPos.X < ZoneBoundaryX)
				: (NoPawnPos.X >= ZoneBoundaryX);

			// Update tracking state
			FNoPawnMigrationTracking& Track = NoPawnTracking.FindOrAdd(PC);
			if (Track.FirstSeenTime == 0.0)
			{
				Track.FirstSeenTime = CurrentTime;
				Track.InitialPosition = NoPawnPos;
				UE_LOG(LogNyx, Log, TEXT("NoPawnPC %s first seen at X=%.1f (%s our zone %s)"),
					*PC->GetName(), NoPawnPos.X,
					bInOurZone ? TEXT("IN") : TEXT("outside"),
					bOwnsNegativeSide ? TEXT("west") : TEXT("east"));
			}

			// Track if the position has moved significantly from initial value.
			// Proxy-setup NoPawnPCs sit at (0,0,0) or the boundary until the proxy
			// starts forwarding real player positions. A significant move means the
			// proxy is actively syncing — this NoPawnPC represents a real player.
			if (!Track.bPositionHasMoved)
			{
				const float PosDelta = FVector::Dist(NoPawnPos, Track.InitialPosition);
				if (PosDelta > 100.0f) // 1 meter threshold
				{
					Track.bPositionHasMoved = true;
					UE_LOG(LogNyx, Log, TEXT("NoPawnPC %s position moved %.0f units from initial (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)"),
						*PC->GetName(), PosDelta,
						Track.InitialPosition.X, Track.InitialPosition.Y, Track.InitialPosition.Z,
						NoPawnPos.X, NoPawnPos.Y, NoPawnPos.Z);
				}
			}

			if (bInOurZone)
			{
				// ── Released by us: block claims until position exits our zone ──
				if (Track.bReleasedByUs)
				{
					continue;
				}

				// Record when the NoPawnPC entered our zone (if not already in)
				if (Track.EnteredOurZoneTime == 0.0)
				{
					Track.EnteredOurZoneTime = CurrentTime;
				}

				const double TimeInZone = CurrentTime - Track.EnteredOurZoneTime;
				bool bShouldClaim = false;
				FVector ClaimPosition = NoPawnPos;

				if (Track.bWasEverOutsideOurZone && TimeInZone >= NoPawnClaimGracePeriodSeconds)
				{
					UE_LOG(LogNyx, Log, TEXT("Migration CLAIM (transition): NoPawnPC %s at X=%.0f — was outside, now in %s zone for %.1fs"),
						*PC->GetName(), NoPawnPos.X,
						bOwnsNegativeSide ? TEXT("west") : TEXT("east"), TimeInZone);
					bShouldClaim = true;
				}
				else if (!Track.bWasEverOutsideOurZone && Track.bPositionHasMoved && TimeInZone >= SettledNoPawnClaimGracePeriodSeconds)
				{
					UE_LOG(LogNyx, Log, TEXT("Migration CLAIM (settled): NoPawnPC %s at X=%.0f — position moved, in %s zone for %.1fs"),
						*PC->GetName(), NoPawnPos.X,
						bOwnsNegativeSide ? TEXT("west") : TEXT("east"), TimeInZone);
					bShouldClaim = true;
				}

				if (bShouldClaim)
				{
					ClaimPawnAuthority(PC, ClaimPosition, FRotator::ZeroRotator);
				}
			}
			else
			{
				// NoPawnPC is outside our zone — mark and reset entry timer
				Track.bWasEverOutsideOurZone = true;
				Track.EnteredOurZoneTime = 0.0;

				// Clear release flag after enough time
				if (Track.bReleasedByUs)
				{
					const double TimeSinceRelease = CurrentTime - Track.ReleasedByUsTime;
					if (TimeSinceRelease > 8.0)
					{
						Track.bReleasedByUs = false;
						Track.ReleasedByUsTime = 0.0;
						UE_LOG(LogNyx, Log, TEXT("NoPawnPC %s release flag cleared — position outside our zone after %.1fs since release"),
							*PC->GetName(), TimeSinceRelease);
					}
				}

				// ── SpacetimeDB migration claim ──
				// The proxy cannot reliably signal us when the other server releases.
				// Instead, we use SpacetimeDB: when the other server releases, it
				// calls SaveCharacterState() which updates character_stats in the DB.
				// Both servers subscribe to character_stats. Our HandleExternalCharacterSaved()
				// fires, checks if the saved position is in our zone, and sets
				// bMigrationClaimPending = true.
				//
				// Here we consume that signal: find any NoPawnPC that represents a
				// real player (bPositionHasMoved) and isn't ours (bReleasedByUs),
				// then claim it at the saved position.
				if (bMigrationClaimPending && !Track.bReleasedByUs && Track.bPositionHasMoved)
				{
					// Use the SpacetimeDB-provided crossing position
					FVector ClaimPos = MigrationClaimPosition;

					// Safety: ensure the claim position is in our zone
					const bool bClaimInOurZone = bOwnsNegativeSide
						? (ClaimPos.X < ZoneBoundaryX)
						: (ClaimPos.X >= ZoneBoundaryX);
					if (!bClaimInOurZone)
					{
						ClaimPos.X = bOwnsNegativeSide
							? (ZoneBoundaryX - 100.f)
							: (ZoneBoundaryX + 100.f);
					}

					UE_LOG(LogNyx, Log, TEXT("Migration CLAIM (spacetimedb): NoPawnPC %s — SpacetimeDB signaled migration. Stale pos X=%.0f, claiming at (%.0f, %.0f, %.0f) in %s zone"),
						*PC->GetName(),
						NoPawnPos.X,
						ClaimPos.X, ClaimPos.Y, ClaimPos.Z,
						bOwnsNegativeSide ? TEXT("west") : TEXT("east"));

					bMigrationClaimPending = false;
					ClaimPawnAuthority(PC, ClaimPos, FRotator::ZeroRotator);
				}
			}
			continue;
		}

		// ── Regular PlayerController with a pawn ──
		ANyxCharacter* NyxChar = Cast<ANyxCharacter>(PC->GetPawn());
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
			// Has the player crossed OUT of this server's zone?
			const bool bLeftOurZone =
				(bOwnsNegativeSide && PlayerX >= ZoneBoundaryX) ||
				(!bOwnsNegativeSide && PlayerX < ZoneBoundaryX);

			if (bLeftOurZone)
			{
				UE_LOG(LogNyx, Log, TEXT("Migration RELEASE: %s crossed boundary at X=%.0f — releasing authority"),
					*PC->GetName(), PlayerX);
				ReleasePawnAuthority(PC, NyxChar);
			}
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

void ANyxGameMode::ReleasePawnAuthority(APlayerController* PC, ANyxCharacter* NyxChar)
{
	// ── Server A side of proxy migration ──
	// The player has crossed into another server's zone. We release authority:
	//   1. Save pawn state to SpacetimeDB
	//   2. Unpossess and destroy the pawn
	//   3. Swap the real PC → ANoPawnPlayerController
	// The proxy detects the PC change and begins finalizing the migration.

	PlayersBeingTransferred.Add(PC);

	// Save character state before destroying pawn
	UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
	if (ServerSub)
	{
		ServerSub->SaveCharacterState(NyxChar);
		ServerSub->OnPlayerLeft(NyxChar);
	}

	const FVector LastPos = NyxChar->GetActorLocation();
	const FRotator LastRot = NyxChar->GetActorRotation();

	UE_LOG(LogNyx, Log, TEXT("Migration RELEASE: Destroying pawn at (%.0f, %.0f, %.0f) and swapping to NoPawnPC"),
		LastPos.X, LastPos.Y, LastPos.Z);

	// NOTE: Migrated close reason is disabled. The proxy assigns different
	// HandshakeIds per backend, so deterministic GUIDs don't match between servers.
	// Migrated + mismatched GUIDs = duplicate actors on client. Instead, we let
	// DestroyActor close channels with the default Destroyed reason. The proxy
	// destroys old actors, Server-B creates new ones, and client-side fallbacks
	// (OnRep_Controller re-bind) handle the transition.

	// Unpossess and destroy the pawn
	PC->UnPossess();
	NyxChar->Destroy();

	// Spawn a NoPawnPlayerController at the player's last position.
	// The proxy uses this position for relevancy on non-primary servers.
	FActorSpawnParameters SpawnParams;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ANoPawnPlayerController* NoPawnPC = GetWorld()->SpawnActor<ANoPawnPlayerController>(
		ANoPawnPlayerController::StaticClass(), LastPos, LastRot, SpawnParams);

	if (!NoPawnPC)
	{
		UE_LOG(LogNyx, Error, TEXT("Migration RELEASE: Failed to spawn NoPawnPlayerController!"));
		PlayersBeingTransferred.Remove(PC);
		return;
	}

	// SwapPlayerControllers transfers the Player/NetConnection and triggers
	// replication updates that the proxy detects as a PC reassignment.
	// However, it does NOT transfer ClientHandshakeId — the proxy uses this
	// ID to find the internal route for this client, so we must transfer it.
	const uint32 HandshakeId = PC->GetClientHandshakeId();

	SwapPlayerControllers(PC, NoPawnPC);

	// Critical: Transfer the handshake ID so the proxy can match this
	// NoPawnPC to the correct client route. Without this, the NoPawnPC
	// arrives with handshake ID 0 and the proxy route lookup fails.
	NoPawnPC->SetClientHandshakeId(HandshakeId);

	UE_LOG(LogNyx, Log, TEXT("Migration RELEASE: Swapped %s → %s (HandshakeId=%u). Proxy will detect NoPawnPC on this route."),
		*PC->GetName(), *NoPawnPC->GetName(), HandshakeId);

	// Pre-populate tracking for the new NoPawnPC with bReleasedByUs = true.
	// This prevents us from re-claiming our own NoPawnPC when the proxy
	// briefly bounces its position back into our zone after the route change.
	{
		FNoPawnMigrationTracking Track;
		Track.bReleasedByUs = true;
		Track.ReleasedByUsTime = GetWorld()->GetTimeSeconds();
		Track.FirstSeenTime = Track.ReleasedByUsTime;
		Track.InitialPosition = LastPos;
		// Starting position is outside our zone (player just crossed boundary)
		Track.bWasEverOutsideOurZone = true;
		NoPawnTracking.Add(NoPawnPC, Track);
	}

	// Remove tracking for the old PC (the NoPawnPC will be cleaned up when
	// the other server claims authority, or when the player disconnects)
	PlayersBeingTransferred.Remove(PC);
	TransferArrivalTimes.Remove(PC);
}

void ANyxGameMode::ClaimPawnAuthority(APlayerController* NoPawnPC, const FVector& SpawnLocation, const FRotator& SpawnRotation)
{
	// ── Server B side of proxy migration ──
	// A NoPawnPlayerController on a child connection has its tracked position
	// inside our zone. We claim authority:
	//   1. Spawn a pawn at the NoPawnPC's position
	//   2. Spawn a new real PlayerController
	//   3. Use SwapPlayerControllers (NoPawnPC → NewPC) so the proxy can
	//      properly remap NetGUIDs and the client keeps its existing PC
	//   4. Possess the pawn
	// The proxy detects the PC change and finalizes the migration.

	// Find the NoPawnPC's net connection. ANoPawnPlayerController overrides
	// GetNetConnection() to return NetConnection directly (bypassing the
	// Player!=nullptr check in APlayerController::GetNetConnection).
	UNetConnection* NetConn = NoPawnPC->GetNetConnection();
	if (!NetConn)
	{
		UE_LOG(LogNyx, Error, TEXT("Migration CLAIM: NoPawnPC %s has no NetConnection — cannot transfer"), *NoPawnPC->GetName());
		return;
	}

	UE_LOG(LogNyx, Log, TEXT("Migration CLAIM: Spawning pawn at (%.0f, %.0f, %.0f) and promoting NoPawnPC to real PC"),
		SpawnLocation.X, SpawnLocation.Y, SpawnLocation.Z);

	// Spawn the pawn at the crossing position
	FActorSpawnParameters PawnSpawnParams;
	PawnSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	ANyxCharacter* NewChar = GetWorld()->SpawnActor<ANyxCharacter>(
		ANyxCharacter::StaticClass(), SpawnLocation, SpawnRotation, PawnSpawnParams);

	if (!NewChar)
	{
		UE_LOG(LogNyx, Error, TEXT("Migration CLAIM: Failed to spawn NyxCharacter!"));
		return;
	}

	// Spawn a new real PlayerController
	FActorSpawnParameters PCSpawnParams;
	PCSpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	APlayerController* NewPC = GetWorld()->SpawnActor<APlayerController>(
		PlayerControllerClass ? PlayerControllerClass.Get() : APlayerController::StaticClass(),
		FVector::ZeroVector, FRotator::ZeroRotator, PCSpawnParams);

	if (!NewPC)
	{
		UE_LOG(LogNyx, Error, TEXT("Migration CLAIM: Failed to spawn PlayerController!"));
		NewChar->Destroy();
		return;
	}

	// NOTE: Deterministic GUID forcing disabled — HandshakeId differs per backend.
	// See PostLogin comment for details.

	// ── SwapPlayerControllers (NoPawnPC → NewPC) ──
	// ANoPawnPlayerController has Player==nullptr (engine doesn't set it for
	// NoPawn connections). SwapPlayerControllers requires OldPC->Player != nullptr.
	// We temporarily set NoPawnPC->Player = NetConn so the swap succeeds.
	//
	// Using SwapPlayerControllers (instead of manual transfer) is critical:
	// it sets OldPC->PendingSwapConnection, which the proxy uses to remap
	// NetGUIDs. Without this, the new PC arrives on the client as a separate
	// actor instead of being mapped to the client's existing PC.

	// Critical: set ClientHandshakeId BEFORE the swap — the proxy uses this
	// during GameServerAssignPlayerController to find the internal route.
	NewPC->SetClientHandshakeId(NetConn->GetClientHandshakeId());

	// Temporarily give the NoPawnPC a Player so SwapPlayerControllers works.
	// This is safe because ANoPawnPlayerController::GetNetConnection() already
	// returns this connection — we're just making the base class field match.
	NoPawnPC->Player = NetConn;

	// Standard engine swap: transfers Player, NetConnection, NetPlayerIndex,
	// calls SetPlayer → HandleClientPlayer → proxy route update.
	// Also sets NoPawnPC->PendingSwapConnection for proxy NetGUID remapping.
	SwapPlayerControllers(NoPawnPC, NewPC);

	UE_LOG(LogNyx, Log, TEXT("Migration CLAIM: Swapped %s → %s (HandshakeId=%u)"),
		*NoPawnPC->GetName(), *NewPC->GetName(), NewPC->GetClientHandshakeId());

	// Initialize the PlayerState (SpawnActor doesn't call this automatically)
	NewPC->InitPlayerState();

	// Possess the pawn — sets up input component & bindings
	NewPC->Possess(NewChar);

	// Mark this PC as a migration claim so PostLogin skips SpacetimeDB registration.
	// OnPlayerJoined restores position from the DB cache, which would override the
	// NoPawnPC's authoritative position (the whole point of migration).
	MigrationClaimPCs.Add(NewPC);

	// PostLogin finishes engine initialization (enhanced input, HUD, etc.)
	PostLogin(NewPC);

	// Clear migration flag now that PostLogin is done
	MigrationClaimPCs.Remove(NewPC);

	// Override any position changes from PostLogin — the NoPawnPC position
	// is authoritative for migration claims, not the SpacetimeDB cache.
	NewChar->SetActorLocation(SpawnLocation);
	NewChar->SetActorRotation(SpawnRotation);

	// Set replicated HUD info
	NewChar->ServerName = DedicatedServerId;
	NewChar->ZoneName = (SpawnLocation.X < ZoneBoundaryX)
		? TEXT("Zone-1 (West)")
		: TEXT("Zone-2 (East)");

	// Grace period: prevent immediate bounce-back
	TransferArrivalTimes.Add(NewPC, GetWorld()->GetTimeSeconds());

	// Clean up tracking for the old NoPawnPC
	NoPawnTracking.Remove(NoPawnPC);
	PlayersBeingTransferred.Remove(NoPawnPC);

	// Destroy the old NoPawnPC — PendingSwapConnection was set by
	// SwapPlayerControllers, so the proxy has already recorded the swap.
	NoPawnPC->Destroy();

	UE_LOG(LogNyx, Log, TEXT("Migration CLAIM: %s now primary on %s at %s. Pawn=%s"),
		*NewPC->GetName(), *DedicatedServerId, *NewChar->ZoneName, *NewChar->GetName());
}

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

// ─── Seamless Migration: Deterministic GUID Helpers ────────────────

FNetworkGUID ANyxGameMode::MakeMigrationGUID(uint32 HandshakeId, uint8 Slot)
{
	// Compute a deterministic GUID that both Server-A and Server-B can derive
	// independently from the stable ClientHandshakeId. This avoids any inter-server
	// communication for GUID transfer.
	//
	// The NetIndex is placed in a high uint64 range (0x200000000000+) that can
	// never collide with sequential GUIDs assigned from seeds (100000, 200000, etc.).
	//
	// Layout of NetIndex:
	//   Bits 47-44: Slot (0=PC, 1=Pawn, 2-15 reserved for component GUIDs)
	//   Bits 43-0:  HandshakeId (supports up to ~17 trillion unique clients)
	//   Bit 48:     Always 1 (sentinel — separates from sequential range)
	//
	// CreateFromIndex(NetIndex, false) produces: ObjectId = NetIndex << 1
	// → even number → IsDynamic() == true (required for spawned actors)
	const uint64 NetIndex = 0x200000000000ULL
		| (static_cast<uint64>(Slot) << 40)
		| static_cast<uint64>(HandshakeId);
	return FNetworkGUID::CreateFromIndex(NetIndex, false);
}

void ANyxGameMode::AssignMigrationGUID(AActor* Actor, uint32 HandshakeId, uint8 Slot)
{
	if (!Actor) return;

	UNetDriver* NetDriver = GetWorld()->GetNetDriver();
	if (!NetDriver) return;

	TSharedPtr<FNetGUIDCache>& Cache = NetDriver->GetNetGuidCache();
	if (!Cache.IsValid()) return;

	const FNetworkGUID DesiredGUID = MakeMigrationGUID(HandshakeId, Slot);
	const FNetworkGUID CurrentGUID = Cache->GetNetGUID(Actor);

	if (CurrentGUID.IsValid())
	{
		if (CurrentGUID == DesiredGUID)
		{
			// Already assigned the correct migration GUID
			return;
		}
		// Remove auto-assigned GUID so RegisterNetGUID_Server won't hit check()
		Cache->RemoveNetGUID(Actor);
	}

	Cache->RegisterNetGUID_Server(DesiredGUID, Actor);

	UE_LOG(LogNyx, Log, TEXT("Migration GUID: Assigned %s to %s (HandshakeId=%u, Slot=%u)"),
		*DesiredGUID.ToString(), *Actor->GetName(), HandshakeId, Slot);
}
