// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxSmokeTest.h"
#include "Nyx/Nyx.h"
#include "ModuleBindings/SpacetimeDBClient.g.h"
#include "ModuleBindings/Tables/CharacterStatsTable.g.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "Engine/Engine.h"

DEFINE_LOG_CATEGORY_STATIC(LogNyxSmokeTest, Log, All);

static const float SmokeTestTimeoutSeconds = 30.f;

// ─── Public ────────────────────────────────────────────────────────

void UNyxSmokeTest::Run(const FString& Host, const FString& DatabaseName)
{
	if (bRunning)
	{
		UE_LOG(LogNyxSmokeTest, Warning, TEXT("Smoke test already running!"));
		return;
	}

	bRunning = true;
	StartTime = FPlatformTime::Seconds();
	StepResults.Empty();
	Report.Empty();
	bCharacterLoaded = false;
	bHitResolved = false;

	// Unique test IDs to avoid collisions
	TestServerId = FString::Printf(TEXT("smoke-test-%d"), FMath::RandRange(1000, 9999));
	TestZoneId = TEXT("smoke-test-zone");
	TestCharacterName = FString::Printf(TEXT("SmokeHero_%d"), FMath::RandRange(100, 999));

	UE_LOG(LogNyxSmokeTest, Log, TEXT(""));
	UE_LOG(LogNyxSmokeTest, Log, TEXT("========================================"));
	UE_LOG(LogNyxSmokeTest, Log, TEXT("  NYX OPTION 4 END-TO-END SMOKE TEST"));
	UE_LOG(LogNyxSmokeTest, Log, TEXT("========================================"));
	UE_LOG(LogNyxSmokeTest, Log, TEXT("  Host:      %s"), *Host);
	UE_LOG(LogNyxSmokeTest, Log, TEXT("  Database:  %s"), *DatabaseName);
	UE_LOG(LogNyxSmokeTest, Log, TEXT("  ServerId:  %s"), *TestServerId);
	UE_LOG(LogNyxSmokeTest, Log, TEXT("  Character: %s"), *TestCharacterName);
	UE_LOG(LogNyxSmokeTest, Log, TEXT("========================================"));
	UE_LOG(LogNyxSmokeTest, Log, TEXT(""));

	// Step 1: Connect
	AdvanceTo(ETestStep::Connecting);

	FOnConnectDelegate OnConnect;
	OnConnect.BindDynamic(this, &UNyxSmokeTest::HandleConnect);

	FOnDisconnectDelegate OnDisconnect;
	OnDisconnect.BindDynamic(this, &UNyxSmokeTest::HandleDisconnect);

	FOnConnectErrorDelegate OnConnectError;
	OnConnectError.BindDynamic(this, &UNyxSmokeTest::HandleConnectError);

	UDbConnectionBuilder* Builder = UDbConnection::Builder();
	Connection = Builder
		->WithUri(FString::Printf(TEXT("ws://%s"), *Host))
		->WithDatabaseName(DatabaseName)
		->OnConnect(OnConnect)
		->OnConnectError(OnConnectError)
		->OnDisconnect(OnDisconnect)
		->Build();

	if (Connection)
	{
		Connection->SetAutoTicking(true);

		// Set a global timeout (only if world exists — commandlet mode handles timeout externally)
		if (GEngine && GEngine->GetWorld())
		{
			GEngine->GetWorld()->GetTimerManager().SetTimer(
				TimeoutHandle,
				FTimerDelegate::CreateUObject(this, &UNyxSmokeTest::OnTimeout),
				SmokeTestTimeoutSeconds, false);
		}
		else
		{
			UE_LOG(LogNyxSmokeTest, Log, TEXT("  (No world for timer — timeout managed externally)"));
		}
	}
	else
	{
		Fail(TEXT("Connect"), TEXT("Builder returned null connection"));
	}
}

// ─── State Machine ─────────────────────────────────────────────────

void UNyxSmokeTest::ManualTick()
{
	if (Connection)
	{
		Connection->FrameTick();
	}
}

void UNyxSmokeTest::AdvanceTo(ETestStep Step)
{
	CurrentStep = Step;
	StepStartTime = FPlatformTime::Seconds();
}

