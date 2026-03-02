// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxGameInstance.h"
#include "Nyx/Nyx.h"
#include "Nyx/Online/NyxAuthSubsystem.h"
#include "Nyx/Core/NyxNetworkSubsystem.h"
#include "Nyx/Core/NyxGameMode.h"
#include "Nyx/World/NyxEntityManager.h"
#include "Nyx/Player/NyxPlayerPawn.h"
#include "Nyx/Player/NyxMovementComponent.h"
#include "Nyx/Sidecar/NyxSidecarSubsystem.h"
#include "ModuleBindings/SpacetimeDBClient.g.h"

void UNyxGameInstance::Init()
{
	Super::Init();
	UE_LOG(LogNyx, Log, TEXT("NyxGameInstance initialized"));
	UE_LOG(LogNyx, Log, TEXT("  SpacetimeDB Host: %s"), *SpacetimeDBHost);
	UE_LOG(LogNyx, Log, TEXT("  Database: %s"), *SpacetimeDBDatabaseName);

	// Bind login callback once
	UNyxAuthSubsystem* AuthSub = GetSubsystem<UNyxAuthSubsystem>();
	if (AuthSub)
	{
		AuthSub->OnLoginCompleteBP.AddDynamic(this, &UNyxGameInstance::OnLoginComplete);
	}

	RegisterConsoleCommands();
}

void UNyxGameInstance::Shutdown()
{
	UnregisterConsoleCommands();
	UE_LOG(LogNyx, Log, TEXT("NyxGameInstance shutting down"));
	Super::Shutdown();
}

void UNyxGameInstance::StartGame(bool bUseMock)
{
	UE_LOG(LogNyx, Log, TEXT("Starting game (mock=%s)"), bUseMock ? TEXT("true") : TEXT("false"));

	UNyxAuthSubsystem* AuthSub = GetSubsystem<UNyxAuthSubsystem>();
	if (!AuthSub)
	{
		UE_LOG(LogNyx, Error, TEXT("NyxAuthSubsystem not available!"));
		return;
	}

	// If already authenticated, logout first so we can re-login
	if (AuthSub->GetAuthState() != ENyxAuthState::NotAuthenticated)
	{
		UE_LOG(LogNyx, Log, TEXT("Logging out before re-starting game..."));
		AuthSub->Logout();
	}

	// Start the auth flow
	AuthSub->Login(EOSLoginType, SpacetimeDBHost, SpacetimeDBDatabaseName, bUseMock);
}

void UNyxGameInstance::OnLoginComplete(bool bSuccess, const FString& ErrorMessage)
{
	if (bSuccess)
	{
		UE_LOG(LogNyx, Log, TEXT("Login successful! Player is fully authenticated."));

		// At this point:
		// - EOS auth is done (or mocked)
		// - SpacetimeDB is connected
		// - The player's identity is linked
		//
		// Next steps (driven by game flow / level blueprint / game mode):
		// 1. NyxEntityManager->StartListening() to begin receiving entity events
		// 2. NyxNetworkSubsystem->UpdateSpatialSubscription() to subscribe to nearby data
		// 3. NyxNetworkSubsystem->GetDatabaseInterface()->CallCreatePlayer() to join the world
	}
	else
	{
		UE_LOG(LogNyx, Error, TEXT("Login failed: %s"), *ErrorMessage);
		// TODO: Show error UI, offer retry
	}
}

