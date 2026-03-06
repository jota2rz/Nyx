// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxCharacter.h"
#include "NyxCharacterMovementComponent.h"
#include "Nyx/Nyx.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Camera/CameraComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "Nyx/Server/NyxServerSubsystem.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "DrawDebugHelpers.h"

ANyxCharacter::ANyxCharacter(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UNyxCharacterMovementComponent>(
		ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;

	// Capsule defaults (set by ACharacter, but be explicit)
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.f);

	// CMC defaults for MMO-style movement
	UCharacterMovementComponent* CMC = GetCharacterMovement();
	if (CMC)
	{
		CMC->MaxWalkSpeed = 600.f;
		CMC->BrakingDecelerationWalking = 2048.f;
		CMC->JumpZVelocity = 420.f;
		CMC->AirControl = 0.2f;
		CMC->bOrientRotationToMovement = true;
		CMC->RotationRate = FRotator(0.f, 540.f, 0.f);
	}

	// Don't rotate character with camera
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Spring arm for third-person camera
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->TargetArmLength = 400.f;
	SpringArm->bUsePawnControlRotation = true;

	// Camera
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;

	// ──── Mannequin mesh + animation ────
	static ConstructorHelpers::FObjectFinder<USkeletalMesh> MeshFinder(
		TEXT("/Game/Characters/Mannequins/Meshes/SKM_Manny_Simple"));
	if (MeshFinder.Succeeded())
	{
		GetMesh()->SetSkeletalMesh(MeshFinder.Object);
		GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, -96.f));
		GetMesh()->SetRelativeRotation(FRotator(0.f, -90.f, 0.f));
		UE_LOG(LogNyx, Log, TEXT("SKM_Manny_Simple loaded successfully"));
	}
	else
	{
		UE_LOG(LogNyx, Error, TEXT("FAILED to load SKM_Manny_Simple!"));
	}

	static ConstructorHelpers::FClassFinder<UAnimInstance> AnimFinder(
		TEXT("/Game/Characters/Mannequins/Anims/Unarmed/ABP_Unarmed"));
	if (AnimFinder.Succeeded())
	{
		GetMesh()->SetAnimInstanceClass(AnimFinder.Class);
		UE_LOG(LogNyx, Log, TEXT("ABP_Unarmed loaded successfully"));
	}
	else
	{
		UE_LOG(LogNyx, Error, TEXT("FAILED to load ABP_Unarmed!"));
	}

	// ──── Enhanced Input assets ────
	static ConstructorHelpers::FObjectFinder<UInputMappingContext> IMCFinder(
		TEXT("/Game/Input/IMC_Default"));
	if (IMCFinder.Succeeded())
	{
		DefaultMappingContext = IMCFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputMappingContext> IMCMouseLookFinder(
		TEXT("/Game/Input/IMC_MouseLook"));
	if (IMCMouseLookFinder.Succeeded())
	{
		MouseLookMappingContext = IMCMouseLookFinder.Object;
		UE_LOG(LogNyx, Log, TEXT("IMC_MouseLook loaded successfully"));
	}
	else
	{
		UE_LOG(LogNyx, Warning, TEXT("FAILED to load IMC_MouseLook"));
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> MoveFinder(
		TEXT("/Game/Input/Actions/IA_Move"));
	if (MoveFinder.Succeeded())
	{
		MoveAction = MoveFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> LookFinder(
		TEXT("/Game/Input/Actions/IA_Look"));
	if (LookFinder.Succeeded())
	{
		LookAction = LookFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> JumpFinder(
		TEXT("/Game/Input/Actions/IA_Jump"));
	if (JumpFinder.Succeeded())
	{
		JumpAction = JumpFinder.Object;
	}

	static ConstructorHelpers::FObjectFinder<UInputAction> MouseLookFinder(
		TEXT("/Game/Input/Actions/IA_MouseLook"));
	if (MouseLookFinder.Succeeded())
	{
		MouseLookAction = MouseLookFinder.Object;
		UE_LOG(LogNyx, Log, TEXT("IA_MouseLook loaded successfully"));
	}
	else
	{
		UE_LOG(LogNyx, Warning, TEXT("FAILED to load IA_MouseLook — mouse camera will not work"));
	}

	// Attack action — create programmatically (Digital/bool trigger)
	AttackAction = CreateDefaultSubobject<UInputAction>(TEXT("IA_Attack"));
	AttackAction->ValueType = EInputActionValueType::Boolean;
}

void ANyxCharacter::BeginPlay()
{
	Super::BeginPlay();

	UE_LOG(LogNyx, Log, TEXT("NyxCharacter::BeginPlay  Name=%s  Role=%s  HasAuthority=%s  Location=%s"),
		*GetName(),
		*UEnum::GetValueAsString(GetLocalRole()),
		HasAuthority() ? TEXT("true") : TEXT("false"),
		*GetActorLocation().ToString());

	// Log camera state for debugging black-screen issues
	if (GetNetMode() == NM_Client)
	{
		APlayerController* PC = Cast<APlayerController>(GetController());
		if (!PC) PC = GetWorld()->GetFirstPlayerController();
		UE_LOG(LogNyx, Log, TEXT("  Camera debug: PC=%s  IsLocal=%s  HasCameraMgr=%s  SpringArm=%s  Camera=%s"),
			*GetNameSafe(PC),
			(PC && PC->IsLocalController()) ? TEXT("true") : TEXT("false"),
			(PC && PC->PlayerCameraManager) ? TEXT("true") : TEXT("false"),
			SpringArm ? TEXT("valid") : TEXT("null"),
			Camera ? TEXT("valid") : TEXT("null"));
	}

	// ── Let pawns pass through each other (server AND client) ──
	// In the multi-server proxy architecture, multiple characters may spawn at the
	// same PlayerStart. Their capsules overlap, causing MoveAlongFloor sweeps to
	// block against each other — the server character never moves, producing
	// constant corrections that rubber-band the client back to spawn.
	// Must be set on BOTH server and client so client-side prediction matches
	// server-side results — otherwise the client blocks against the other pawn
	// while the server walks through, causing position desync and corrections.
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);

	// Try input setup here — will succeed for listen server host, may fail for client
	// (controller not replicated yet). Client gets a second chance in OnRep_PlayerState.
	SetupInputMappingContexts();

	// Proxy-connected clients may not get SetupPlayerInputComponent via the normal
	// possession chain (OnRep_Pawn → PawnClientRestart) due to proxy timing.
	// Start a retry timer to ensure input gets fully set up.
	// IMPORTANT: Only do this on actual game clients (NM_Client), NOT on the proxy
	// (which is NM_DedicatedServer/NM_ListenServer) or game servers.
	// NOTE: Do NOT check IsLocallyControlled() here — the proxy may assign a
	// NoPawnPlayerController which returns false for IsLocalController().
	// AutonomousProxy role alone is sufficient to identify the owned character.
	if (GetNetMode() == NM_Client && GetLocalRole() == ROLE_AutonomousProxy
		&& !bInputSetupComplete)
	{
		// Don't start retry timer if the local PC already has a fully-setup character.
		// This prevents a pre-migration pawn from server-2 stealing possession from
		// the active pawn on server-1 (race condition when claim fires before release).
		bool bShouldRetry = true;
		if (APlayerController* LocalPC = GetWorld()->GetFirstPlayerController())
		{
			if (APawn* CurrentPawn = LocalPC->GetPawn())
			{
				if (CurrentPawn != this)
				{
					ANyxCharacter* CurrentNyx = Cast<ANyxCharacter>(CurrentPawn);
					if (CurrentNyx && CurrentNyx->bInputSetupComplete)
					{
						UE_LOG(LogNyx, Log, TEXT("NyxCharacter::BeginPlay SKIP RetryInputSetup for %s — %s already controlled by %s"),
							*GetName(), *CurrentPawn->GetName(), *LocalPC->GetName());
						bShouldRetry = false;
					}
				}
			}
		}

		if (bShouldRetry)
		{
			GetWorld()->GetTimerManager().SetTimer(InputRetryTimerHandle,
				this, &ANyxCharacter::RetryInputSetup,
				0.2f, true, 0.5f);
		}
	}
}

void ANyxCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Server-side: mapping contexts needed if this is the listen server's local pawn
	SetupInputMappingContexts();
}

void ANyxCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorld()->GetTimerManager().ClearTimer(InputRetryTimerHandle);
	Super::EndPlay(EndPlayReason);
}

