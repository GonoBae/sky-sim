// Copyright Epic Games, Inc. All Rights Reserved.

#include "SkySimSpectatorPawn.h"

#include "Camera/CameraComponent.h"
#include "Components/SphereComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpectatorPawnMovement.h"
#include "InputCoreTypes.h"

ASkySimSpectatorPawn::ASkySimSpectatorPawn(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	bAddDefaultMovementBindings = false;
	bUseControllerRotationPitch = true;
	bUseControllerRotationYaw = true;
	bUseControllerRotationRoll = false;

	GetCollisionComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	CameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("SkySimSpectatorCamera"));
	CameraComponent->SetupAttachment(RootComponent);
	CameraComponent->bUsePawnControlRotation = true;
	CameraComponent->SetFieldOfView(95.0f);

	if (USpectatorPawnMovement* SpectatorMovement = Cast<USpectatorPawnMovement>(GetMovementComponent()))
	{
		SpectatorMovement->MaxSpeed = MoveSpeed;
		SpectatorMovement->Acceleration = MovementAcceleration;
		SpectatorMovement->Deceleration = MovementDeceleration;
	}
}

void ASkySimSpectatorPawn::BeginPlay()
{
	Super::BeginPlay();
	SetActorEnableCollision(false);
	ConfigureLocalController();
}

void ASkySimSpectatorPawn::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	ConfigureLocalController();
}

void ASkySimSpectatorPawn::ConfigureLocalController()
{
	APlayerController* PlayerController = Cast<APlayerController>(GetController());
	if (!PlayerController || !PlayerController->IsLocalController())
	{
		return;
	}

	PlayerController->SetShowMouseCursor(false);
	PlayerController->SetInputMode(FInputModeGameOnly());
	PlayerController->SetIgnoreMoveInput(false);
	PlayerController->SetIgnoreLookInput(false);
	PlayerController->SetViewTarget(this);
	FRotator InitialRotation = GetActorRotation();
	if (!bInitialViewRotationApplied)
	{
		// GameMode spawning preserves PlayerStart yaw but clears its pitch. Restore
		// the authored skyward composition once without fighting later mouse look.
		InitialRotation.Pitch = FMath::Clamp(InitialViewPitchDegrees, -89.0f, 89.0f);
		InitialRotation.Roll = 0.0f;
		SetActorRotation(InitialRotation);
		bInitialViewRotationApplied = true;
	}
	PlayerController->SetControlRotation(InitialRotation);
}

void ASkySimSpectatorPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	APlayerController* PlayerController = Cast<APlayerController>(GetController());
	if (!PlayerController || !PlayerController->IsLocalController())
	{
		return;
	}

	float MouseX = 0.0f;
	float MouseY = 0.0f;
	PlayerController->GetInputMouseDelta(MouseX, MouseY);

	FRotator ViewRotation = PlayerController->GetControlRotation();
	ViewRotation.Yaw = FRotator::NormalizeAxis(ViewRotation.Yaw + MouseX * LookSensitivity);
	// Raw MouseY is positive when the cursor moves down. Match Unreal's
	// DefaultPawn convention so non-inverted controls look up when moving up.
	const float VerticalLookSign = bInvertMouseY ? 1.0f : -1.0f;
	ViewRotation.Pitch = FMath::Clamp(
		FRotator::NormalizeAxis(ViewRotation.Pitch) + MouseY * LookSensitivity * VerticalLookSign,
		-89.0f,
		89.0f);
	ViewRotation.Roll = 0.0f;
	PlayerController->SetControlRotation(ViewRotation);
	SetActorRotation(ViewRotation);

	const bool bBoosting = PlayerController->IsInputKeyDown(EKeys::LeftShift)
		|| PlayerController->IsInputKeyDown(EKeys::RightShift);
	if (USpectatorPawnMovement* SpectatorMovement = Cast<USpectatorPawnMovement>(GetMovementComponent()))
	{
		SpectatorMovement->MaxSpeed = MoveSpeed * (bBoosting ? BoostMultiplier : 1.0f);
		SpectatorMovement->Acceleration = MovementAcceleration * (bBoosting ? BoostMultiplier : 1.0f);
		SpectatorMovement->Deceleration = MovementDeceleration;
	}

	const float ForwardInput =
		(PlayerController->IsInputKeyDown(EKeys::W) ? 1.0f : 0.0f)
		- (PlayerController->IsInputKeyDown(EKeys::S) ? 1.0f : 0.0f);
	const float RightInput =
		(PlayerController->IsInputKeyDown(EKeys::D) ? 1.0f : 0.0f)
		- (PlayerController->IsInputKeyDown(EKeys::A) ? 1.0f : 0.0f);
	const float UpInput =
		(PlayerController->IsInputKeyDown(EKeys::E) ? 1.0f : 0.0f)
		- (PlayerController->IsInputKeyDown(EKeys::Q) ? 1.0f : 0.0f);

	const FRotationMatrix ViewMatrix(ViewRotation);
	AddMovementInput(ViewMatrix.GetUnitAxis(EAxis::X), ForwardInput);
	AddMovementInput(ViewMatrix.GetUnitAxis(EAxis::Y), RightInput);
	AddMovementInput(FVector::UpVector, UpInput);
}
