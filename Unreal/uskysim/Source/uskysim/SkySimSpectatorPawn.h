// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SpectatorPawn.h"
#include "SkySimSpectatorPawn.generated.h"

class UCameraComponent;

/**
 * Free-flight camera used by the SkySim preview map.
 *
 * The controls are intentionally read directly from the local player controller so the
 * preview keeps working without requiring an Enhanced Input mapping context asset.
 */
UCLASS(Blueprintable)
class USKYSIM_API ASkySimSpectatorPawn : public ASpectatorPawn
{
	GENERATED_BODY()

public:
	ASkySimSpectatorPawn(const FObjectInitializer& ObjectInitializer);

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void PossessedBy(AController* NewController) override;

	/** Normal free-flight speed in centimetres per second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SkySim|Spectator", meta = (ClampMin = "100.0", UIMin = "1000.0", UIMax = "300000.0"))
	float MoveSpeed = 100000.0f;

	/** Movement speed multiplier while either Shift key is held. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SkySim|Spectator", meta = (ClampMin = "1.0", UIMin = "1.0", UIMax = "10.0"))
	float BoostMultiplier = 4.0f;

	/** Mouse-look multiplier applied after Unreal's configured mouse axis sensitivity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SkySim|Spectator", meta = (ClampMin = "0.01", UIMin = "0.1", UIMax = "10.0"))
	float LookSensitivity = 2.5f;

	/** Initial upward view used by the wide-sky preview map. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SkySim|Spectator", meta = (ClampMin = "-89.0", ClampMax = "89.0", UIMin = "-45.0", UIMax = "45.0"))
	float InitialViewPitchDegrees = 14.0f;

	/** Reverses vertical mouse look for users who prefer flight-style controls. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SkySim|Spectator", meta = (DisplayName = "Invert Mouse Y"))
	bool bInvertMouseY = false;

	/** How quickly the free camera reaches its target speed. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SkySim|Spectator", meta = (ClampMin = "100.0", UIMin = "1000.0", UIMax = "2000000.0"))
	float MovementAcceleration = 500000.0f;

	/** How quickly the free camera stops after movement input is released. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SkySim|Spectator", meta = (ClampMin = "100.0", UIMin = "1000.0", UIMax = "2000000.0"))
	float MovementDeceleration = 700000.0f;

private:
	void ConfigureLocalController();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "SkySim|Spectator", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> CameraComponent;

	bool bInitialViewRotationApplied = false;
};
