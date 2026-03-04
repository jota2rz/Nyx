// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxGameMode.h"
#include "Nyx/Nyx.h"
#include "Nyx/Core/NyxGameInstance.h"
#include "Nyx/Online/NyxAuthSubsystem.h"
#include "Nyx/Player/NyxCharacter.h"
#include "Nyx/Server/NyxServerSubsystem.h"
#include "Nyx/Networking/NyxMultiServerSubsystem.h"
#include "GameFramework/PlayerController.h"

ANyxGameMode::ANyxGameMode()
{
	// Default pawn is NyxCharacter — standard ACharacter with CMC
	DefaultPawnClass = ANyxCharacter::StaticClass();
}

void ANyxGameMode::StartPlay()
{
	Super::StartPlay();

	const bool bServer = IsNyxServer();
	UE_LOG(LogNyx, Log, TEXT("NyxGameMode::StartPlay (IsNyxServer=%s  NetMode=%d)"),
		bServer ? TEXT("true") : TEXT("false"), static_cast<int32>(GetNetMode()));

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
			if (FParse::Value(FCommandLine::Get(), TEXT("-ServerId="), CmdServerId, false))
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
	Super::PostLogin(NewPlayer);

	if (!NewPlayer) return;

	UE_LOG(LogNyx, Log, TEXT("PostLogin: %s"), *NewPlayer->GetName());

	if (IsNyxServer())
	{
		// Get the spawned NyxCharacter pawn
		ANyxCharacter* NyxChar = Cast<ANyxCharacter>(NewPlayer->GetPawn());
		if (NyxChar)
		{
			UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
			if (ServerSub)
			{
				// Use a display name — in production this would come from auth/EOS
				FString PlayerName = NewPlayer->GetName();
				ServerSub->OnPlayerJoined(NyxChar, PlayerName);
			}
		}
		else
		{
			UE_LOG(LogNyx, Warning, TEXT("PostLogin: Player pawn is not ANyxCharacter"));
		}
	}
}

void ANyxGameMode::Logout(AController* Exiting)
{
	if (IsNyxServer() && Exiting)
	{
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

void ANyxGameMode::OnAuthStateChanged(ENyxAuthState NewState)
{
	if (NewState == ENyxAuthState::FullyAuthenticated)
	{
		EnterWorld();
	}
}
