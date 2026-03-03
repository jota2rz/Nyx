// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxSmokeTestCommandlet.h"
#include "NyxSmokeTest.h"
#include "Containers/Ticker.h"
#include "Containers/BackgroundableTicker.h"

DEFINE_LOG_CATEGORY_STATIC(LogNyxSmokeTestCmd, Log, All);

static const double CommandletTimeoutSeconds = 45.0;
static const double TickIntervalSeconds = 0.033; // ~30 fps tick rate

UNyxSmokeTestCommandlet::UNyxSmokeTestCommandlet()
{
	IsClient = false;
	IsEditor = false;
	IsServer = false;
	LogToConsole = true;
}

int32 UNyxSmokeTestCommandlet::Main(const FString& Params)
{
	// Parse command-line args
	FString Host = TEXT("127.0.0.1:3000");
	FString DB = TEXT("nyx");

	TArray<FString> Tokens;
	TArray<FString> Switches;
	TMap<FString, FString> ParamMap;
	UCommandlet::ParseCommandLine(*Params, Tokens, Switches, ParamMap);

	if (const FString* HostParam = ParamMap.Find(TEXT("host")))
	{
		Host = *HostParam;
	}
	if (const FString* DbParam = ParamMap.Find(TEXT("db")))
	{
		DB = *DbParam;
	}

	UE_LOG(LogNyxSmokeTestCmd, Log, TEXT(""));
	UE_LOG(LogNyxSmokeTestCmd, Log, TEXT("NyxSmokeTestCommandlet starting..."));
	UE_LOG(LogNyxSmokeTestCmd, Log, TEXT("  Host: %s"), *Host);
	UE_LOG(LogNyxSmokeTestCmd, Log, TEXT("  Database: %s"), *DB);
	UE_LOG(LogNyxSmokeTestCmd, Log, TEXT(""));

	// Create and run the test
	UNyxSmokeTest* Test = NewObject<UNyxSmokeTest>(GetTransientPackage());
	Test->AddToRoot(); // prevent GC

	Test->Run(Host, DB);

	// Pump the engine ticker until test completes or timeout
	const double StartTime = FPlatformTime::Seconds();

	while (Test->IsRunning())
	{
		const double Elapsed = FPlatformTime::Seconds() - StartTime;
		if (Elapsed > CommandletTimeoutSeconds)
		{
			UE_LOG(LogNyxSmokeTestCmd, Error, TEXT("Commandlet timeout after %.0fs — test is stuck"), Elapsed);
			break;
		}

		// Pump the backgroundable ticker (drives FLwsWebSocketsManager::GameThreadTick
		// which dispatches WebSocket OnConnected/OnMessage events)
		FTSBackgroundableTicker::GetCoreTicker().Tick(TickIntervalSeconds);

		// Pump the core ticker (drives other async work)
		FTSTicker::GetCoreTicker().Tick(TickIntervalSeconds);

		// Manually process pending SpacetimeDB server messages
		Test->ManualTick();

		// Brief sleep to avoid spinning CPU
		FPlatformProcess::Sleep(TickIntervalSeconds);
	}

	// Output results
	const FString& Report = Test->GetReport();
	if (!Report.IsEmpty())
	{
		UE_LOG(LogNyxSmokeTestCmd, Log, TEXT("%s"), *Report);
	}

	// Cleanup
	Test->RemoveFromRoot();

	const bool bPassed = !Test->IsRunning() && Report.Contains(TEXT("ALL PASSED"));
	UE_LOG(LogNyxSmokeTestCmd, Log, TEXT("NyxSmokeTestCommandlet finished: %s"),
		bPassed ? TEXT("SUCCESS") : TEXT("FAILURE"));

	return bPassed ? 0 : 1;
}