void ANyxCharacter::OnRep_Controller()
{
	// Guard: Super::OnRep_Controller() crashes (ACCESS_VIOLATION at 0x510)
	// when called before BeginPlay — the engine dereferences world/component
	// pointers that haven't been initialized yet. Skip entirely if too early.
	if (!HasActorBegunPlay())
	{
		UE_LOG(LogNyx, Warning, TEXT("NyxCharacter::OnRep_Controller SKIPPED (BeginPlay not called yet)  Name=%s  Controller=%s"),
			*GetName(), *GetNameSafe(GetController()));
		return;
	}

	Super::OnRep_Controller();

	APlayerController* PC = Cast<APlayerController>(GetController());

	UE_LOG(LogNyx, Log, TEXT("NyxCharacter::OnRep_Controller  Name=%s  Controller=%s  IsLocal=%s  InputComplete=%s"),
		*GetName(), *GetNameSafe(GetController()),
		(PC && PC->IsLocalController()) ? TEXT("true") : TEXT("false"),
		bInputSetupComplete ? TEXT("true") : TEXT("false"));

	if (GetNetMode() != NM_Client || GetLocalRole() != ROLE_AutonomousProxy)
	{
		return;
	}

	// ── Post-Migration Controller Re-bind ──
	// After proxy migration finalization, the client receives a NEW PlayerController
	// (e.g. PC_2 replacing PC_1). At this point bInputSetupComplete is already true
	// from the initial setup, but the NEW PC has no view target, no input bindings,
	// and no HUD. We must detect this "controller changed to a new local PC" case
	// and re-establish everything.
	if (!PC || !PC->IsLocalController())
	{
		// Try the world's first PC — proxy may not have set controller ownership yet
		PC = GetWorld()->GetFirstPlayerController();
	}

	if (PC && PC->IsLocalController())
	{
		if (bInputSetupComplete)
		{
			// Controller changed AFTER initial input setup — this is a migration swap.
			// Force full re-initialization on the new PC.
			UE_LOG(LogNyx, Log, TEXT("OnRep_Controller: Post-migration re-bind for %s → new PC=%s"),
				*GetName(), *GetNameSafe(PC));

			// Re-bind this pawn to the new PC
			PC->SetPawn(this);
			PC->ClientRestart(this);
			PC->SetViewTarget(this);

			// Re-apply input mapping contexts on the new PC
			SetupInputMappingContexts();

			UE_LOG(LogNyx, Log, TEXT("OnRep_Controller: Post-migration re-bind complete. ViewTarget=%s  HasCameraMgr=%s"),
				*GetName(),
				PC->PlayerCameraManager ? TEXT("true") : TEXT("false"));
		}
		else
		{
			// Initial setup — start the retry timer
			if (!GetWorld()->GetTimerManager().IsTimerActive(InputRetryTimerHandle))
			{
				GetWorld()->GetTimerManager().SetTimer(InputRetryTimerHandle,
					this, &ANyxCharacter::RetryInputSetup,
					0.1f, true, 0.0f); // immediate first tick, then 100ms retries
			}
		}
	}
}

void ANyxCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	// Client-side: this fires when the pawn's PlayerState replicates,
	// which means the controller/possession is now valid on the client.
	UE_LOG(LogNyx, Log, TEXT("NyxCharacter::OnRep_PlayerState  Name=%s  Role=%s  Controller=%s"),
		*GetName(),
		*UEnum::GetValueAsString(GetLocalRole()),
		*GetNameSafe(GetController()));

	SetupInputMappingContexts();
}

void ANyxCharacter::SetupInputMappingContexts()
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC || !PC->IsLocalController()) return;

	UEnhancedInputLocalPlayerSubsystem* InputSub =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
	if (!InputSub) return;

	if (DefaultMappingContext)
	{
		// Inject left-click → AttackAction into the shared IMC (idempotent)
		if (AttackAction)
		{
			DefaultMappingContext->MapKey(AttackAction, EKeys::LeftMouseButton);
		}
		InputSub->AddMappingContext(DefaultMappingContext, 0);
	}

	if (MouseLookMappingContext)
	{
		InputSub->AddMappingContext(MouseLookMappingContext, 1);
	}

	UE_LOG(LogNyx, Log, TEXT("Input mapping contexts applied for %s"), *GetName());
}

void ANyxCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput) return;

	if (MoveAction)
	{
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ANyxCharacter::HandleMove);
	}

	if (LookAction)
	{
		EnhancedInput->BindAction(LookAction, ETriggerEvent::Triggered, this, &ANyxCharacter::HandleLook);
	}

	if (MouseLookAction)
	{
		EnhancedInput->BindAction(MouseLookAction, ETriggerEvent::Triggered, this, &ANyxCharacter::HandleLook);
	}

	if (JumpAction)
	{
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
	}

	if (AttackAction)
	{
		EnhancedInput->BindAction(AttackAction, ETriggerEvent::Started, this, &ANyxCharacter::HandleAttack);
	}

	// This is the most reliable place to set up mapping contexts on clients.
	// SetupPlayerInputComponent is called by APlayerController::SetPawn when
	// possession completes — GetController() is guaranteed valid here.
	SetupInputMappingContexts();

	bInputSetupComplete = true;
	GetWorld()->GetTimerManager().ClearTimer(InputRetryTimerHandle);
	UE_LOG(LogNyx, Log, TEXT("SetupPlayerInputComponent complete for %s"), *GetName());

	// ── Proxy camera fix ──
	// In the multi-server proxy architecture, the PlayerCameraManager may not
	// automatically follow the possessed pawn because the PlayerController
	// arrives via a proxy connection rather than the normal login path.
	// Explicitly set the view target so the camera attaches to this character.
	if (GetNetMode() == NM_Client)
	{
		APlayerController* PC = Cast<APlayerController>(GetController());
		if (!PC || !PC->IsLocalController())
		{
			PC = GetWorld()->GetFirstPlayerController();
		}
		if (PC && PC->IsLocalController())
		{
			PC->SetViewTarget(this);
			UE_LOG(LogNyx, Log, TEXT("SetViewTarget → %s  (PC=%s  HasCameraManager=%s)"),
				*GetName(), *GetNameSafe(PC),
				PC->PlayerCameraManager ? TEXT("true") : TEXT("false"));
		}
	}
}

void ANyxCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ANyxCharacter, CurrentHP);
	DOREPLIFETIME(ANyxCharacter, MaxHP);
	DOREPLIFETIME(ANyxCharacter, CurrentMP);
	DOREPLIFETIME(ANyxCharacter, MaxMP);
	DOREPLIFETIME(ANyxCharacter, Level);
	DOREPLIFETIME(ANyxCharacter, DisplayName);
	DOREPLIFETIME(ANyxCharacter, AttackPower);
	DOREPLIFETIME(ANyxCharacter, Defense);
	DOREPLIFETIME(ANyxCharacter, MagicPower);
	DOREPLIFETIME(ANyxCharacter, MagicDefense);
	DOREPLIFETIME(ANyxCharacter, ServerName);
	DOREPLIFETIME(ANyxCharacter, ZoneName);
}

// ─── Stats ─────────────────────────────────────────────────────────

void ANyxCharacter::ApplyCharacterStats(const FCharacterStatsType& Stats)
{
	DisplayName = Stats.DisplayName;
	Level = static_cast<int32>(Stats.Level);
	MaxHP = Stats.MaxHp;
	CurrentHP = Stats.CurrentHp;
	MaxMP = Stats.MaxMp;
	CurrentMP = Stats.CurrentMp;
	AttackPower = static_cast<int32>(Stats.AttackPower);
	Defense = static_cast<int32>(Stats.Defense);
	MagicPower = static_cast<int32>(Stats.MagicPower);
	MagicDefense = static_cast<int32>(Stats.MagicDefense);
	SpacetimeIdentity = Stats.Identity;

	// Restore saved position and rotation from SpacetimeDB (critical for cross-server transfer)
	const FVector SavedPos(Stats.SavedPosX, Stats.SavedPosY, Stats.SavedPosZ);
	if (!SavedPos.IsNearlyZero())
	{
		SetActorLocation(SavedPos);
		UE_LOG(LogNyx, Log, TEXT("Restored %s position to (%.0f, %.0f, %.0f)"),
			*DisplayName, SavedPos.X, SavedPos.Y, SavedPos.Z);
	}

	// Restore rotation yaw
	if (!FMath::IsNearlyZero(Stats.SavedRotYaw, 0.1f) || !SavedPos.IsNearlyZero())
	{
		SetActorRotation(FRotator(0.f, Stats.SavedRotYaw, 0.f));
		if (Controller)
		{
			Controller->SetControlRotation(FRotator(0.f, Stats.SavedRotYaw, 0.f));
		}
		UE_LOG(LogNyx, Log, TEXT("Restored %s rotation yaw to %.1f"),
			*DisplayName, Stats.SavedRotYaw);
	}

	UE_LOG(LogNyx, Log, TEXT("Applied stats for %s: Level=%d HP=%d/%d MP=%d/%d ATK=%d DEF=%d"),
		*DisplayName, Level, CurrentHP, MaxHP, CurrentMP, MaxMP, AttackPower, Defense);
}