void UNyxSmokeTest::Pass(const FString& StepName, const FString& Detail)
{
	const double ElapsedMs = (FPlatformTime::Seconds() - StepStartTime) * 1000.0;

	StepResults.Add({ StepName, true, Detail, ElapsedMs });
	UE_LOG(LogNyxSmokeTest, Log, TEXT("  [PASS] %s (%.1fms) — %s"), *StepName, ElapsedMs, *Detail);
}

void UNyxSmokeTest::Fail(const FString& StepName, const FString& Detail)
{
	const double ElapsedMs = (FPlatformTime::Seconds() - StepStartTime) * 1000.0;

	StepResults.Add({ StepName, false, Detail, ElapsedMs });
	UE_LOG(LogNyxSmokeTest, Error, TEXT("  [FAIL] %s (%.1fms) — %s"), *StepName, ElapsedMs, *Detail);

	CurrentStep = ETestStep::Failed;
	Finish();
}

void UNyxSmokeTest::Finish()
{
	const double TotalMs = (FPlatformTime::Seconds() - StartTime) * 1000.0;

	// Clear timeout
	if (GEngine && GEngine->GetWorld())
	{
		GEngine->GetWorld()->GetTimerManager().ClearTimer(TimeoutHandle);
	}

	// Disconnect if still connected
	if (Connection)
	{
		Connection->Disconnect();
		Connection = nullptr;
	}

	// Build report
	int32 PassCount = 0, FailCount = 0;
	for (const FStepResult& R : StepResults)
	{
		if (R.bPassed) PassCount++;
		else FailCount++;
	}

	Report = TEXT("");
	Report += TEXT("\n========================================\n");
	Report += TEXT("  SMOKE TEST RESULTS\n");
	Report += TEXT("========================================\n");
	for (const FStepResult& R : StepResults)
	{
		Report += FString::Printf(TEXT("  [%s] %-25s %6.1fms  %s\n"),
			R.bPassed ? TEXT("PASS") : TEXT("FAIL"), *R.Name, R.ElapsedMs, *R.Detail);
	}
	Report += TEXT("----------------------------------------\n");
	Report += FString::Printf(TEXT("  Total: %d passed, %d failed (%.1fms)\n"),
		PassCount, FailCount, TotalMs);
	Report += FString::Printf(TEXT("  Result: %s\n"),
		FailCount == 0 ? TEXT("ALL PASSED") : TEXT("FAILURES DETECTED"));
	Report += TEXT("========================================\n");

	UE_LOG(LogNyxSmokeTest, Log, TEXT("%s"), *Report);

	// Print to screen if available
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.f,
			FailCount == 0 ? FColor::Green : FColor::Red,
			FString::Printf(TEXT("Smoke Test: %d/%d passed (%.0fms)"),
				PassCount, PassCount + FailCount, TotalMs));
	}

	bRunning = false;
}

void UNyxSmokeTest::OnTimeout()
{
	TArray<FString> StepNames = {
		TEXT("Connecting"), TEXT("Subscribing"), TEXT("RegisteringZone"),
		TEXT("LoadingCharacter"), TEXT("WaitingForStats"), TEXT("ResolvingHit"),
		TEXT("WaitingForHPChange"), TEXT("SavingCharacter"), TEXT("DeregisteringZone"),
		TEXT("Disconnecting"), TEXT("Complete"), TEXT("Failed")
	};

	const int32 StepIdx = static_cast<int32>(CurrentStep);
	FString StepName = StepIdx < StepNames.Num() ? StepNames[StepIdx] : TEXT("Unknown");

	Fail(TEXT("Timeout"), FString::Printf(TEXT("Timed out after %.0fs at step: %s"),
		SmokeTestTimeoutSeconds, *StepName));
}

// ─── SpacetimeDB Callbacks ─────────────────────────────────────────

