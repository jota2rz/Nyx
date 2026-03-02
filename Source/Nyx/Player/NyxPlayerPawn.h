// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "NyxPlayerPawn.generated.h"

class UCapsuleComponent;
class USpringArmComponent;
class UCameraComponent;
class UNyxMovementComponent;
struct FInputActionValue;
class UInputMappingContext;
class UInputAction;

/**
 * Minimal player pawn for Spike 6: Client-Side Prediction & Reconciliation.
 *
 * This does NOT use UE5's replication or CharacterMovementComponent.
 * Movement is driven entirely by UNyxMovementComponent which talks to
 * SpacetimeDB via move_player reducer calls.
 *
 * For the spike:
 *  - Local player: predicted movement + server reconciliation
 *  - Remote player: interpolation between server updates
 */
UCLASS()
class NYX_API ANyxPlayerPawn : public APawn
{
	GENERATED_BODY()

public:
	ANyxPlayerPawn();

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void BeginPlay() override;

	/** Get the movement component */
	UFUNCTION(BlueprintCallable, Category = "Nyx")
	UNyxMovementComponent* GetNyxMovement() const { return NyxMovement; }

	/** Whether this is the locally controlled pawn (has input) */
	bool IsLocalNyxPlayer() const;

protected:
	// ──── Components ────

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCapsuleComponent> CapsuleComp;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UNyxMovementComponent> NyxMovement;

	// ──── Input ────

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> LookAction;

private:
	void HandleMove(const FInputActionValue& Value);
	void HandleLook(const FInputActionValue& Value);
};
