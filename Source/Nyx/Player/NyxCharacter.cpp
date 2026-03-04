// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxCharacter.h"
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
#include "DrawDebugHelpers.h"

ANyxCharacter::ANyxCharacter()
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

	UE_LOG(LogNyx, Log, TEXT("NyxCharacter::BeginPlay  Name=%s  Role=%s  HasAuthority=%s"),
		*GetName(),
		*UEnum::GetValueAsString(GetLocalRole()),
		HasAuthority() ? TEXT("true") : TEXT("false"));

	// Try input setup here — will succeed for listen server host, may fail for client
	// (controller not replicated yet). Client gets a second chance in OnRep_PlayerState.
	SetupInputMappingContexts();
}

void ANyxCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// Server-side: mapping contexts needed if this is the listen server's local pawn
	SetupInputMappingContexts();
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

void ANyxCharacter::HandleMove(const FInputActionValue& Value)
{
	const FVector2D MoveInput = Value.Get<FVector2D>();

	if (Controller)
	{
		const FRotator ControlRot(0.f, Controller->GetControlRotation().Yaw, 0.f);
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

	if (Controller)
	{
		AddControllerYawInput(LookInput.X);
		AddControllerPitchInput(LookInput.Y);
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