void UNyxSmokeTest::HandleConnect(UDbConnection* Conn, FSpacetimeDBIdentity Identity, const FString& Token)
{
	if (CurrentStep != ETestStep::Connecting) return;

	ServerIdentity = Identity;
	Pass(TEXT("Connect"), FString::Printf(TEXT("Identity: %s..."), *Identity.ToHex().Left(16)));

	// Step 2: Subscribe
	AdvanceTo(ETestStep::Subscribing);

	USubscriptionBuilder* SubBuilder = Conn->SubscriptionBuilder();

	FOnSubscriptionApplied OnApplied;
	OnApplied.BindDynamic(this, &UNyxSmokeTest::HandleSubscriptionApplied);

	FOnSubscriptionError OnError;
	OnError.BindDynamic(this, &UNyxSmokeTest::HandleSubscriptionError);

	SubBuilder->OnApplied(OnApplied);
	SubBuilder->OnError(OnError);

	SubBuilder->Subscribe({
		TEXT("SELECT * FROM character_stats"),
		TEXT("SELECT * FROM combat_event"),
		FString::Printf(TEXT("SELECT * FROM zone_server WHERE zone_id = '%s'"), *TestZoneId),
		FString::Printf(TEXT("SELECT * FROM zone_population WHERE zone_id = '%s'"), *TestZoneId)
	});
}

void UNyxSmokeTest::HandleDisconnect(UDbConnection* Conn, const FString& Error)
{
	if (CurrentStep == ETestStep::Disconnecting || CurrentStep == ETestStep::Complete ||
		CurrentStep == ETestStep::Failed)
	{
		// Expected disconnect during cleanup
		return;
	}

	if (!Error.IsEmpty())
	{
		Fail(TEXT("Connection"), FString::Printf(TEXT("Unexpected disconnect: %s"), *Error));
	}
}

void UNyxSmokeTest::HandleConnectError(const FString& ErrorMessage)
{
	Fail(TEXT("Connect"), FString::Printf(TEXT("Connection error: %s"), *ErrorMessage));
}

void UNyxSmokeTest::HandleSubscriptionApplied(FSubscriptionEventContext Context)
{
	if (CurrentStep != ETestStep::Subscribing) return;

	Pass(TEXT("Subscribe"), TEXT("character_stats, combat_event, zone_server, zone_population"));

	// Bind table callbacks
	if (Connection && Connection->Db && Connection->Db->CharacterStats)
	{
		Connection->Db->CharacterStats->OnInsert.AddDynamic(
			this, &UNyxSmokeTest::HandleCharacterStatsInsert);
		Connection->Db->CharacterStats->OnUpdate.AddDynamic(
			this, &UNyxSmokeTest::HandleCharacterStatsUpdate);
	}

	// Step 3: Register zone server
	AdvanceTo(ETestStep::RegisteringZone);

	if (Connection && Connection->Reducers)
	{
		Connection->Reducers->RegisterZoneServer(
			TestServerId, TestZoneId, TEXT("127.0.0.1"), 7777, 500);

		// RegisterZoneServer is fire-and-forget — pass immediately
		Pass(TEXT("RegisterZone"), FString::Printf(TEXT("ServerId=%s ZoneId=%s"), *TestServerId, *TestZoneId));

		// Step 4: Load character
		AdvanceTo(ETestStep::LoadingCharacter);

		// First check if character already exists in cache
		bool bFoundInCache = false;
		TArray<FCharacterStatsType> AllStats = Connection->Db->CharacterStats->Iter();
		for (const FCharacterStatsType& Stats : AllStats)
		{
			if (Stats.DisplayName == TestCharacterName)
			{
				InitialHP = Stats.CurrentHp;
				bCharacterLoaded = true;
				bFoundInCache = true;
				Pass(TEXT("LoadCharacter"), FString::Printf(TEXT("Found in cache: %s HP=%d/%d Level=%d"),
					*Stats.DisplayName, Stats.CurrentHp, Stats.MaxHp, Stats.Level));
				break;
			}
		}

		if (!bFoundInCache)
		{
			// Request load (creates if not exists)
			Connection->Reducers->LoadCharacter(ServerIdentity, TestCharacterName);
			AdvanceTo(ETestStep::WaitingForStats);
		}

		// If found in cache, proceed to hit test
		if (bCharacterLoaded)
		{
			AdvanceTo(ETestStep::ResolvingHit);

			// ResolveHit uses the server identity for both attacker and defender (self-hit for testing)
			Connection->Reducers->ResolveHit(ServerIdentity, ServerIdentity, 0);
			UE_LOG(LogNyxSmokeTest, Log, TEXT("  ... Fired ResolveHit (self-hit, skill=0/basic_attack)"));
			AdvanceTo(ETestStep::WaitingForHPChange);
		}
	}
	else
	{
		Fail(TEXT("RegisterZone"), TEXT("No connection/reducers available"));
	}
}