void ANyxCharacter::SetCurrentHP(int32 NewHP)
{
	const int32 OldHP = CurrentHP;
	CurrentHP = FMath::Clamp(NewHP, 0, MaxHP);

	if (OldHP != CurrentHP)
	{
		OnHPChanged.Broadcast(CurrentHP, MaxHP);

		if (CurrentHP <= 0 && OldHP > 0)
		{
			OnCharacterDied.Broadcast(this);
			UE_LOG(LogNyx, Log, TEXT("%s has died!"), *DisplayName);
		}
	}
}

void ANyxCharacter::SetCurrentMP(int32 NewMP)
{
	CurrentMP = FMath::Clamp(NewMP, 0, MaxMP);
}

void ANyxCharacter::OnRep_CurrentHP()
{
	UE_LOG(LogNyx, Log, TEXT("[Client] OnRep_CurrentHP for %s: HP=%d/%d"),
		*GetDisplayName(), CurrentHP, MaxHP);

	OnHPChanged.Broadcast(CurrentHP, MaxHP);

	if (CurrentHP <= 0)
	{
		OnCharacterDied.Broadcast(this);
	}
}

// ─── Input ─────────────────────────────────────────────────────────

void ANyxCharacter::RetryInputSetup()
{
	if (bInputSetupComplete)
	{
		GetWorld()->GetTimerManager().ClearTimer(InputRetryTimerHandle);
		return;
	}

	// Only run on actual game clients, not proxy/server processes.
	if (GetNetMode() != NM_Client) return;

	APlayerController* PC = Cast<APlayerController>(GetController());

	// The multi-server proxy may assign a NoPawnPlayerController from a
	// non-primary game server. Try the local player's controller instead.
	if (!PC || !PC->IsLocalController())
	{
		if (UWorld* W = GetWorld())
		{
			PC = W->GetFirstPlayerController();
		}
	}

	if (PC && PC->IsLocalController())
	{
		// Don't steal possession from an already-active, input-bound pawn.
		// This prevents a pre-migration pawn (from non-primary server) from
		// hijacking the local PC while the original pawn is still alive.
		APawn* CurrentPawn = PC->GetPawn();
		if (CurrentPawn && CurrentPawn != this)
		{
			ANyxCharacter* CurrentNyx = Cast<ANyxCharacter>(CurrentPawn);
			if (CurrentNyx && CurrentNyx->bInputSetupComplete)
			{
				// Active pawn exists — wait for migration to complete.
				// OnRep_Controller will set us up when the new PC arrives.
				return;
			}
		}

		UE_LOG(LogNyx, Log, TEXT("RetryInputSetup: Forcing PawnClientRestart for %s (Controller=%s)"),
			*GetName(), *GetNameSafe(PC));

		// Ensure the correct controller acknowledges this pawn
		if (GetController() != PC)
		{
			PC->SetPawn(this);
		}

		// Force the client-side possession chain which triggers
		// SetupPlayerInputComponent via Restart() → CreatePlayerInputComponent
		PC->ClientRestart(this);

		// Force camera to track this character. NoPawnPlayerController::GetViewTarget()
		// returns a custom target (not the pawn), so the camera component won't activate
		// unless we explicitly set the view target.
		PC->SetViewTarget(this);
	}
}

void ANyxCharacter::HandleMove(const FInputActionValue& Value)
{
	const FVector2D MoveInput = Value.Get<FVector2D>();

	// Use whatever controller is available — on proxy clients GetController()
	// may return NoPawnPlayerController, but movement math only needs a rotation.
	AController* MoveController = Controller;
	if (!MoveController)
	{
		// Fallback: find the local player controller
		if (UWorld* W = GetWorld())
		{
			MoveController = W->GetFirstPlayerController();
		}
	}

	if (MoveController)
	{
		const FRotator ControlRot(0.f, MoveController->GetControlRotation().Yaw, 0.f);
		const FVector Forward = FRotationMatrix(ControlRot).GetUnitAxis(EAxis::X);
		const FVector Right = FRotationMatrix(ControlRot).GetUnitAxis(EAxis::Y);

		// AddMovementInput is the standard UE5 way — CMC handles the rest,
		// including replication, prediction, and correction.
		AddMovementInput(Forward, MoveInput.Y);
		AddMovementInput(Right, MoveInput.X);
	}
}