void UNyxGameInstance::RegisterConsoleCommands()
{
	// Nyx.Connect [Host] [Database] — connect directly to SpacetimeDB (bypasses auth)
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Connect"),
		TEXT("Connect directly to SpacetimeDB. Usage: Nyx.Connect [Host] [Database]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			const FString Host = Args.Num() > 0 ? Args[0] : SpacetimeDBHost;
			const FString DB = Args.Num() > 1 ? Args[1] : SpacetimeDBDatabaseName;

			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Connect %s %s"), *Host, *DB);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub)
			{
				NetworkSub->ConnectToServer(Host, DB, /*bUseMock=*/false);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("NyxNetworkSubsystem not available!"));
			}
		}),
		ECVF_Default));

	// Nyx.ConnectMock — connect with mock backend
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.ConnectMock"),
		TEXT("Connect with mock backend (no SpacetimeDB needed)"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.ConnectMock"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub)
			{
				NetworkSub->ConnectToServer(SpacetimeDBHost, SpacetimeDBDatabaseName, /*bUseMock=*/true);
			}
		}),
		ECVF_Default));

	// Nyx.Disconnect — disconnect from server
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Disconnect"),
		TEXT("Disconnect from SpacetimeDB"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Disconnect"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub)
			{
				NetworkSub->DisconnectFromServer();
			}
		}),
		ECVF_Default));

	// Nyx.StartGame [mock] — full auth flow
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.StartGame"),
		TEXT("Start full game flow (EOS + SpacetimeDB). Pass 'mock' for mock mode."),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			bool bMock = Args.Num() > 0 && Args[0].ToLower() == TEXT("mock");
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.StartGame (mock=%s)"), bMock ? TEXT("true") : TEXT("false"));
			StartGame(bMock);
		}),
		ECVF_Default));

	// Nyx.Seed <count> — seed world entities for spatial stress testing
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Seed"),
		TEXT("Seed world entities for testing. Usage: Nyx.Seed <count>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			uint32 Count = 100;
			if (Args.Num() > 0)
			{
				Count = FCString::Atoi(*Args[0]);
			}
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Seed %d"), Count);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->SeedEntities(Count);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.ClearEntities — remove all world entities
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.ClearEntities"),
		TEXT("Remove all world entities from the database"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.ClearEntities"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->ClearEntities();
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.Move <x> <y> <z> — teleport local player and update spatial subscription
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Move"),
		TEXT("Move player to position and update spatial subscription. Usage: Nyx.Move <x> <y> <z>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			if (Args.Num() < 3)
			{
				UE_LOG(LogNyx, Warning, TEXT("Usage: Nyx.Move <x> <y> <z>"));
				return;
			}

			const double X = FCString::Atod(*Args[0]);
			const double Y = FCString::Atod(*Args[1]);
			const double Z = FCString::Atod(*Args[2]);

			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Move %.0f %.0f %.0f"), X, Y, Z);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				// Update server position (seq=0 for debug teleport)
				NetworkSub->GetSpacetimeDBConnection()->Reducers->MovePlayer(X, Y, Z, 0.0f, 0);
				// Update spatial subscription for new position
				NetworkSub->UpdateSpatialSubscription(FVector(X, Y, Z));
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.Walk <dx> <dy> <dz> — relative movement for testing prediction
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Walk"),
		TEXT("Apply relative movement to local player pawn. Usage: Nyx.Walk <dx> <dy> <dz>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogNyx, Warning, TEXT("Usage: Nyx.Walk <dx> <dy> [dz]"));
				return;
			}

			const float DX = FCString::Atof(*Args[0]);
			const float DY = FCString::Atof(*Args[1]);
			const float DZ = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.f;

			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Walk %.0f %.0f %.0f"), DX, DY, DZ);

			// Find the local player pawn's movement component and add input
			UWorld* World = GetWorld();
			if (!World) return;

			UNyxEntityManager* EntityMgr = World->GetSubsystem<UNyxEntityManager>();
			if (EntityMgr && EntityMgr->GetLocalPlayerPawn())
			{
				if (UNyxMovementComponent* MoveComp = EntityMgr->GetLocalPlayerPawn()->GetNyxMovement())
				{
					FVector Dir(DX, DY, DZ);
					Dir.Normalize();
					MoveComp->AddMovementInput(Dir);
					UE_LOG(LogNyx, Log, TEXT("Walk input applied: (%.1f, %.1f, %.1f)"), Dir.X, Dir.Y, Dir.Z);
				}
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("No local player pawn available! Call Nyx.EnterWorld first."));
			}
		}),
		ECVF_Default));

	// Nyx.EnterWorld — create player and start entity listening (bypasses auth flow)
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.EnterWorld"),
		TEXT("Create player and start entity listening (use after Nyx.Connect)"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.EnterWorld"));

			UWorld* World = GetWorld();
			if (!World) return;

			ANyxGameMode* GM = Cast<ANyxGameMode>(World->GetAuthGameMode());
			if (GM)
			{
				GM->EnterWorld();
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("GameMode is not ANyxGameMode!"));
			}
		}),
		ECVF_Default));

	// ─── Spike 7: Benchmark Console Commands ──────────────────────────

	// Nyx.Bench <simple|medium|complex|burst|burstupdate> <count>
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Bench"),
		TEXT("Run benchmark. Usage: Nyx.Bench <simple|medium|complex|burst|burstupdate> <count>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			if (Args.Num() < 2)
			{
				UE_LOG(LogNyx, Warning, TEXT("Usage: Nyx.Bench <simple|medium|complex|burst|burstupdate> <count>"));
				return;
			}

			const FString Type = Args[0].ToLower();
			const int32 Count = FCString::Atoi(*Args[1]);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (!NetworkSub || !NetworkSub->GetSpacetimeDBConnection())
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
				return;
			}

			UDbConnection* Conn = NetworkSub->GetSpacetimeDBConnection();
			const double StartTime = FPlatformTime::Seconds();

			if (Type == TEXT("burst"))
			{
				// Single reducer call that does N operations internally
				UE_LOG(LogNyx, Log, TEXT("Bench BURST: %d ops in one reducer call"), Count);
				Conn->Reducers->BenchBurst(Count);
			}
			else if (Type == TEXT("burstupdate"))
			{
				// Single reducer call that updates N entity rows
				UE_LOG(LogNyx, Log, TEXT("Bench BURST UPDATE: %d entity updates in one reducer call"), Count);
				Conn->Reducers->BenchBurstUpdate(Count);
			}
			else if (Type == TEXT("simple"))
			{
				// Fire N individual reducer calls
				UE_LOG(LogNyx, Log, TEXT("Bench SIMPLE: firing %d individual reducer calls..."), Count);
				for (int32 i = 0; i < Count; ++i)
				{
					Conn->Reducers->BenchSimple();
				}
				const double Elapsed = FPlatformTime::Seconds() - StartTime;
				UE_LOG(LogNyx, Log, TEXT("Bench SIMPLE: %d calls dispatched in %.3f sec (%.0f calls/sec dispatch rate)"),
					Count, Elapsed, Count / FMath::Max(Elapsed, 0.001));
			}
			else if (Type == TEXT("medium"))
			{
				UE_LOG(LogNyx, Log, TEXT("Bench MEDIUM: firing %d individual reducer calls..."), Count);
				for (int32 i = 0; i < Count; ++i)
				{
					Conn->Reducers->BenchMedium();
				}
				const double Elapsed = FPlatformTime::Seconds() - StartTime;
				UE_LOG(LogNyx, Log, TEXT("Bench MEDIUM: %d calls dispatched in %.3f sec (%.0f calls/sec dispatch rate)"),
					Count, Elapsed, Count / FMath::Max(Elapsed, 0.001));
			}
			else if (Type == TEXT("complex"))
			{
				UE_LOG(LogNyx, Log, TEXT("Bench COMPLEX: firing %d individual reducer calls..."), Count);
				for (int32 i = 0; i < Count; ++i)
				{
					Conn->Reducers->BenchComplex();
				}
				const double Elapsed = FPlatformTime::Seconds() - StartTime;
				UE_LOG(LogNyx, Log, TEXT("Bench COMPLEX: %d calls dispatched in %.3f sec (%.0f calls/sec dispatch rate)"),
					Count, Elapsed, Count / FMath::Max(Elapsed, 0.001));
			}
			else
			{
				UE_LOG(LogNyx, Warning, TEXT("Unknown bench type '%s'. Use: simple, medium, complex, burst, burstupdate"), *Type);
			}
		}),
		ECVF_Default));

	// Nyx.BenchSeed <count> — seed bench entities
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.BenchSeed"),
		TEXT("Seed bench entities. Usage: Nyx.BenchSeed <count>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			const uint32 Count = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 100;
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.BenchSeed %d"), Count);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->BenchSeed(Count);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.BenchReset — clear all benchmark data
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.BenchReset"),
		TEXT("Clear all benchmark data"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.BenchReset"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->BenchReset();
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.BenchTick <interval_ms> — start scheduled game tick
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.BenchTick"),
		TEXT("Start scheduled game tick. Usage: Nyx.BenchTick <interval_ms>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			const uint64 IntervalMs = Args.Num() > 0 ? FCString::Atoi64(*Args[0]) : 100;
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.BenchTick %llu ms"), IntervalMs);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->BenchStartTick(IntervalMs);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.BenchTickStop — stop scheduled game tick
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.BenchTickStop"),
		TEXT("Stop the scheduled game tick"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.BenchTickStop"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->BenchStopTick();
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.BenchMem <megabytes> — test WASM memory limits
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.BenchMem"),
		TEXT("Test WASM memory limits. Usage: Nyx.BenchMem <megabytes>"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			const uint32 MB = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : 1;
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.BenchMem %d MB"), MB);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->BenchMemory(MB);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// ─── Spike 8: Sidecar / Physics Console Commands ─────────────────

	// Nyx.StartSidecar — start the physics sidecar (second SpacetimeDB connection)
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.StartSidecar"),
		TEXT("Start UE5 physics sidecar. Usage: Nyx.StartSidecar [Host] [Database]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			const FString Host = Args.Num() > 0 ? Args[0] : SpacetimeDBHost;
			const FString DB = Args.Num() > 1 ? Args[1] : SpacetimeDBDatabaseName;
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.StartSidecar %s %s"), *Host, *DB);

			UNyxSidecarSubsystem* Sidecar = GetSubsystem<UNyxSidecarSubsystem>();
			if (Sidecar)
			{
				Sidecar->StartSidecar(Host, DB);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("NyxSidecarSubsystem not available!"));
			}
		}),
		ECVF_Default));

	// Nyx.StopSidecar — stop the physics sidecar
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.StopSidecar"),
		TEXT("Stop the UE5 physics sidecar"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.StopSidecar"));

			UNyxSidecarSubsystem* Sidecar = GetSubsystem<UNyxSidecarSubsystem>();
			if (Sidecar)
			{
				Sidecar->StopSidecar();
			}
		}),
		ECVF_Default));

	// Nyx.Shoot <vx> <vy> <vz> — spawn a projectile at player's position (or origin)
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.Shoot"),
		TEXT("Spawn a physics projectile. Usage: Nyx.Shoot [vx] [vy] [vz]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([this](const TArray<FString>& Args)
		{
			// Default: shoot forward-up at 45 degrees
			double VelX = Args.Num() > 0 ? FCString::Atod(*Args[0]) : 500.0;
			double VelY = Args.Num() > 1 ? FCString::Atod(*Args[1]) : 0.0;
			double VelZ = Args.Num() > 2 ? FCString::Atod(*Args[2]) : 500.0;

			// Get spawn position from local player pawn, or use default
			double PosX = 0.0, PosY = 0.0, PosZ = 200.0;
			UWorld* World = GetWorld();
			if (World)
			{
				UNyxEntityManager* EntityMgr = World->GetSubsystem<UNyxEntityManager>();
				if (EntityMgr && EntityMgr->GetLocalPlayerPawn())
				{
					FVector Loc = EntityMgr->GetLocalPlayerPawn()->GetActorLocation();
					PosX = Loc.X;
					PosY = Loc.Y;
					PosZ = Loc.Z + 50.0; // Slightly above pawn
				}
			}

			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.Shoot pos=(%.0f, %.0f, %.0f) vel=(%.0f, %.0f, %.0f)"),
				PosX, PosY, PosZ, VelX, VelY, VelZ);

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->SpawnProjectile(
					PosX, PosY, PosZ, VelX, VelY, VelZ);
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	// Nyx.PhysicsReset — remove all physics bodies
	ConsoleCommands.Add(IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("Nyx.PhysicsReset"),
		TEXT("Remove all physics bodies from the database"),
		FConsoleCommandDelegate::CreateLambda([this]()
		{
			UE_LOG(LogNyx, Log, TEXT("Console: Nyx.PhysicsReset"));

			UNyxNetworkSubsystem* NetworkSub = GetSubsystem<UNyxNetworkSubsystem>();
			if (NetworkSub && NetworkSub->GetSpacetimeDBConnection())
			{
				NetworkSub->GetSpacetimeDBConnection()->Reducers->PhysicsReset();
			}
			else
			{
				UE_LOG(LogNyx, Error, TEXT("Not connected to SpacetimeDB!"));
			}
		}),
		ECVF_Default));

	UE_LOG(LogNyx, Log, TEXT("Registered console commands: Nyx.Connect, Nyx.ConnectMock, Nyx.Disconnect, Nyx.StartGame, Nyx.Seed, Nyx.ClearEntities, Nyx.Move, Nyx.Walk, Nyx.EnterWorld, Nyx.Bench, Nyx.BenchSeed, Nyx.BenchReset, Nyx.BenchTick, Nyx.BenchTickStop, Nyx.BenchMem, Nyx.StartSidecar, Nyx.StopSidecar, Nyx.Shoot, Nyx.PhysicsReset"));
}

void UNyxGameInstance::UnregisterConsoleCommands()
{
	for (IConsoleObject* Cmd : ConsoleCommands)
	{
		IConsoleManager::Get().UnregisterConsoleObject(Cmd);
	}
	ConsoleCommands.Empty();
}