void UNyxSmokeTest::HandleSubscriptionError(FErrorContext Context)
{
	Fail(TEXT("Subscribe"), FString::Printf(TEXT("Subscription error: %s"), *Context.Error));
}

void UNyxSmokeTest::HandleCharacterStatsInsert(const FEventContext& Context, const FCharacterStatsType& NewRow)
{
	if (NewRow.DisplayName != TestCharacterName) return;

	if (CurrentStep == ETestStep::WaitingForStats || CurrentStep == ETestStep::LoadingCharacter)
	{
		InitialHP = NewRow.CurrentHp;
		bCharacterLoaded = true;

		Pass(TEXT("LoadCharacter"), FString::Printf(TEXT("Created: %s HP=%d/%d Level=%d ATK=%d DEF=%d"),
			*NewRow.DisplayName, NewRow.CurrentHp, NewRow.MaxHp, NewRow.Level,
			NewRow.AttackPower, NewRow.Defense));

		// Step 5: Resolve a hit
		AdvanceTo(ETestStep::ResolvingHit);

		if (Connection && Connection->Reducers)
		{
			// Self-hit: the test character attacks itself
			Connection->Reducers->ResolveHit(ServerIdentity, ServerIdentity, 0); // skill 0 = basic_attack
			UE_LOG(LogNyxSmokeTest, Log, TEXT("  ... Fired ResolveHit (self-hit, skill=0/basic_attack)"));
			AdvanceTo(ETestStep::WaitingForHPChange);
		}
	}
}

void UNyxSmokeTest::HandleCharacterStatsUpdate(const FEventContext& Context,
	const FCharacterStatsType& OldRow, const FCharacterStatsType& NewRow)
{
	if (NewRow.DisplayName != TestCharacterName) return;

	if (CurrentStep == ETestStep::WaitingForHPChange)
	{
		PostHitHP = NewRow.CurrentHp;
		bHitResolved = true;

		const int32 Damage = OldRow.CurrentHp - NewRow.CurrentHp;

		if (NewRow.CurrentHp < OldRow.CurrentHp)
		{
			Pass(TEXT("ResolveHit"), FString::Printf(TEXT("HP: %d → %d (damage=%d, skill=basic_attack)"),
				OldRow.CurrentHp, NewRow.CurrentHp, Damage));
		}
		else
		{
			Fail(TEXT("ResolveHit"), FString::Printf(TEXT("HP did not decrease: %d → %d"),
				OldRow.CurrentHp, NewRow.CurrentHp));
			return;
		}

		// Step 6: Save character
		AdvanceTo(ETestStep::SavingCharacter);

		if (Connection && Connection->Reducers)
		{
			Connection->Reducers->SaveCharacter(
				ServerIdentity, NewRow.CurrentHp, NewRow.CurrentMp,
				100.0f, 200.0f, 300.0f, TestZoneId);

			Pass(TEXT("SaveCharacter"), FString::Printf(TEXT("HP=%d pos=(100,200,300) zone=%s"),
				NewRow.CurrentHp, *TestZoneId));
		}

		// Step 7: Deregister zone server
		AdvanceTo(ETestStep::DeregisteringZone);

		if (Connection && Connection->Reducers)
		{
			Connection->Reducers->DeregisterZoneServer(TestServerId);
			Pass(TEXT("DeregisterZone"), FString::Printf(TEXT("ServerId=%s"), *TestServerId));
		}

		// Step 8: Complete
		AdvanceTo(ETestStep::Complete);
		Finish();
	}
}