void ANyxCharacter::HandleLook(const FInputActionValue& Value)
{
	const FVector2D LookInput = Value.Get<FVector2D>();

	// AddControllerYawInput/Pitch work on the pawn's Controller member.
	// If Controller is null but we have a local PC, use it directly.
	if (Controller)
	{
		AddControllerYawInput(LookInput.X);
		AddControllerPitchInput(LookInput.Y);
	}
	else if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
	{
		PC->AddYawInput(LookInput.X);
		PC->AddPitchInput(LookInput.Y);
	}
}

void ANyxCharacter::HandleAttack()
{
	// Cooldown check (client-side, prevents spam)
	const float Now = GetWorld()->GetTimeSeconds();
	if (Now - LastAttackTime < AttackCooldown) return;
	LastAttackTime = Now;

	UE_LOG(LogNyx, Log, TEXT("%s: Attack pressed!"), *GetDisplayName());

	// Call Server RPC — server will do the line trace and SpacetimeDB call
	ServerRPC_RequestAttack();
}

void ANyxCharacter::ServerRPC_RequestAttack_Implementation()
{
	// Server-side: line trace forward from this character to find a target
	const FVector Start = GetActorLocation() + FVector(0.f, 0.f, 50.f); // eye height
	const FVector Forward = GetActorForwardVector();
	const FVector End = Start + Forward * AttackRange;

	FHitResult HitResult;
	FCollisionQueryParams Params;
	Params.AddIgnoredActor(this);

	const bool bHit = GetWorld()->LineTraceSingleByChannel(
		HitResult, Start, End, ECC_Pawn, Params);

	// Debug line (visible in editor)
	DrawDebugLine(GetWorld(), Start, End,
		bHit ? FColor::Red : FColor::Green, false, 1.0f, 0, 2.f);

	if (!bHit)
	{
		UE_LOG(LogNyx, Log, TEXT("%s: Attack missed (no target in range)"), *GetDisplayName());
		return;
	}

	ANyxCharacter* Target = Cast<ANyxCharacter>(HitResult.GetActor());
	if (!Target || Target == this)
	{
		UE_LOG(LogNyx, Log, TEXT("%s: Attack hit non-character"), *GetDisplayName());
		return;
	}

	UE_LOG(LogNyx, Log, TEXT("%s attacks %s! (range=%.0f)"),
		*GetDisplayName(), *Target->GetDisplayName(),
		FVector::Dist(GetActorLocation(), Target->GetActorLocation()));

	// Route through NyxServerSubsystem → SpacetimeDB
	UNyxServerSubsystem* ServerSub = GetGameInstance()->GetSubsystem<UNyxServerSubsystem>();
	if (ServerSub)
	{
		ServerSub->RequestResolveHit(SpacetimeIdentity, Target->SpacetimeIdentity, 0); // skill 0 = basic attack
	}
}

void ANyxCharacter::ServerRPC_ForceNetCorrection_Implementation(float Offset)
{
	// Server-side: teleport the character by offset along a random horizontal direction.
	// The CMC will detect the mismatch vs client prediction and send a net correction.
	const float Angle = FMath::FRandRange(0.f, 360.f);
	const FVector Delta(FMath::Cos(FMath::DegreesToRadians(Angle)) * Offset,
						 FMath::Sin(FMath::DegreesToRadians(Angle)) * Offset,
						 0.f);

	const FVector OldLocation = GetActorLocation();
	const FVector NewLocation = OldLocation + Delta;

	UE_LOG(LogNyx, Warning, TEXT("ForceNetCorrection: Server teleporting %s by %.0f units "
		"from (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f)"),
		*GetDisplayName(), Offset,
		OldLocation.X, OldLocation.Y, OldLocation.Z,
		NewLocation.X, NewLocation.Y, NewLocation.Z);

	SetActorLocation(NewLocation);
}

void ANyxCharacter::ClientRPC_TransferToServer_Implementation(const FString& Address)
{
	UE_LOG(LogNyx, Log, TEXT("Zone transfer: travelling to %s"), *Address);

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (PC)
	{
		PC->ClientTravel(Address, TRAVEL_Absolute);
	}
}
