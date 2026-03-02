// Copyright Nyx MMO Project. All Rights Reserved.

#include "NyxPlayerPawn.h"
#include "Nyx/Nyx.h"
#include "Nyx/Player/NyxMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"

ANyxPlayerPawn::ANyxPlayerPawn()
{
	PrimaryActorTick.bCanEverTick = false; // Movement component ticks, not the pawn

	// Capsule root
	CapsuleComp = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	CapsuleComp->InitCapsuleSize(42.f, 96.f);
	CapsuleComp->SetCollisionProfileName(TEXT("Pawn"));
	SetRootComponent(CapsuleComp);

	// Spring arm for third-person-ish camera
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("SpringArm"));
	SpringArm->SetupAttachment(CapsuleComp);
	SpringArm->TargetArmLength = 400.f;
	SpringArm->bUsePawnControlRotation = true;

	// Camera
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm, USpringArmComponent::SocketName);
	Camera->bUsePawnControlRotation = false;

	// Movement component (prediction + reconciliation)
	NyxMovement = CreateDefaultSubobject<UNyxMovementComponent>(TEXT("NyxMovement"));
}

void ANyxPlayerPawn::BeginPlay()
{
	Super::BeginPlay();

	// Add input mapping context for the local player
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* InputSub =
			ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer()))
		{
			if (DefaultMappingContext)
			{
				InputSub->AddMappingContext(DefaultMappingContext, 0);
				UE_LOG(LogNyx, Log, TEXT("NyxPlayerPawn: Added input mapping context"));
			}
			else
			{
				UE_LOG(LogNyx, Warning, TEXT("NyxPlayerPawn: No DefaultMappingContext set — input won't work. Set it in the pawn defaults."));
			}
		}
	}
}

void ANyxPlayerPawn::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent);
	if (!EnhancedInput)
	{
		UE_LOG(LogNyx, Error, TEXT("NyxPlayerPawn requires Enhanced Input. Check Project Settings > Input > Default Classes."));
		return;
	}

	if (MoveAction)
	{
		EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ANyxPlayerPawn::HandleMove);
	}

	if (LookAction)
	{
		EnhancedInput->BindAction(LookAction, ETriggerEvent::Triggered, this, &ANyxPlayerPawn::HandleLook);
	}

	UE_LOG(LogNyx, Log, TEXT("NyxPlayerPawn: Input bound (Move=%s, Look=%s)"),
		MoveAction ? TEXT("yes") : TEXT("no"),
		LookAction ? TEXT("yes") : TEXT("no"));
}

bool ANyxPlayerPawn::IsLocalNyxPlayer() const
{
	return GetController() && GetController()->IsLocalController();
}

void ANyxPlayerPawn::HandleMove(const FInputActionValue& Value)
{
	if (!NyxMovement) return;

	const FVector2D MoveInput = Value.Get<FVector2D>();

	// Convert input to world-space movement direction based on controller rotation
	if (Controller)
	{
		const FRotator ControlRot(0.f, Controller->GetControlRotation().Yaw, 0.f);
		const FVector Forward = FRotationMatrix(ControlRot).GetUnitAxis(EAxis::X);
		const FVector Right = FRotationMatrix(ControlRot).GetUnitAxis(EAxis::Y);

		FVector MoveDirection = Forward * MoveInput.Y + Right * MoveInput.X;
		MoveDirection.Normalize();
		NyxMovement->AddMovementInput(MoveDirection);
	}
}

void ANyxPlayerPawn::HandleLook(const FInputActionValue& Value)
{
	const FVector2D LookInput = Value.Get<FVector2D>();

	if (APlayerController* PC = Cast<APlayerController>(Controller))
	{
		PC->AddYawInput(LookInput.X);
		PC->AddPitchInput(LookInput.Y);
	}
}
