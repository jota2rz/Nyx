// Copyright Nyx MMO Project. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "ModuleBindings/Types/CharacterStatsType.g.h"
#include "NyxCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
class UNyxServerSubsystem;
struct FInputActionValue;
class UInputMappingContext;
class UInputAction;

/**
 * Nyx player character — Option 4 architecture.
 *
 * Uses standard UCharacterMovementComponent (CMC) for movement.
 * UE5 dedicated server is authoritative for movement/physics.
 * SpacetimeDB handles persistence (stats) and combat compute.
 *
 * Replicated properties (server → client):
 *   - HP, MP, Level via standard UE5 rep
 *   - Position/rotation via CMC (automatic)
 *
 * Flow:
 *   1. Player joins dedicated server
 *   2. Server calls SpacetimeDB::LoadCharacter(identity)
 *   3. SpacetimeDB returns CharacterStats via subscription
 *   4. Server applies stats to this character
 *   5. Movement handled entirely by CMC (no SpacetimeDB involvement)
 *   6. Combat: server calls SpacetimeDB::ResolveHit/ResolveHeal
 *   7. SpacetimeDB updates CharacterStats → server receives via subscription
 *   8. Server replicates updated stats to owning client
 *   9. Server periodically calls SpacetimeDB::SaveCharacter for persistence
 */
UCLASS()
class NYX_API ANyxCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ANyxCharacter(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	virtual void OnRep_Controller() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ──── Stats (replicated from dedicated server) ────

	/** Apply stats loaded from SpacetimeDB. Called on the server. */
	void ApplyCharacterStats(const FCharacterStatsType& Stats);

	/** Get current HP */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Stats")
	int32 GetCurrentHP() const { return CurrentHP; }

	/** Get max HP */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Stats")
	int32 GetMaxHP() const { return MaxHP; }

	/** Get current MP */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Stats")
	int32 GetCurrentMP() const { return CurrentMP; }

	/** Get character level */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Stats")
	int32 GetCharacterLevel() const { return Level; }

	/** Get display name */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Stats")
	const FString& GetDisplayName() const { return DisplayName; }

	/** Is this character dead? */
	UFUNCTION(BlueprintCallable, Category = "Nyx|Stats")
	bool IsDead() const { return CurrentHP <= 0; }

	/** Update HP from SpacetimeDB combat resolution. Called on the server. */
	void SetCurrentHP(int32 NewHP);

	/** Update MP. Called on the server. */
	void SetCurrentMP(int32 NewMP);

	/** The SpacetimeDB identity associated with this character. */
	FSpacetimeDBIdentity SpacetimeIdentity;

	// ──── Delegates ────

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnHPChanged, int32, NewHP, int32, MaxHP);

	UPROPERTY(BlueprintAssignable, Category = "Nyx|Stats")
	FOnHPChanged OnHPChanged;

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCharacterDied, ANyxCharacter*, Character);

	UPROPERTY(BlueprintAssignable, Category = "Nyx|Stats")
	FOnCharacterDied OnCharacterDied;

protected:
	// ──── Components ────

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USpringArmComponent> SpringArm;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UCameraComponent> Camera;

	// ──── Input ────

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> MouseLookMappingContext;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> MoveAction;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> LookAction;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> JumpAction;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> MouseLookAction;

	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputAction> AttackAction;

	// ──── Replicated Stats ────

	UPROPERTY(ReplicatedUsing = OnRep_CurrentHP, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 CurrentHP = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 MaxHP = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 CurrentMP = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 MaxMP = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 Level = 1;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	FString DisplayName;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 AttackPower = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 Defense = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 MagicPower = 0;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Stats")
	int32 MagicDefense = 0;

public:
	// ──── Server/Zone Info (replicated for HUD) ────

	/** Which game server this character is on (e.g. "server-1"). Set by GameMode on PostLogin. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Network")
	FString ServerName;

	/** Which zone this character is in (e.g. "Zone-1 (West)"). Set by GameMode on PostLogin. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Nyx|Network")
	FString ZoneName;

private:
	void HandleMove(const FInputActionValue& Value);
	void HandleLook(const FInputActionValue& Value);
	void HandleAttack();

	/** Server RPC: client requests a basic attack. Server does line trace + SpacetimeDB ResolveHit. */
	UFUNCTION(Server, Reliable)
	void ServerRPC_RequestAttack();

public:
	/** Server RPC: teleport character server-side to force a CMC net correction. */
	UFUNCTION(Server, Reliable)
	void ServerRPC_ForceNetCorrection(float Offset);

	/** Client RPC: server tells client to travel to another server for zone transfer. */
	UFUNCTION(Client, Reliable)
	void ClientRPC_TransferToServer(const FString& Address);

private:

	/** Attack cooldown */
	float LastAttackTime = 0.f;
	static constexpr float AttackCooldown = 0.5f;
	static constexpr float AttackRange = 300.f;

	UFUNCTION()
	void OnRep_CurrentHP();

	/** Sets up input mapping contexts for the local player. Safe to call multiple times. */
	void SetupInputMappingContexts();

	/** Deferred input setup for proxy-connected clients. */
	void RetryInputSetup();

	/** True once SetupPlayerInputComponent has been called successfully. */
	bool bInputSetupComplete = false;

	FTimerHandle InputRetryTimerHandle;
};
