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
#include "Net/UnrealNetwork.h"

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
}

void ANyxCharacter::BeginPlay()
{
	Super::BeginPlay();

	UE_LOG(LogNyx, Log, TEXT("NyxCharacter::BeginPlay  Name=%s  Role=%s  HasAuthority=%s"),
		*GetName(),
		*UEnum::GetValueAsString(GetLocalRole()),
		HasAuthority() ? TEXT("true") : TEXT("false"));

	// Add input mapping context for the local player
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* InputSub =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			if (DefaultMappingContext)
			{
				InputSub->AddMappingContext(DefaultMappingContext, 0);
			}
		}
	}
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

	if (JumpAction)
	{
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Started, this, &ACharacter::Jump);
		EnhancedInput->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);
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

	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		PC->AddYawInput(LookInput.X);
		PC->AddPitchInput(LookInput.Y);
	}
}
