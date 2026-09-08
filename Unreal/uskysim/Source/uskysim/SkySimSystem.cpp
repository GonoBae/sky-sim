#include "SkySimSystem.h"

#include "Common/UdpSocketBuilder.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/HeterogeneousVolumeComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "HAL/Platform.h"
#include "Engine/Engine.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "UObject/UObjectIterator.h"

namespace
{
	constexpr int32 SkyControlPacketBytes = 512;
	constexpr uint64 SkyApplyUtc = 0x000001ull;
	constexpr uint64 SkyApplyLocation = 0x000002ull;
	constexpr uint64 SkyApplyTimeScale = 0x000008ull;
	constexpr uint64 SkyApplyThermodynamics = 0x000020ull;
	constexpr uint64 SkyApplyVisibility = 0x000040ull;
	constexpr uint64 SkyApplyWind = 0x000080ull;
	constexpr uint64 SkyApplyPrecipitation = 0x000100ull;
	constexpr uint64 SkyApplyConvection = 0x000200ull;
	constexpr uint64 SkyApplyAllCloudLayers = 0x007800ull;
	constexpr uint64 SkyApplyAdvancedWeather = SkyApplyThermodynamics | SkyApplyVisibility |
		SkyApplyWind | SkyApplyPrecipitation | SkyApplyConvection;
	constexpr uint64 SkyReleasableWeatherMask = 0x007be0ull;
	constexpr uint32 SkyPresetCustom = 8u;
	constexpr double MinimumSkyUtcUnixSeconds = -2208988800.0;
	constexpr double MaximumSkyUtcUnixSeconds = 4133980800.0;

	uint16 ReadU16(const uint8* Data, int32 Offset)
	{
		return static_cast<uint16>(Data[Offset]) |
			(static_cast<uint16>(Data[Offset + 1]) << 8);
	}

	uint32 ReadU32(const uint8* Data, int32 Offset)
	{
		return static_cast<uint32>(Data[Offset]) |
			(static_cast<uint32>(Data[Offset + 1]) << 8) |
			(static_cast<uint32>(Data[Offset + 2]) << 16) |
			(static_cast<uint32>(Data[Offset + 3]) << 24);
	}

	void WriteU16(TArray<uint8>& Data, int32 Offset, uint16 Value)
	{
		Data[Offset] = static_cast<uint8>(Value);
		Data[Offset + 1] = static_cast<uint8>(Value >> 8);
	}

	void WriteU32(TArray<uint8>& Data, int32 Offset, uint32 Value)
	{
		for (int32 ByteIndex = 0; ByteIndex < 4; ++ByteIndex)
		{
			Data[Offset + ByteIndex] = static_cast<uint8>(Value >> (ByteIndex * 8));
		}
	}

	void WriteU64(TArray<uint8>& Data, int32 Offset, uint64 Value)
	{
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			Data[Offset + ByteIndex] = static_cast<uint8>(Value >> (ByteIndex * 8));
		}
	}

	void WriteF32(TArray<uint8>& Data, int32 Offset, float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Value));
		WriteU32(Data, Offset, Bits);
	}

	void WriteF64(TArray<uint8>& Data, int32 Offset, double Value)
	{
		uint64 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Value));
		WriteU64(Data, Offset, Bits);
	}

	float ReadF32(const uint8* Data, int32 Offset)
	{
		const uint32 Bits = ReadU32(Data, Offset);
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}

	double ReadF64(const uint8* Data, int32 Offset)
	{
		uint64 Bits = 0;
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			Bits |= static_cast<uint64>(Data[Offset + ByteIndex]) << (ByteIndex * 8);
		}
		double Value = 0.0;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}

	bool HasMagic(const uint8* Data, const ANSICHAR* Magic)
	{
		return FMemory::Memcmp(Data, Magic, 4) == 0;
	}

	double WrapDegrees(double Degrees)
	{
		Degrees = FMath::Fmod(Degrees, 360.0);
		return Degrees < 0.0 ? Degrees + 360.0 : Degrees;
	}

	uint8 CloudLayerWireKind(ESkySimCloudLayerType Type, float TopAltitudeAmslMeters)
	{
		switch (Type)
		{
		case ESkySimCloudLayerType::Stratiform:
		case ESkySimCloudLayerType::Fog:
			return 1;
		case ESkySimCloudLayerType::Convective:
			return TopAltitudeAmslMeters >= 8000.0f ? 3 : 2;
		case ESkySimCloudLayerType::Cirrus:
			return 4;
		}
		return 1;
	}

	FVector CalculateSunDirectionEnu(double UtcUnixSeconds, double LatitudeDegrees, double LongitudeDegrees)
	{
		const double DaysSinceJ2000 = UtcUnixSeconds / 86400.0 + 2440587.5 - 2451545.0;
		const double MeanLongitude = FMath::DegreesToRadians(WrapDegrees(280.460 + 0.9856474 * DaysSinceJ2000));
		const double MeanAnomaly = FMath::DegreesToRadians(WrapDegrees(357.528 + 0.9856003 * DaysSinceJ2000));
		const double EclipticLongitude = MeanLongitude + FMath::DegreesToRadians(1.915) * FMath::Sin(MeanAnomaly) +
			FMath::DegreesToRadians(0.020) * FMath::Sin(2.0 * MeanAnomaly);
		const double Obliquity = FMath::DegreesToRadians(23.439 - 0.0000004 * DaysSinceJ2000);
		const double RightAscension = FMath::Atan2(FMath::Cos(Obliquity) * FMath::Sin(EclipticLongitude), FMath::Cos(EclipticLongitude));
		const double Declination = FMath::Asin(FMath::Sin(Obliquity) * FMath::Sin(EclipticLongitude));
		const double SiderealDegrees = WrapDegrees(280.46061837 + 360.98564736629 * DaysSinceJ2000 + LongitudeDegrees);
		const double HourAngle = FMath::DegreesToRadians(WrapDegrees(SiderealDegrees - FMath::RadiansToDegrees(RightAscension) + 180.0) - 180.0);
		const double Latitude = FMath::DegreesToRadians(LatitudeDegrees);

		return FVector(
			-FMath::Cos(Declination) * FMath::Sin(HourAngle),
			FMath::Sin(Declination) * FMath::Cos(Latitude) - FMath::Cos(Declination) * FMath::Cos(HourAngle) * FMath::Sin(Latitude),
			FMath::Sin(Declination) * FMath::Sin(Latitude) + FMath::Cos(Declination) * FMath::Cos(HourAngle) * FMath::Cos(Latitude)).GetSafeNormal();
	}
}

ASkySimSystem::ASkySimSystem()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.0f;

	CustomCloudLayer1.bEnabled = true;
	CustomCloudLayer1.Type = ESkySimCloudLayerType::Stratiform;
	CustomCloudLayer1.BaseAltitudeAglMeters = 4200.0f;
	CustomCloudLayer1.TopAltitudeAglMeters = 6000.0f;
	CustomCloudLayer1.Coverage = 0.30f;
	CustomCloudLayer1.OpticalDepth = 3.5f;
	CustomCloudLayer1.ConvectiveActivity = 0.12f;
	CustomCloudLayer1.LiquidFraction = 0.9f;

	CustomCloudLayer2.bEnabled = true;
	CustomCloudLayer2.Type = ESkySimCloudLayerType::Cirrus;
	CustomCloudLayer2.BaseAltitudeAglMeters = 7200.0f;
	CustomCloudLayer2.TopAltitudeAglMeters = 10800.0f;
	CustomCloudLayer2.Coverage = 0.28f;
	CustomCloudLayer2.OpticalDepth = 2.2f;
	CustomCloudLayer2.ConvectiveActivity = 0.04f;
	CustomCloudLayer2.LiquidFraction = 0.12f;

	CustomCloudLayer3.bEnabled = false;
	CustomCloudLayer3.Type = ESkySimCloudLayerType::Convective;
	CustomCloudLayer3.BaseAltitudeAglMeters = 650.0f;
	CustomCloudLayer3.TopAltitudeAglMeters = 10500.0f;
	CustomCloudLayer3.Coverage = 0.42f;
	CustomCloudLayer3.OpticalDepth = 38.0f;
	CustomCloudLayer3.ConvectiveActivity = 0.92f;
	CustomCloudLayer3.LiquidFraction = 0.98f;
	CustomCloudLayer3.PrecipitationRateMmPerHour = 24.0f;

	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	SetRootComponent(SceneRoot);
	CloudVolumeComponent = CreateDefaultSubobject<UHeterogeneousVolumeComponent>(TEXT("CloudVolume"));
	CloudVolumeComponent->SetupAttachment(SceneRoot);
	CloudVolumeComponent->SetCastShadow(true);
	CloudVolumeComponent->bPivotAtCentroid = false;
	CloudVolumeComponent->StepFactor = 1.0f;
	CloudVolumeComponent->ShadowStepFactor = 2.0f;
	CloudVolumeComponent->LightingDownsampleFactor = 1.0f;
	CloudVolumeComponent->LightingDownsampleFactor = 1.0f;

	SunLightComponent = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("SkySimSunLight"));
	SunLightComponent->SetupAttachment(SceneRoot);
	SunLightComponent->SetMobility(EComponentMobility::Movable);
	SunLightComponent->SetIntensity(DefaultSunIlluminanceLux);
	SunLightComponent->SetRelativeRotation(FRotator(-35.0, -45.0, 0.0));
	SunLightComponent->SetCastShadows(true);
	SunLightComponent->SetVolumetricScatteringIntensity(1.0f);
	SunLightComponent->ForwardShadingPriority = 1;
	SunLightComponent->bAtmosphereSunLight = true;
	SunLightComponent->AtmosphereSunLightIndex = 0;
	SunLightComponent->bPerPixelAtmosphereTransmittance = true;

	MoonLightComponent = CreateDefaultSubobject<UDirectionalLightComponent>(TEXT("SkySimMoonLight"));
	MoonLightComponent->SetupAttachment(SceneRoot);
	MoonLightComponent->SetMobility(EComponentMobility::Movable);
	MoonLightComponent->SetIntensity(0.0f);
	MoonLightComponent->SetRelativeRotation(FRotator(35.0, 135.0, 0.0));
	MoonLightComponent->SetCastShadows(bMoonCastsShadows);
	MoonLightComponent->SetVolumetricScatteringIntensity(MoonVolumetricScatteringIntensity);
	MoonLightComponent->ForwardShadingPriority = 0;
	MoonLightComponent->bAtmosphereSunLight = true;
	MoonLightComponent->AtmosphereSunLightIndex = 1;
	MoonLightComponent->bPerPixelAtmosphereTransmittance = true;

	SkyAtmosphereComponent = CreateDefaultSubobject<USkyAtmosphereComponent>(TEXT("SkySimAtmosphere"));
	SkyAtmosphereComponent->SetupAttachment(SceneRoot);

	SkyLightComponent = CreateDefaultSubobject<USkyLightComponent>(TEXT("SkySimSkyLight"));
	SkyLightComponent->SetupAttachment(SceneRoot);
	SkyLightComponent->SetMobility(EComponentMobility::Movable);
	SkyLightComponent->SetIntensity(SkyLightIntensity);
	SkyLightComponent->bRealTimeCapture = true;

	WeatherFogComponent = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("SkySimWeatherFog"));
	WeatherFogComponent->SetupAttachment(SceneRoot);
	WeatherFogComponent->SetMobility(EComponentMobility::Movable);
	WeatherFogComponent->SetFogDensity(0.0f);
	WeatherFogComponent->SetFogHeightFalloff(FogHeightFalloff);
	WeatherFogComponent->SetFogMaxOpacity(FogMaximumOpacity);
	WeatherFogComponent->SetFogInscatteringColor(FogInscatteringTint);
	WeatherFogComponent->SetVolumetricFog(bEnableVolumetricWeatherFog);
	WeatherFogComponent->SetVolumetricFogExtinctionScale(VolumetricFogExtinctionScale);
	ReceiveBuffer.SetNumUninitialized(64 * 1024);
}

void ASkySimSystem::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	if (!bEditorPreviewClockInitialized)
	{
		ResetEditorPreviewClock();
	}
	RefreshEstimatedCloudSourceCount();
	UpdateEnvironmentLighting();
	UpdateVolumeRenderer();
}

void ASkySimSystem::BeginPlay()
{
	Super::BeginPlay();
	if (!bEditorPreviewClockInitialized)
	{
		ResetEditorPreviewClock();
	}

#if WITH_EDITOR
	// A Details edit can start the editor-world receiver so the volume preview
	// updates without PIE. Release those ports before the PIE copy starts its
	// authoritative receiver, then let the editor actor resume after PIE ends.
	if (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::PIE)
	{
		for (TObjectIterator<ASkySimSystem> It; It; ++It)
		{
			ASkySimSystem* EditorSystem = *It;
			if (EditorSystem != this && IsValid(EditorSystem) && EditorSystem->GetWorld() != nullptr &&
				EditorSystem->GetWorld()->WorldType == EWorldType::Editor &&
				EditorSystem->bIsReceiving)
			{
				if (EditorSystem->bRuntimeClockHasServerReference)
				{
					const double HandoffPlatformSeconds = FPlatformTime::Seconds();
					const bool bEditorHasFreshSkyState = EditorSystem->bHasSkyState &&
						EditorSystem->LastSkyStateReceivePlatformSeconds >= 0.0 &&
						HandoffPlatformSeconds - EditorSystem->LastSkyStateReceivePlatformSeconds <= 2.0;
					const double HandoffUtcUnixSeconds = EditorSystem->EvaluateRuntimeClockUtc(
						HandoffPlatformSeconds,
						EditorSystem->bPreviewInEditor &&
							(EditorSystem->bAnimateTimeInEditor || bEditorHasFreshSkyState));
					RebaseRuntimeClockFromServer(
						HandoffUtcUnixSeconds,
						EditorSystem->RuntimeClockTimeScale,
						EditorSystem->RuntimeClockLatitudeDegrees,
						EditorSystem->RuntimeClockLongitudeDegrees,
						HandoffPlatformSeconds);
					// PIE is taking over an already-authoritative editor session. Do not
					// resend the static authored start time and rewind the server.
					bAwaitingInitialServerSync = false;
				}
				EditorSystem->bEditorReceiverPausedForPIE = true;
				EditorSystem->StopReceiving();
				EditorSystem->CloseControlSocket();
			}
		}
	}
#endif

	UpdateEnvironmentLighting();
	UpdateVolumeRenderer();
	if (bAutoStart)
	{
		StartReceiving();
	}
	if (bSyncAuthoringSettingsOnConnect)
	{
		if (bAwaitingInitialServerSync)
		{
			ScheduleAuthoringSync();
		}
	}
	else if (ControlWeatherPreset == ESkySimWeatherPreset::Natural)
	{
		ApplyWeatherPreset();
	}
	else if (bAutoApplyCustomCumulus)
	{
		ApplyCloudLayerSettings();
	}
}

void ASkySimSystem::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	StopReceiving();
	CloseControlSocket();
	Super::EndPlay(EndPlayReason);
}

void ASkySimSystem::Destroyed()
{
	StopReceiving();
	CloseControlSocket();
	Super::Destroyed();
}

#if WITH_EDITOR
void ASkySimSystem::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	const FName MemberPropertyName = PropertyChangedEvent.GetMemberPropertyName();
	const bool bCustomCloudProperty =
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudCoverage) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudBaseAltitudeAglMeters) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudTopAltitudeAglMeters) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudOpticalDepth) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudConvectiveActivity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudLiquidFraction) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudPrecipitationRateMmPerHour) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, bCustomCloudLayer0Enabled) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudLayer0Type) ||
		MemberPropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudLayer1) ||
		MemberPropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudLayer2) ||
		MemberPropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudLayer3);
	const bool bTimeOrLocationProperty =
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, ControlLocalDateTime) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, UtcOffsetHours) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, ControlLatitudeDegrees) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, ControlLongitudeDegrees) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, ControlElevationMeters);
	const bool bTimeScaleProperty =
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, ControlTimeScale);
	const bool bWeatherPresetProperty =
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, ControlWeatherPreset) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, WeatherSeed);
	const bool bAdvancedWeatherProperty =
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomSurfaceTemperatureCelsius) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomSeaLevelPressureHpa) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomRelativeHumidity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomVisibilityMeters) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomAerosolOpticalDepth550Nm) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomOzoneDobsonUnits) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomMeanWindEnuMetersPerSecond) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomGustSpeedMetersPerSecond) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomPrecipitationRateMmPerHour) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomSnowFraction) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomSurfaceWetness) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomWeatherConvectiveActivity) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomLightningActivity);
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CustomCloudCoverage))
	{
		RefreshEstimatedCloudSourceCount();
	}
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, bAutoApplyCustomCumulus) &&
		!bAutoApplyCustomCumulus)
	{
		bCustomCumulusApplyScheduled = false;
	}
	else if ((bCustomCloudProperty ||
		 PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, bAutoApplyCustomCumulus)) &&
		bAutoApplyCustomCumulus)
	{
		// A Details slider can emit many interactive changes. Coalesce them into
		// one physical-server update after the user pauses for a quarter second.
		bCustomCumulusApplyScheduled = true;
		CustomCumulusApplyDuePlatformSeconds = FPlatformTime::Seconds() + 0.25;
	}
	if (bTimeOrLocationProperty)
	{
		ResetEditorPreviewClock();
		UpdatePreviewLightingFromControls();
		if (bAutoApplySkyControlsInEditor)
		{
			bSkyControlsApplyScheduled = true;
			SkyControlsApplyDuePlatformSeconds = FPlatformTime::Seconds() + 0.25;
		}
	}
	if (bTimeScaleProperty)
	{
		RebaseRuntimeClockTimeScale(ControlTimeScale);
		if (bAutoApplySkyControlsInEditor)
		{
			bTimeScaleApplyScheduled = true;
			TimeScaleApplyDuePlatformSeconds = FPlatformTime::Seconds() + 0.25;
		}
	}
	if (bWeatherPresetProperty && bAutoApplySkyControlsInEditor)
	{
		bWeatherPresetApplyScheduled = true;
		WeatherPresetApplyDuePlatformSeconds = FPlatformTime::Seconds() + 0.25;
		if (ControlWeatherPreset == ESkySimWeatherPreset::Natural)
		{
			bCustomWeatherApplyScheduled = false;
			bCustomCumulusApplyScheduled = false;
		}
	}
	if (bAdvancedWeatherProperty && bAutoApplySkyControlsInEditor)
	{
		bCustomWeatherApplyScheduled = true;
		CustomWeatherApplyDuePlatformSeconds = FPlatformTime::Seconds() + 0.25;
	}
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, bPreviewInEditor) && !bPreviewInEditor)
	{
		StopReceiving();
	}
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, bAutoApplySkyControlsInEditor) &&
		!bAutoApplySkyControlsInEditor)
	{
		bSkyControlsApplyScheduled = false;
		bTimeScaleApplyScheduled = false;
		bWeatherPresetApplyScheduled = false;
		bCustomWeatherApplyScheduled = false;
	}
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, bSyncAuthoringSettingsOnConnect) &&
		bSyncAuthoringSettingsOnConnect && bIsReceiving)
	{
		ScheduleAuthoringSync();
	}
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CloudSpreadIterations) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, CloudSpreadStrength) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, DensityShapePower) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, DensityPresentationGain) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, bStabilizeDensityScale) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, bUsePhysicalDensityScaleForRendering) ||
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, DensityReferenceScale))
	{
		RefreshDensityRendering();
	}
	UpdateEnvironmentLighting();
	UpdateVolumeRenderer();
}

bool ASkySimSystem::ShouldTickIfViewportsOnly() const
{
	return true;
}
#endif

void ASkySimSystem::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

#if WITH_EDITOR
	const bool bEditorWorld = GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::Editor;
	if (bEditorWorld &&
		bEditorReceiverPausedForPIE)
	{
		bool bPIESystemStillActive = false;
		for (TObjectIterator<ASkySimSystem> It; It; ++It)
		{
			const ASkySimSystem* Candidate = *It;
			if (Candidate != this && IsValid(Candidate) && Candidate->GetWorld() != nullptr &&
				Candidate->GetWorld()->WorldType == EWorldType::PIE)
			{
				bPIESystemStillActive = true;
				break;
			}
		}
		if (!bPIESystemStillActive)
		{
			bEditorReceiverPausedForPIE = false;
			if (bAutoStart)
			{
				StartReceiving();
			}
		}
	}
	if (bEditorWorld && bPreviewInEditor && bAutoStart && !bEditorReceiverPausedForPIE && !bIsReceiving)
	{
		const double Now = FPlatformTime::Seconds();
		if (Now >= NextEditorReceiverAttemptPlatformSeconds)
		{
			NextEditorReceiverAttemptPlatformSeconds = Now + 2.0;
			StartReceiving();
		}
	}
#endif

	// SKC1 acknowledgement tracking intentionally has one in-flight packet.
	// Serialize coalesced editor edits so simultaneous debounce expiry cannot
	// overwrite PendingControlPacket before the earlier command is acknowledged.
	if (PendingControlPacket.IsEmpty())
	{
		const double Now = FPlatformTime::Seconds();
		if (bSkyControlsApplyScheduled && Now >= SkyControlsApplyDuePlatformSeconds)
		{
			bSkyControlsApplyScheduled = false;
			const bool bQueuedTimeScaleApply = bTimeScaleApplyScheduled;
			bTimeScaleApplyScheduled = false;
			const bool bUsePreservedRuntimeUtc = bUseRuntimeUtcForScheduledSkyControls;
			const double PreservedRuntimeUtc = EvaluateRuntimeClockUtc(Now, true);
			const float PreservedRuntimeTimeScale = RuntimeClockTimeScale;
			bUseRuntimeUtcForScheduledSkyControls = false;
			if (bUsePreservedRuntimeUtc)
			{
				RebaseRuntimeClockFromServer(
					PreservedRuntimeUtc,
					PreservedRuntimeTimeScale,
					ControlLatitudeDegrees,
					ControlLongitudeDegrees,
					Now);
				SendSkyControlCommand(
					1,
					3,
					0,
					SkyPresetCustom,
					SkyApplyUtc | SkyApplyLocation | SkyApplyTimeScale,
					0,
					static_cast<uint32>(FMath::Max(1, WeatherSeed)),
					PreservedRuntimeUtc,
					ControlLatitudeDegrees,
					ControlLongitudeDegrees,
					ControlElevationMeters,
					PreservedRuntimeTimeScale,
					TEXT("Restoring continuous runtime date, location, and time scale"));
				if (!PendingControlPacket.IsEmpty())
				{
					RuntimeClockResyncControlSequence = PendingControlSequence;
					if (bQueuedTimeScaleApply)
					{
						bTimeScaleOverridePending = true;
						TimeScaleControlSequence = PendingControlSequence;
					}
				}
				else
				{
					bRuntimeClockResyncPending = false;
					RuntimeClockResyncControlSequence = 0;
				}
			}
			else
			{
				ApplyDateTimeAndLocation();
			}
		}
		else if (bTimeScaleApplyScheduled && Now >= TimeScaleApplyDuePlatformSeconds)
		{
			bTimeScaleApplyScheduled = false;
			ApplyTimeScale();
		}
		else if (bWeatherPresetApplyScheduled && Now >= WeatherPresetApplyDuePlatformSeconds)
		{
			bWeatherPresetApplyScheduled = false;
			ApplyWeatherPreset();
		}
		else if (bCustomWeatherApplyScheduled && Now >= CustomWeatherApplyDuePlatformSeconds)
		{
			bCustomWeatherApplyScheduled = false;
			ApplyCustomWeatherSettings();
		}
		else if (bCustomCumulusApplyScheduled && Now >= CustomCumulusApplyDuePlatformSeconds)
		{
			ApplyCloudLayerSettings();
		}
	}
	PumpControlRetry();
	if (bIsReceiving)
	{
		DrainSocket(SkyStateSocket, true);
		DrainSocket(VolumeSocket, false);
		VolumeReceiver.Prune(FPlatformTime::Seconds());
	}
	RefreshDensityRendering();
	UpdateRuntimeClockAndLighting();

	if (bShowDebugOverlay && GEngine != nullptr)
	{
		const FColor StatusColor = bHasSkyState ? FColor::Green : FColor::Yellow;
		const FString StatusText = FString::Printf(
			TEXT("SkySim UDP: %s | Sky=%lld | Volume packets=%lld | Complete=%lld | Frame=%d"),
			bHasSkyState ? TEXT("CONNECTED") : TEXT("WAITING"),
			ValidSkyPackets,
			ValidVolumePackets,
			CompleteVolumeFrames,
			LatestVolumeFrameId);
		GEngine->AddOnScreenDebugMessage(reinterpret_cast<uint64>(this), 0.0f, StatusColor, StatusText);
		const FString ClockText = FString::Printf(
			TEXT("SkySim Clock: %s | Local=%s | x%.3f | Sun=%.2f deg"),
			*RuntimeClockSource,
			*CurrentLocalDateTime.ToString(TEXT("%Y-%m-%d %H:%M:%S")),
			CurrentEffectiveTimeScale,
			CurrentSunElevationDegrees);
		GEngine->AddOnScreenDebugMessage(reinterpret_cast<uint64>(this) + 1, 0.0f, FColor::Cyan, ClockText);
	}
}

FSocket* ASkySimSystem::CreateBoundSocket(const TCHAR* DebugName, int32 Port) const
{
	return FUdpSocketBuilder(DebugName)
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(FIPv4Address::Any)
		.BoundToPort(Port)
		.WithReceiveBufferSize(4 * 1024 * 1024);
}

bool ASkySimSystem::StartReceiving()
{
	StopReceiving();

	SkyStateSocket = CreateBoundSocket(TEXT("SkySim-SKS1"), SkyStatePort);
	VolumeSocket = CreateBoundSocket(TEXT("SkySim-CLD2"), VolumePort);
	bIsReceiving = SkyStateSocket != nullptr && VolumeSocket != nullptr;

	if (!bIsReceiving)
	{
		UE_LOG(LogTemp, Error, TEXT("SkySim: UDP 포트 바인딩 실패 (CLD2=%d, SKS1=%d)"), VolumePort, SkyStatePort);
		StopReceiving();
		return false;
	}
	UE_LOG(LogTemp, Display, TEXT("SkySim: UDP 수신 시작 (CLD2=%d, SKS1=%d)"), VolumePort, SkyStatePort);
	return true;
}

void ASkySimSystem::StopReceiving()
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (SkyStateSocket != nullptr)
	{
		SkyStateSocket->Close();
		SocketSubsystem->DestroySocket(SkyStateSocket);
		SkyStateSocket = nullptr;
	}
	if (VolumeSocket != nullptr)
	{
		VolumeSocket->Close();
		SocketSubsystem->DestroySocket(VolumeSocket);
		VolumeSocket = nullptr;
	}
	bIsReceiving = false;
	bHasSkyState = false;
	LastSkyStateReceivePlatformSeconds = -1.0;
	VolumeReceiver.Reset();
	DensityFrames.BreakContinuity();
	// Keep the displayed pair available for offline presentation edits. The next
	// published frame starts a new segment through DensityFrames.BreakContinuity.
	FrozenCloudPlaybackSeconds = FPlatformTime::Seconds();
}

bool ASkySimSystem::EnsureControlSocket()
{
	if (SkyControlSocket != nullptr)
	{
		return true;
	}

	SkyControlSocket = FUdpSocketBuilder(TEXT("SkySim-SKC1-Control"))
		.AsNonBlocking()
		.AsReusable()
		.BoundToAddress(FIPv4Address::Any)
		.BoundToPort(0)
		.WithSendBufferSize(64 * 1024);
	if (SkyControlSocket == nullptr)
	{
		ControlStatus = ESkySimControlStatus::SendFailed;
		ControlStatusMessage = TEXT("Could not create the SKC1 control socket");
		UE_LOG(LogTemp, Error, TEXT("SkySim: SKC1 control socket creation failed"));
		return false;
	}

	const FGuid Guid = FGuid::NewGuid();
	ControlSessionId = Guid.A ^ Guid.B ^ Guid.C ^ Guid.D ^ static_cast<uint32>(FPlatformTime::Cycles());
	if (ControlSessionId == 0)
	{
		ControlSessionId = 1;
	}
	NextControlSequence = 0;
	return true;
}

void ASkySimSystem::CloseControlSocket()
{
	if (SkyControlSocket != nullptr)
	{
		SkyControlSocket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(SkyControlSocket);
		SkyControlSocket = nullptr;
	}
	PendingControlPacket.Reset();
	PendingControlSequence = 0;
	PendingControlSendAttempts = 0;
	ControlSessionId = 0;
	NextControlSequence = 0;
	bRuntimeClockResyncPending = false;
	RuntimeClockResyncControlSequence = 0;
	bTimeScaleOverridePending = false;
	TimeScaleControlSequence = 0;
}

bool ASkySimSystem::SendSkyControlCommand(
	uint8 Opcode,
	uint8 EvolutionMode,
	uint32 TransitionMilliseconds,
	uint32 Preset,
	uint64 ApplyMask,
	uint64 ClearMask,
	uint32 Seed,
	double UtcUnix,
	double Latitude,
	double Longitude,
	float Elevation,
	float TimeScale,
	const TCHAR* Description)
{
	if (!PendingControlPacket.IsEmpty())
	{
		ControlStatus = ESkySimControlStatus::Pending;
		ControlStatusMessage = TEXT("A previous sky command is still awaiting acknowledgement");
		return false;
	}
	const bool bUtcValid = (ApplyMask & SkyApplyUtc) == 0 ||
		(FMath::IsFinite(UtcUnix) && UtcUnix >= MinimumSkyUtcUnixSeconds && UtcUnix <= MaximumSkyUtcUnixSeconds);
	const bool bLocationValid = (ApplyMask & SkyApplyLocation) == 0 ||
		(FMath::IsFinite(Latitude) && FMath::IsFinite(Longitude) && FMath::IsFinite(Elevation) &&
		 Latitude >= -90.0 && Latitude <= 90.0 && Longitude >= -180.0 && Longitude <= 180.0 &&
		 Elevation >= -500.0f && Elevation <= 100000.0f);
	const bool bTimeScaleValid = (ApplyMask & SkyApplyTimeScale) == 0 ||
		(FMath::IsFinite(TimeScale) && TimeScale >= -86400.0f && TimeScale <= 86400.0f);
	if (!bUtcValid || !bLocationValid || !bTimeScaleValid || TransitionMilliseconds > 86400000u)
	{
		ControlStatus = ESkySimControlStatus::Rejected;
		ControlStatusMessage = TEXT("Control values are outside the SKC1 protocol range");
		UE_LOG(LogTemp, Warning, TEXT("SkySim: invalid SKC1 control values"));
		return false;
	}

	if (!bIsReceiving)
	{
		// CallInEditor buttons also open the receive ports so their ACK and new state
		// can be consumed while the level editor viewport is idle.
		StartReceiving();
	}
	if (!EnsureControlSocket())
	{
		return false;
	}

	++NextControlSequence;
	if (NextControlSequence == 0)
	{
		++NextControlSequence;
	}

	TArray<uint8> Packet;
	Packet.SetNumZeroed(SkyControlPacketBytes);
	FMemory::Memcpy(Packet.GetData(), "SKC1", 4);
	WriteU16(Packet, 4, 1);
	WriteU16(Packet, 6, SkyControlPacketBytes);
	WriteU32(Packet, 8, ControlSessionId);
	WriteU32(Packet, 12, NextControlSequence);
	WriteF64(Packet, 16, FMath::Max(0.0, FPlatformTime::Seconds()));
	Packet[24] = Opcode;
	Packet[25] = EvolutionMode;
	WriteU32(Packet, 28, TransitionMilliseconds);
	WriteU32(Packet, 36, Preset);
	WriteU64(Packet, 40, ApplyMask);
	WriteU64(Packet, 48, ClearMask);
	WriteU32(Packet, 56, Seed);
	if ((ApplyMask & SkyApplyUtc) != 0)
	{
		WriteF64(Packet, 64, UtcUnix);
	}
	if ((ApplyMask & SkyApplyLocation) != 0)
	{
		WriteF64(Packet, 72, Latitude);
		WriteF64(Packet, 80, Longitude);
		WriteF32(Packet, 88, Elevation);
	}
	if ((ApplyMask & SkyApplyTimeScale) != 0)
	{
		WriteF32(Packet, 92, TimeScale);
	}
	WriteU32(Packet, 508, FCrc::MemCrc32(Packet.GetData(), 508));

	PendingControlPacket = MoveTemp(Packet);
	PendingControlSequence = NextControlSequence;
	PendingControlSendAttempts = 0;
	ControlStatus = ESkySimControlStatus::Pending;
	ControlStatusMessage = FString::Printf(TEXT("%s (sequence %u)"), Description, PendingControlSequence);
	return SendPendingControlPacket();
}

bool ASkySimSystem::SendPendingControlPacket()
{
	if (PendingControlPacket.Num() != SkyControlPacketBytes || !EnsureControlSocket())
	{
		return false;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> Destination = SocketSubsystem->CreateInternetAddr();
	bool bAddressValid = false;
	Destination->SetIp(*SkyControlHost, bAddressValid);
	Destination->SetPort(SkyControlPort);
	if (!bAddressValid)
	{
		ControlStatus = ESkySimControlStatus::SendFailed;
		ControlStatusMessage = FString::Printf(TEXT("Invalid control IPv4 address: %s"), *SkyControlHost);
		return false;
	}

	int32 BytesSent = 0;
	const bool bSent = SkyControlSocket->SendTo(
		PendingControlPacket.GetData(), PendingControlPacket.Num(), BytesSent, *Destination);
	LastControlSendPlatformSeconds = FPlatformTime::Seconds();
	++PendingControlSendAttempts;
	if (!bSent || BytesSent != SkyControlPacketBytes)
	{
		ControlStatus = ESkySimControlStatus::SendFailed;
		ControlStatusMessage = FString::Printf(
			TEXT("SKC1 send failed (%d/%d bytes, attempt %d)"),
			BytesSent,
			SkyControlPacketBytes,
			PendingControlSendAttempts);
		UE_LOG(LogTemp, Warning, TEXT("SkySim: SKC1 send failed for sequence %u"), PendingControlSequence);
		return false;
	}

	ControlStatus = ESkySimControlStatus::Pending;
	UE_LOG(
		LogTemp,
		Display,
		TEXT("SkySim: SKC1 sent session=%u sequence=%u attempt=%d to %s:%d"),
		ControlSessionId,
		PendingControlSequence,
		PendingControlSendAttempts,
		*SkyControlHost,
		SkyControlPort);
	return true;
}

void ASkySimSystem::PumpControlRetry()
{
	if (PendingControlPacket.IsEmpty())
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	if (Now - LastControlSendPlatformSeconds < 0.5)
	{
		return;
	}
	if (PendingControlSendAttempts >= 6)
	{
		ControlStatus = ESkySimControlStatus::TimedOut;
		ControlStatusMessage = FString::Printf(
			TEXT("No SKS1 acknowledgement for sequence %u"), PendingControlSequence);
		UE_LOG(LogTemp, Warning, TEXT("SkySim: SKC1 acknowledgement timed out for sequence %u"), PendingControlSequence);
		PendingControlPacket.Reset();
		if (PendingControlSequence == RuntimeClockResyncControlSequence)
		{
			bRuntimeClockResyncPending = false;
			RuntimeClockResyncControlSequence = 0;
		}
		if (PendingControlSequence == TimeScaleControlSequence)
		{
			bTimeScaleOverridePending = false;
			TimeScaleControlSequence = 0;
		}
		PendingControlSequence = 0;
		PendingControlSendAttempts = 0;
		return;
	}
	SendPendingControlPacket();
}

void ASkySimSystem::HandleControlAcknowledgement(uint32 Session, uint32 Sequence, uint8 Result)
{
	LastControlSession = static_cast<int64>(Session);
	LastControlSequence = static_cast<int64>(Sequence);
	LastControlResult = Result;
	if (PendingControlPacket.IsEmpty() || Session != ControlSessionId || Sequence != PendingControlSequence)
	{
		return;
	}

	if (Result == 1)
	{
		ControlStatus = ESkySimControlStatus::Applied;
		ControlStatusMessage = FString::Printf(TEXT("Applied by server (sequence %u)"), Sequence);
		UE_LOG(LogTemp, Display, TEXT("SkySim: SKC1 applied, sequence=%u"), Sequence);
	}
	else if (Result == 2)
	{
		ControlStatus = ESkySimControlStatus::Rejected;
		ControlStatusMessage = FString::Printf(TEXT("Rejected by server (sequence %u)"), Sequence);
		UE_LOG(LogTemp, Warning, TEXT("SkySim: SKC1 rejected by server, sequence=%u"), Sequence);
	}
	else
	{
		return;
	}
	if (Sequence == RuntimeClockResyncControlSequence)
	{
		bRuntimeClockResyncPending = false;
		RuntimeClockResyncControlSequence = 0;
	}
	if (Sequence == TimeScaleControlSequence)
	{
		bTimeScaleOverridePending = false;
		TimeScaleControlSequence = 0;
	}

	PendingControlPacket.Reset();
	PendingControlSequence = 0;
	PendingControlSendAttempts = 0;
}

void ASkySimSystem::ApplyDateTimeAndLocation()
{
	bSkyControlsApplyScheduled = false;
	bUseRuntimeUtcForScheduledSkyControls = false;
	const bool bRuntimeResyncIsInFlight = bRuntimeClockResyncPending &&
		!PendingControlPacket.IsEmpty() &&
		PendingControlSequence == RuntimeClockResyncControlSequence;
	if (!bRuntimeResyncIsInFlight)
	{
		bRuntimeClockResyncPending = false;
		RuntimeClockResyncControlSequence = 0;
	}
	bTimeScaleApplyScheduled = false;
	NextRuntimeClockDiagnosticPlatformSeconds = FPlatformTime::Seconds() + 2.0;
	if (!FMath::IsFinite(UtcOffsetHours) || UtcOffsetHours < -14.0f || UtcOffsetHours > 14.0f ||
		ControlLocalDateTime.GetYear() < 1900 || ControlLocalDateTime.GetYear() > 2100)
	{
		ControlStatus = ESkySimControlStatus::Rejected;
		ControlStatusMessage = TEXT("Local date/time or UTC offset is outside the supported range");
		return;
	}
	const FDateTime UtcDateTime = ControlLocalDateTime - FTimespan::FromHours(UtcOffsetHours);
	const double ControlUtcUnix = static_cast<double>(UtcDateTime.ToUnixTimestamp());
	ResetEditorPreviewClock();
	UpdatePreviewLightingFromControls();
	if (!PendingControlPacket.IsEmpty())
	{
		bSkyControlsApplyScheduled = true;
		SkyControlsApplyDuePlatformSeconds = FPlatformTime::Seconds();
		ControlStatus = ESkySimControlStatus::Pending;
		ControlStatusMessage = TEXT("Date, location, and time scale queued behind the current sky command");
		return;
	}
	SendSkyControlCommand(
		1,
		3,
		0,
		SkyPresetCustom,
		SkyApplyUtc | SkyApplyLocation | SkyApplyTimeScale,
		0,
		static_cast<uint32>(FMath::Max(1, WeatherSeed)),
		ControlUtcUnix,
		ControlLatitudeDegrees,
		ControlLongitudeDegrees,
		ControlElevationMeters,
		ControlTimeScale,
		TEXT("Applying date, location, and time scale"));
	if (!PendingControlPacket.IsEmpty())
	{
		bTimeScaleOverridePending = true;
		TimeScaleControlSequence = PendingControlSequence;
	}
}

void ASkySimSystem::ApplyTimeScale()
{
	if (!FMath::IsNearlyZero(ControlTimeScale))
	{
		LastNonZeroTimeScale = ControlTimeScale;
	}
	if (FMath::IsFinite(ControlTimeScale) && ControlTimeScale >= -86400.0f && ControlTimeScale <= 86400.0f)
	{
		RebaseRuntimeClockTimeScale(ControlTimeScale);
	}
	if (!PendingControlPacket.IsEmpty())
	{
		// The control channel is intentionally single-flight. Preserve the
		// newest requested scale and send it immediately after the current ACK.
		bTimeScaleApplyScheduled = true;
		TimeScaleApplyDuePlatformSeconds = FPlatformTime::Seconds();
		ControlStatus = ESkySimControlStatus::Pending;
		ControlStatusMessage = TEXT("Time-scale change queued behind the current sky command");
		return;
	}

	bTimeScaleApplyScheduled = false;
	SendSkyControlCommand(
		1,
		3,
		0,
		SkyPresetCustom,
		SkyApplyTimeScale,
		0,
		static_cast<uint32>(FMath::Max(1, WeatherSeed)),
		0.0,
		0.0,
		0.0,
		0.0f,
		ControlTimeScale,
		TEXT("Applying sky time scale"));
	if (!PendingControlPacket.IsEmpty())
	{
		bTimeScaleOverridePending = true;
		TimeScaleControlSequence = PendingControlSequence;
	}
}

void ASkySimSystem::PauseSkyTime()
{
	if (!FMath::IsNearlyZero(ControlTimeScale))
	{
		LastNonZeroTimeScale = ControlTimeScale;
	}
	ControlTimeScale = 0.0f;
	ApplyTimeScale();
}

void ASkySimSystem::ResumeSkyTime()
{
	if (FMath::IsNearlyZero(ControlTimeScale))
	{
		ControlTimeScale = FMath::IsNearlyZero(LastNonZeroTimeScale) ? 1.0f : LastNonZeroTimeScale;
	}
	ApplyTimeScale();
}

void ASkySimSystem::ApplyWeatherPreset()
{
	bWeatherPresetApplyScheduled = false;
	const uint32 Preset = static_cast<uint32>(ControlWeatherPreset);
	const uint8 EvolutionMode = ControlWeatherPreset == ESkySimWeatherPreset::Natural ? 1 : 3;
	const uint32 TransitionMilliseconds = static_cast<uint32>(FMath::RoundToInt(
		FMath::Clamp(WeatherTransitionSeconds, 0.0f, 86400.0f) * 1000.0f));
	SendSkyControlCommand(
		3,
		EvolutionMode,
		TransitionMilliseconds,
		Preset,
		0,
		0,
		static_cast<uint32>(FMath::Max(1, WeatherSeed)),
		0.0,
		0.0,
		0.0,
		0.0f,
		0.0f,
		TEXT("Loading weather preset"));
}

void ASkySimSystem::ReleaseWeatherToNatural()
{
	SendSkyControlCommand(
		2,
		1,
		static_cast<uint32>(FMath::RoundToInt(FMath::Clamp(WeatherTransitionSeconds, 0.0f, 86400.0f) * 1000.0f)),
		0,
		0,
		SkyReleasableWeatherMask,
		static_cast<uint32>(FMath::Max(1, WeatherSeed)),
		0.0,
		0.0,
		0.0,
		0.0f,
		0.0f,
		TEXT("Releasing weather to natural evolution"));
}

void ASkySimSystem::ApplyCustomWeatherSettings()
{
	bCustomWeatherApplyScheduled = false;
	SendCustomWeatherControl();
}

bool ASkySimSystem::SendCustomWeatherControl()
{
	if (!PendingControlPacket.IsEmpty())
	{
		ControlStatus = ESkySimControlStatus::Pending;
		ControlStatusMessage = TEXT("A previous sky command is still awaiting acknowledgement");
		return false;
	}
	const float TemperatureKelvin = CustomSurfaceTemperatureCelsius + 273.15f;
	const float PressurePascal = CustomSeaLevelPressureHpa * 100.0f;
	const float MeanWindSpeed = static_cast<float>(CustomMeanWindEnuMetersPerSecond.Size());
	const bool bFinite = FMath::IsFinite(TemperatureKelvin) && FMath::IsFinite(PressurePascal) &&
		FMath::IsFinite(CustomRelativeHumidity) && FMath::IsFinite(CustomVisibilityMeters) &&
		FMath::IsFinite(CustomAerosolOpticalDepth550Nm) && FMath::IsFinite(CustomOzoneDobsonUnits) &&
		FMath::IsFinite(CustomMeanWindEnuMetersPerSecond.X) && FMath::IsFinite(CustomMeanWindEnuMetersPerSecond.Y) &&
		FMath::IsFinite(CustomMeanWindEnuMetersPerSecond.Z) && FMath::IsFinite(CustomGustSpeedMetersPerSecond) &&
		FMath::IsFinite(CustomPrecipitationRateMmPerHour) && FMath::IsFinite(CustomSnowFraction) &&
		FMath::IsFinite(CustomSurfaceWetness) && FMath::IsFinite(CustomWeatherConvectiveActivity) &&
		FMath::IsFinite(CustomLightningActivity);
	const bool bInRange = TemperatureKelvin >= 203.15f && TemperatureKelvin <= 333.15f &&
		PressurePascal >= 80000.0f && PressurePascal <= 108000.0f &&
		CustomRelativeHumidity >= 0.01f && CustomRelativeHumidity <= 1.0f &&
		CustomVisibilityMeters >= 25.0f && CustomVisibilityMeters <= 200000.0f &&
		CustomAerosolOpticalDepth550Nm >= 0.005f && CustomAerosolOpticalDepth550Nm <= 3.0f &&
		CustomOzoneDobsonUnits >= 100.0f && CustomOzoneDobsonUnits <= 600.0f &&
		MeanWindSpeed <= 150.0f && CustomGustSpeedMetersPerSecond >= MeanWindSpeed &&
		CustomGustSpeedMetersPerSecond <= 200.0f &&
		CustomPrecipitationRateMmPerHour >= 0.0f && CustomPrecipitationRateMmPerHour <= 300.0f &&
		CustomSnowFraction >= 0.0f && CustomSnowFraction <= 1.0f &&
		CustomSurfaceWetness >= 0.0f && CustomSurfaceWetness <= 1.0f &&
		CustomWeatherConvectiveActivity >= 0.0f && CustomWeatherConvectiveActivity <= 1.0f &&
		CustomLightningActivity >= 0.0f && CustomLightningActivity <= 1.0f;
	if (!bFinite || !bInRange)
	{
		ControlStatus = ESkySimControlStatus::Rejected;
		ControlStatusMessage = TEXT("Advanced weather values are outside the SKC1 protocol range");
		UE_LOG(LogTemp, Warning, TEXT("SkySim: invalid advanced weather settings"));
		return false;
	}

	if (!bIsReceiving)
	{
		StartReceiving();
	}
	if (!EnsureControlSocket())
	{
		return false;
	}
	++NextControlSequence;
	if (NextControlSequence == 0)
	{
		++NextControlSequence;
	}

	TArray<uint8> Packet;
	Packet.SetNumZeroed(SkyControlPacketBytes);
	FMemory::Memcpy(Packet.GetData(), "SKC1", 4);
	WriteU16(Packet, 4, 1);
	WriteU16(Packet, 6, SkyControlPacketBytes);
	WriteU32(Packet, 8, ControlSessionId);
	WriteU32(Packet, 12, NextControlSequence);
	WriteF64(Packet, 16, FMath::Max(0.0, FPlatformTime::Seconds()));
	Packet[24] = 1; // PatchOverride
	Packet[25] = 3; // Manual evolution
	WriteU32(Packet, 28, static_cast<uint32>(FMath::RoundToInt(
		FMath::Clamp(WeatherTransitionSeconds, 0.0f, 86400.0f) * 1000.0f)));
	WriteU32(Packet, 36, SkyPresetCustom);
	WriteU64(Packet, 40, SkyApplyAdvancedWeather);
	WriteU32(Packet, 56, static_cast<uint32>(FMath::Max(1, WeatherSeed)));

	WriteF32(Packet, 108, TemperatureKelvin);
	WriteF32(Packet, 112, PressurePascal);
	WriteF32(Packet, 116, CustomRelativeHumidity);
	WriteF32(Packet, 120, CustomVisibilityMeters);
	WriteF32(Packet, 124, CustomAerosolOpticalDepth550Nm);
	WriteF32(Packet, 128, CustomOzoneDobsonUnits);
	WriteF32(Packet, 132, static_cast<float>(CustomMeanWindEnuMetersPerSecond.X));
	WriteF32(Packet, 136, static_cast<float>(CustomMeanWindEnuMetersPerSecond.Y));
	WriteF32(Packet, 140, static_cast<float>(CustomMeanWindEnuMetersPerSecond.Z));
	const float GustDelta = FMath::Max(0.0f, CustomGustSpeedMetersPerSecond - MeanWindSpeed);
	const FVector GustDeltaEnu = MeanWindSpeed > 0.0001f
		? CustomMeanWindEnuMetersPerSecond / MeanWindSpeed * GustDelta
		: FVector(GustDelta, 0.0, 0.0);
	WriteF32(Packet, 144, static_cast<float>(GustDeltaEnu.X));
	WriteF32(Packet, 148, static_cast<float>(GustDeltaEnu.Y));
	WriteF32(Packet, 152, static_cast<float>(GustDeltaEnu.Z));
	WriteF32(Packet, 160, CustomPrecipitationRateMmPerHour / 3600.0f);
	WriteF32(Packet, 164, 1.0f - CustomSnowFraction);
	WriteF32(Packet, 168, CustomSnowFraction);
	WriteF32(Packet, 176, CustomSurfaceWetness);
	WriteF32(Packet, 184, CustomWeatherConvectiveActivity * 3000.0f);
	WriteF32(Packet, 192, CustomLightningActivity * 0.25f);
	WriteU32(Packet, 508, FCrc::MemCrc32(Packet.GetData(), 508));

	PendingControlPacket = MoveTemp(Packet);
	PendingControlSequence = NextControlSequence;
	PendingControlSendAttempts = 0;
	ControlStatus = ESkySimControlStatus::Pending;
	ControlStatusMessage = FString::Printf(
		TEXT("Applying advanced weather (sequence %u)"), PendingControlSequence);
	return SendPendingControlPacket();
}

void ASkySimSystem::RefreshEstimatedCloudSourceCount()
{
	const float Coverage = FMath::Clamp(CustomCloudCoverage, 0.0f, 1.0f);
	EstimatedCloudSourceCount = FMath::Clamp(FMath::CeilToInt(Coverage * 20.0f), 4, 20);
}

void ASkySimSystem::ScheduleAuthoringSync(bool bPreserveRuntimeUtc, double PreservedUtcUnixSeconds)
{
	const double DueTime = FPlatformTime::Seconds() + 0.05;
	bAwaitingInitialServerSync = false;
	bSkyControlsApplyScheduled = true;
	SkyControlsApplyDuePlatformSeconds = DueTime;
	bUseRuntimeUtcForScheduledSkyControls = bPreserveRuntimeUtc &&
		FMath::IsFinite(PreservedUtcUnixSeconds) &&
		PreservedUtcUnixSeconds >= MinimumSkyUtcUnixSeconds &&
		PreservedUtcUnixSeconds <= MaximumSkyUtcUnixSeconds;
	bRuntimeClockResyncPending = bUseRuntimeUtcForScheduledSkyControls;
	RuntimeClockResyncControlSequence = 0;
	// Do not compare the pre-authoring server epoch with the deliberately
	// authored epoch in progression diagnostics.
	NextRuntimeClockDiagnosticPlatformSeconds = FMath::Max(
		NextRuntimeClockDiagnosticPlatformSeconds,
		DueTime + 2.0);
	bTimeScaleApplyScheduled = false;
	bWeatherPresetApplyScheduled = true;
	WeatherPresetApplyDuePlatformSeconds = DueTime;
	bCustomWeatherApplyScheduled = bApplyAdvancedWeatherOnConnect;
	CustomWeatherApplyDuePlatformSeconds = DueTime;
	const bool bAuthoredLayersAreActive =
		ControlWeatherPreset != ESkySimWeatherPreset::Natural && bAutoApplyCustomCumulus;
	bCustomCumulusApplyScheduled = bAuthoredLayersAreActive;
	CustomCumulusApplyDuePlatformSeconds = DueTime;
}

void ASkySimSystem::ApplyCustomCumulusSettings()
{
	ApplyCloudLayerSettings();
}

void ASkySimSystem::ApplyCloudLayerSettings()
{
	bCustomCumulusApplyScheduled = false;
	RefreshEstimatedCloudSourceCount();
	SendCloudLayerControl();
}

bool ASkySimSystem::SendCloudLayerControl()
{
	if (!PendingControlPacket.IsEmpty())
	{
		ControlStatus = ESkySimControlStatus::Pending;
		ControlStatusMessage = TEXT("A previous sky command is still awaiting acknowledgement");
		return false;
	}
	FSkySimCloudLayerSettings AuthoredCloudLayers[4];
	AuthoredCloudLayers[0].bEnabled = bCustomCloudLayer0Enabled;
	AuthoredCloudLayers[0].Type = CustomCloudLayer0Type;
	AuthoredCloudLayers[0].BaseAltitudeAglMeters = CustomCloudBaseAltitudeAglMeters;
	AuthoredCloudLayers[0].TopAltitudeAglMeters = CustomCloudTopAltitudeAglMeters;
	AuthoredCloudLayers[0].Coverage = CustomCloudCoverage;
	AuthoredCloudLayers[0].OpticalDepth = CustomCloudOpticalDepth;
	AuthoredCloudLayers[0].ConvectiveActivity = CustomCloudConvectiveActivity;
	AuthoredCloudLayers[0].LiquidFraction = CustomCloudLiquidFraction;
	AuthoredCloudLayers[0].PrecipitationRateMmPerHour = CustomCloudPrecipitationRateMmPerHour;
	AuthoredCloudLayers[1] = CustomCloudLayer1;
	AuthoredCloudLayers[2] = CustomCloudLayer2;
	AuthoredCloudLayers[3] = CustomCloudLayer3;

	// Once an SKS1 state has arrived, its elevation is authoritative. Before
	// that first state, fall back to the location value currently authored in
	// Details so an offline/editor-only control send remains possible.
	const float ElevationReferenceMeters = bHasSkyState
		? ServerElevationMeters
		: ControlElevationMeters;
	if (!FMath::IsFinite(ElevationReferenceMeters))
	{
		ControlStatus = ESkySimControlStatus::Rejected;
		ControlStatusMessage = TEXT("The cloud-layer elevation reference is not finite");
		return false;
	}
	for (FSkySimCloudLayerSettings& Layer : AuthoredCloudLayers)
	{
		if (Layer.bEnabled)
		{
			continue;
		}
		// A disabled slot is a protocol transport detail, not authored weather.
		// Normalize it to a tiny valid zero-effect deck so invalid hidden values
		// never prevent the user from turning a problematic layer off.
		const float MaximumTopAgl = 100000.0f - ElevationReferenceMeters;
		if (MaximumTopAgl < 1.0f)
		{
			ControlStatus = ESkySimControlStatus::Rejected;
			ControlStatusMessage = TEXT("Elevation leaves no valid SKC1 cloud-layer altitude range");
			return false;
		}
		const float RequestedBase = FMath::IsFinite(Layer.BaseAltitudeAglMeters)
			? Layer.BaseAltitudeAglMeters
			: 0.0f;
		const float RequestedTop = FMath::IsFinite(Layer.TopAltitudeAglMeters)
			? Layer.TopAltitudeAglMeters
			: RequestedBase + 1.0f;
		Layer.BaseAltitudeAglMeters = FMath::Clamp(RequestedBase, 0.0f, MaximumTopAgl - 1.0f);
		Layer.TopAltitudeAglMeters = FMath::Clamp(
			RequestedTop, Layer.BaseAltitudeAglMeters + 1.0f, MaximumTopAgl);
		Layer.Type = ESkySimCloudLayerType::Stratiform;
		Layer.Coverage = 0.0f;
		Layer.OpticalDepth = 0.0f;
		Layer.ConvectiveActivity = 0.0f;
		Layer.LiquidFraction = 1.0f;
		Layer.PrecipitationRateMmPerHour = 0.0f;
	}
	for (int32 LayerIndex = 0; LayerIndex < UE_ARRAY_COUNT(AuthoredCloudLayers); ++LayerIndex)
	{
		const FSkySimCloudLayerSettings& Layer = AuthoredCloudLayers[LayerIndex];
		const float Thickness = Layer.TopAltitudeAglMeters - Layer.BaseAltitudeAglMeters;
		const float BaseAmsl = Layer.BaseAltitudeAglMeters + ElevationReferenceMeters;
		const float TopAmsl = Layer.TopAltitudeAglMeters + ElevationReferenceMeters;
		const bool bFinite = FMath::IsFinite(Layer.BaseAltitudeAglMeters) &&
			FMath::IsFinite(Layer.TopAltitudeAglMeters) && FMath::IsFinite(Layer.Coverage) &&
			FMath::IsFinite(Layer.OpticalDepth) && FMath::IsFinite(Layer.ConvectiveActivity) &&
			FMath::IsFinite(Layer.LiquidFraction) && FMath::IsFinite(Layer.PrecipitationRateMmPerHour);
		const bool bInRange = Layer.BaseAltitudeAglMeters >= 0.0f && Thickness >= 1.0f &&
			BaseAmsl >= -500.0f && TopAmsl <= 100000.0f && Layer.Coverage >= 0.0f &&
			Layer.Coverage <= 1.0f && Layer.OpticalDepth >= 0.0f && Layer.OpticalDepth <= 500.0f &&
			Layer.OpticalDepth <= Thickness && Layer.ConvectiveActivity >= 0.0f &&
			Layer.ConvectiveActivity <= 1.0f && Layer.LiquidFraction >= 0.0f &&
			Layer.LiquidFraction <= 1.0f && Layer.PrecipitationRateMmPerHour >= 0.0f &&
			Layer.PrecipitationRateMmPerHour <= 300.0f &&
			(!Layer.bEnabled || !bHasSkyState || Layer.TopAltitudeAglMeters <= ServerDomainExtentMeters.Z);
		const bool bFogRangeValid = !Layer.bEnabled || Layer.Type != ESkySimCloudLayerType::Fog ||
			(Layer.BaseAltitudeAglMeters <= 5.0f && Layer.TopAltitudeAglMeters <= 1000.0f);
		if (!bFinite || !bInRange || !bFogRangeValid)
		{
			ControlStatus = ESkySimControlStatus::Rejected;
			ControlStatusMessage = FString::Printf(
				TEXT("Cloud layer %d is outside the SKC1/live domain range (fog must be 0-1000 m AGL)"), LayerIndex);
			UE_LOG(LogTemp, Warning, TEXT("SkySim: invalid cloud layer %d"), LayerIndex);
			return false;
		}
	}

	if (!bIsReceiving)
	{
		StartReceiving();
	}
	if (!EnsureControlSocket())
	{
		return false;
	}

	++NextControlSequence;
	if (NextControlSequence == 0)
	{
		++NextControlSequence;
	}

	TArray<uint8> Packet;
	Packet.SetNumZeroed(SkyControlPacketBytes);
	FMemory::Memcpy(Packet.GetData(), "SKC1", 4);
	WriteU16(Packet, 4, 1);
	WriteU16(Packet, 6, SkyControlPacketBytes);
	WriteU32(Packet, 8, ControlSessionId);
	WriteU32(Packet, 12, NextControlSequence);
	WriteF64(Packet, 16, FMath::Max(0.0, FPlatformTime::Seconds()));
	Packet[24] = 1; // PatchOverride
	Packet[25] = 3; // Manual evolution
	WriteU32(Packet, 28, static_cast<uint32>(FMath::RoundToInt(
		FMath::Clamp(WeatherTransitionSeconds, 0.0f, 86400.0f) * 1000.0f)));
	WriteU32(Packet, 36, SkyPresetCustom);
	// Always write all slots. Disabled slots remain valid zero-effect layers,
	// avoiding stale/ghost decks when a previous setup used more layers.
	WriteU64(Packet, 40, SkyApplyAllCloudLayers);
	WriteU32(Packet, 56, static_cast<uint32>(FMath::Max(1, WeatherSeed)));

	for (int32 LayerIndex = 0; LayerIndex < UE_ARRAY_COUNT(AuthoredCloudLayers); ++LayerIndex)
	{
		const FSkySimCloudLayerSettings& Layer = AuthoredCloudLayers[LayerIndex];
		const int32 Offset = 224 + LayerIndex * 32;
		const float BaseAmsl = Layer.BaseAltitudeAglMeters + ElevationReferenceMeters;
		const float TopAmsl = Layer.TopAltitudeAglMeters + ElevationReferenceMeters;
		const float Thickness = Layer.TopAltitudeAglMeters - Layer.BaseAltitudeAglMeters;
		const float Coverage = Layer.bEnabled ? Layer.Coverage : 0.0f;
		const float OpticalDepth = Layer.bEnabled ? Layer.OpticalDepth : 0.0f;
		const float ConvectiveActivity = Layer.bEnabled ? Layer.ConvectiveActivity : 0.0f;
		const float PrecipitationRate = Layer.bEnabled ? Layer.PrecipitationRateMmPerHour : 0.0f;
		const ESkySimCloudLayerType Type = Layer.bEnabled
			? Layer.Type
			: ESkySimCloudLayerType::Stratiform;
		WriteF32(Packet, Offset, BaseAmsl);
		WriteF32(Packet, Offset + 4, TopAmsl);
		WriteF32(Packet, Offset + 8, Coverage);
		WriteF32(Packet, Offset + 12, OpticalDepth / (100.0f * Thickness));
		WriteF32(Packet, Offset + 16, 1.0f - Layer.LiquidFraction);
		WriteF32(Packet, Offset + 20, PrecipitationRate / 3600.0f);
		WriteF32(Packet, Offset + 24, 0.25f + 4.0f * ConvectiveActivity);
		Packet[Offset + 28] = CloudLayerWireKind(Type, TopAmsl);
		Packet[Offset + 29] = static_cast<uint8>(
			(ConvectiveActivity > 0.35f ? 1u : 0u) |
			(PrecipitationRate > 0.001f ? 2u : 0u));
	}
	WriteU32(Packet, 508, FCrc::MemCrc32(Packet.GetData(), 508));

	PendingControlPacket = MoveTemp(Packet);
	PendingControlSequence = NextControlSequence;
	PendingControlSendAttempts = 0;
	ControlStatus = ESkySimControlStatus::Pending;
	ControlStatusMessage = FString::Printf(
		TEXT("Applying four cloud layers (sequence %u)"), PendingControlSequence);
	return SendPendingControlPacket();
}

void ASkySimSystem::RequestSkyKeyframe()
{
	SendSkyControlCommand(
		4,
		3,
		0,
		0,
		0,
		0,
		0,
		0.0,
		0.0,
		0.0,
		0.0f,
		0.0f,
		TEXT("Requesting an immediate sky keyframe"));
}

void ASkySimSystem::DrainSocket(FSocket* Socket, bool bSkyState)
{
	if (Socket == nullptr)
	{
		return;
	}

	uint32 PendingBytes = 0;
	while (Socket->HasPendingData(PendingBytes))
	{
		const int32 DatagramCapacity = FMath::Min<int32>(ReceiveBuffer.Num(), static_cast<int32>(PendingBytes));
		int32 BytesRead = 0;
		if (!Socket->Recv(ReceiveBuffer.GetData(), DatagramCapacity, BytesRead) || BytesRead <= 0)
		{
			break;
		}

		if (bSkyState)
		{
			ParseSkyState(ReceiveBuffer.GetData(), BytesRead);
		}
		else
		{
			ParseVolumePacket(ReceiveBuffer.GetData(), BytesRead);
		}
	}
}

bool ASkySimSystem::ParseSkyState(const uint8* Data, int32 NumBytes)
{
	if (NumBytes != 512 || !HasMagic(Data, "SKS1") || ReadU16(Data, 4) != 1 || ReadU16(Data, 6) != 512)
	{
		return false;
	}
	if (FCrc::MemCrc32(Data, 508) != ReadU32(Data, 508))
	{
		return false;
	}

	const uint32 NewControlSequence = ReadU32(Data, 12);
	const uint32 NewControlSession = ReadU32(Data, 16);
	const uint32 NewSkyStateSequence = ReadU32(Data, 8);
	const uint8 NewControlResult = Data[49];
	const uint8 NewCloudLayerCount = Data[50];
	const double NewUtcUnixSeconds = ReadF64(Data, 64);
	const double NewLatitudeDegrees = ReadF64(Data, 72);
	const double NewLongitudeDegrees = ReadF64(Data, 80);
	const float NewElevationMeters = ReadF32(Data, 88);
	const float NewTimeScale = ReadF32(Data, 92);
	const FVector NewDomainExtentMeters(ReadF32(Data, 96), ReadF32(Data, 100), ReadF32(Data, 104));
	const float NewRelativeHumidity = ReadF32(Data, 116);
	const float NewVisibilityMeters = ReadF32(Data, 120);
	const FVector NewMeanWind(ReadF32(Data, 132), ReadF32(Data, 136), ReadF32(Data, 140));
	const FVector NewSun(ReadF32(Data, 352), ReadF32(Data, 356), ReadF32(Data, 360));
	const float NewSunIlluminanceLux = ReadF32(Data, 372);
	const FVector NewMoon(ReadF32(Data, 376), ReadF32(Data, 380), ReadF32(Data, 384));
	const float NewMoonIlluminatedFraction = ReadF32(Data, 392);
	const float NewMoonIlluminanceLux = ReadF32(Data, 396);
	const double ReceivePlatformSeconds = FPlatformTime::Seconds();
	const bool bCanPreserveRuntimeUtc = bEditorPreviewClockInitialized && bRuntimeClockHasServerReference;
	const double RuntimeUtcBeforePacket = bCanPreserveRuntimeUtc
		? EvaluateRuntimeClockUtc(ReceivePlatformSeconds, true)
		: NewUtcUnixSeconds;
	const float RuntimeTimeScaleBeforePacket = RuntimeClockTimeScale;
	const double RuntimeLatitudeBeforePacket = RuntimeClockLatitudeDegrees;
	const double RuntimeLongitudeBeforePacket = RuntimeClockLongitudeDegrees;
	const uint32 PreviousSkyStateSequence = LastAcceptedSkyStateSequence;
	const bool bSunFinite = FMath::IsFinite(NewSun.X) && FMath::IsFinite(NewSun.Y) && FMath::IsFinite(NewSun.Z);
	const bool bMoonFinite = FMath::IsFinite(NewMoon.X) && FMath::IsFinite(NewMoon.Y) && FMath::IsFinite(NewMoon.Z);
	const bool bEnvironmentFinite = FMath::IsFinite(NewUtcUnixSeconds) && FMath::IsFinite(NewLatitudeDegrees) &&
		FMath::IsFinite(NewLongitudeDegrees) && FMath::IsFinite(NewElevationMeters) && FMath::IsFinite(NewTimeScale) &&
		FMath::IsFinite(NewDomainExtentMeters.X) && FMath::IsFinite(NewDomainExtentMeters.Y) &&
		FMath::IsFinite(NewDomainExtentMeters.Z) && FMath::IsFinite(NewRelativeHumidity) &&
		FMath::IsFinite(NewVisibilityMeters) && FMath::IsFinite(NewMeanWind.X) && FMath::IsFinite(NewMeanWind.Y) &&
		FMath::IsFinite(NewMeanWind.Z) && FMath::IsFinite(NewSunIlluminanceLux) &&
		FMath::IsFinite(NewMoonIlluminatedFraction) && FMath::IsFinite(NewMoonIlluminanceLux);
	if (!bSunFinite || !bMoonFinite || !bEnvironmentFinite || NewControlResult > 2 || NewCloudLayerCount > 4 ||
		NewUtcUnixSeconds < MinimumSkyUtcUnixSeconds || NewUtcUnixSeconds > MaximumSkyUtcUnixSeconds ||
		NewLatitudeDegrees < -90.0 || NewLatitudeDegrees > 90.0 || NewLongitudeDegrees < -180.0 || NewLongitudeDegrees > 180.0 ||
		NewElevationMeters < -500.0f || NewElevationMeters > 100000.0f || NewTimeScale < -86400.0f || NewTimeScale > 86400.0f ||
		NewDomainExtentMeters.X <= 0.0 || NewDomainExtentMeters.Y <= 0.0 || NewDomainExtentMeters.Z <= 0.0 ||
		NewRelativeHumidity < 0.0f || NewRelativeHumidity > 1.0f ||
		NewVisibilityMeters < 1.0f || NewVisibilityMeters > 200000.0f ||
		NewMoonIlluminatedFraction < 0.0f || NewMoonIlluminatedFraction > 1.0f ||
		NewMoonIlluminanceLux < 0.0f || NewMoonIlluminanceLux > 10.0f ||
		!FMath::IsNearlyEqual(NewSun.SizeSquared(), 1.0, 0.03) || !FMath::IsNearlyEqual(NewMoon.SizeSquared(), 1.0, 0.03))
	{
		return false;
	}

	bool bServerSequenceRestarted = false;
	if (bHasSkyState)
	{
		const int32 SignedSequenceDelta = static_cast<int32>(NewSkyStateSequence - PreviousSkyStateSequence);
		if (SignedSequenceDelta <= 0)
		{
			const bool bSequenceMovedFarBackward = NewSkyStateSequence < PreviousSkyStateSequence &&
				PreviousSkyStateSequence - NewSkyStateSequence > 32U;
			const bool bRestartedAfterReceiveGap = LastSkyStateReceivePlatformSeconds >= 0.0 &&
				ReceivePlatformSeconds - LastSkyStateReceivePlatformSeconds > 5.0 &&
				NewSkyStateSequence < PreviousSkyStateSequence;
			if (bSequenceMovedFarBackward || bRestartedAfterReceiveGap)
			{
				bServerSequenceRestarted = true;
				ConsecutiveBackwardSkyPackets = 0;
			}
			else if (NewSkyStateSequence < PreviousSkyStateSequence)
			{
				// One reordered UDP datagram must never rewind a high-speed clock.
				// Three consecutive backward packets instead indicate a fast server
				// restart whose old sequence had not yet advanced past 32.
				++ConsecutiveBackwardSkyPackets;
				if (ConsecutiveBackwardSkyPackets < 3)
				{
					return false;
				}
				bServerSequenceRestarted = true;
				ConsecutiveBackwardSkyPackets = 0;
			}
			else
			{
				// A duplicate after backward packets can be the third packet of a
				// fast restart (for example previous 2 -> new 0, 1, 2). A lone
				// duplicate is ordinary UDP duplication and remains ignored.
				if (ConsecutiveBackwardSkyPackets == 0)
				{
					return false;
				}
				++ConsecutiveBackwardSkyPackets;
				if (ConsecutiveBackwardSkyPackets < 3)
				{
					return false;
				}
				bServerSequenceRestarted = true;
				ConsecutiveBackwardSkyPackets = 0;
			}
		}
		else
		{
			ConsecutiveBackwardSkyPackets = 0;
		}
	}
	else if (bCanPreserveRuntimeUtc && !bAwaitingInitialServerSync)
	{
		const double AllowedReconnectClockErrorSeconds = FMath::Max(
			5.0,
			FMath::Abs(static_cast<double>(RuntimeTimeScaleBeforePacket)) * 0.5);
		bServerSequenceRestarted = FMath::Abs(NewUtcUnixSeconds - RuntimeUtcBeforePacket) >
			AllowedReconnectClockErrorSeconds;
	}

	if (bServerSequenceRestarted)
	{
		// A new solver epoch may restart CLD2 IDs and timestamps at zero. Drop
		// incomplete packets and interpolation history, but retain the wind phase.
		VolumeReceiver.Reset();
		DensityFrames.BreakContinuity();
		CloudMotion.BreakContinuity();
	}
	const bool bShouldSynchronizeAuthoring = bSyncAuthoringSettingsOnConnect &&
		(bAwaitingInitialServerSync || bServerSequenceRestarted);
	const bool bPreserveContinuityForRestart = bShouldSynchronizeAuthoring &&
		bServerSequenceRestarted && bCanPreserveRuntimeUtc;
	const bool bRuntimeResyncAcknowledged = bRuntimeClockResyncPending &&
		RuntimeClockResyncControlSequence != 0 &&
		NewControlSession == ControlSessionId &&
		NewControlSequence == RuntimeClockResyncControlSequence &&
		(NewControlResult == 1 || NewControlResult == 2);
	const bool bTimeScaleControlAcknowledged = bTimeScaleOverridePending &&
		TimeScaleControlSequence != 0 &&
		NewControlSession == ControlSessionId &&
		NewControlSequence == TimeScaleControlSequence &&
		(NewControlResult == 1 || NewControlResult == 2);
	const bool bUseLocalTimeScaleOverride =
		(bTimeScaleApplyScheduled || bTimeScaleOverridePending) && !bTimeScaleControlAcknowledged;
	const bool bHoldRuntimeEnvironment = bPreserveContinuityForRestart ||
		(bRuntimeClockResyncPending && !bRuntimeResyncAcknowledged);

	SkyStateSequence = static_cast<int32>(NewSkyStateSequence);
	LastAcceptedSkyStateSequence = NewSkyStateSequence;
	LatestVolumeFrameId = static_cast<int32>(ReadU32(Data, 20));
	ServerUtcDateTime = FDateTime::FromUnixTimestamp(static_cast<int64>(FMath::FloorToDouble(NewUtcUnixSeconds)));
	ServerLocalDateTime = ServerUtcDateTime + FTimespan::FromHours(UtcOffsetHours);
	ServerLatitudeDegrees = NewLatitudeDegrees;
	ServerLongitudeDegrees = NewLongitudeDegrees;
	ServerElevationMeters = NewElevationMeters;
	ServerTimeScale = NewTimeScale;
	if (bPreserveContinuityForRestart)
	{
		RebaseRuntimeClockFromServer(
			RuntimeUtcBeforePacket,
			RuntimeTimeScaleBeforePacket,
			RuntimeLatitudeBeforePacket,
			RuntimeLongitudeBeforePacket,
			ReceivePlatformSeconds);
	}
	else if (!bRuntimeClockResyncPending || bRuntimeResyncAcknowledged)
	{
		RebaseRuntimeClockFromServer(
			NewUtcUnixSeconds,
			bUseLocalTimeScaleOverride ? ControlTimeScale : NewTimeScale,
			NewLatitudeDegrees,
			NewLongitudeDegrees,
			ReceivePlatformSeconds);
	}
	ServerDomainExtentMeters = NewDomainExtentMeters;
	ServerCloudLayerCount = NewCloudLayerCount;
	if (!bHoldRuntimeEnvironment)
	{
		UtcUnixSeconds = NewUtcUnixSeconds;
		RelativeHumidity = NewRelativeHumidity;
		VisibilityMeters = NewVisibilityMeters;
		MeanWindEnuMetersPerSecond = NewMeanWind;
		SunDirectionEnu = NewSun;
		SunIlluminanceLux = NewSunIlluminanceLux;
		MoonDirectionEnu = NewMoon;
		MoonIlluminatedFraction = NewMoonIlluminatedFraction;
		MoonIlluminanceLux = NewMoonIlluminanceLux;
	}
	bHasSkyState = true;
	LastSkyStateReceivePlatformSeconds = ReceivePlatformSeconds;
	HandleControlAcknowledgement(NewControlSession, NewControlSequence, NewControlResult);
	if (bShouldSynchronizeAuthoring)
	{
		ScheduleAuthoringSync(
			bPreserveContinuityForRestart,
			RuntimeUtcBeforePacket);
	}
	UpdateEnvironmentLighting();
	if (bUseServerDomainSize)
	{
		UpdateVolumeRenderer();
	}
	++ValidSkyPackets;
	return true;
}

bool ASkySimSystem::ParseVolumePacket(const uint8* Data, int32 NumBytes)
{
	FSkySimDensityFrame CompletedFrame;
	const FSkySimVolumeReceiver::EPacketResult Result = VolumeReceiver.Consume(
		Data, NumBytes, FPlatformTime::Seconds(), CompletedFrame);
	if (Result == FSkySimVolumeReceiver::EPacketResult::Rejected)
	{
		++RejectedVolumePackets;
		return false;
	}
	++ValidVolumePackets;
	if (Result == FSkySimVolumeReceiver::EPacketResult::FrameComplete)
	{
		PublishCompletedFrame(MoveTemp(CompletedFrame));
	}
	return true;
}

void ASkySimSystem::PublishCompletedFrame(FSkySimDensityFrame&& CompletedFrame)
{
	const FSkySimDensityFrame& LastFrame = DensityFrames.GetCurrent();
	if (LastFrame.IsValid() && CompletedFrame.ReceivePlatformSeconds - LastFrame.ReceivePlatformSeconds > 2.0)
	{
		// Density-only senders have no SKS1 restart signal. A prolonged gap starts
		// a new interpolation segment rather than blending across an outage.
		DensityFrames.BreakContinuity();
	}
	if (!DensityFrames.Publish(MoveTemp(CompletedFrame)))
	{
		return;
	}
	const FSkySimDensityFrame& Frame = DensityFrames.GetCurrent();
	CloudMotion.PushFrame(Frame, DensityFrames.GetPrevious().IsValid());
	DensityGridSize = Frame.GridSize;
	DensityValueScale = Frame.ValueScale;
	DensityValueBias = Frame.ValueBias;
	LatestVolumeFrameId = static_cast<int32>(Frame.FrameId);
	bHasCompleteVolumeFrame = true;
	++CompleteVolumeFrames;

	uint16 Minimum = TNumericLimits<uint16>::Max();
	uint16 Maximum = 0;
	uint64 Sum = 0;
	const int32 VoxelCount = Frame.Bytes.Num() / 2;
	for (int32 Index = 0; Index < VoxelCount; ++Index)
	{
		const uint16 Value = ReadU16(Frame.Bytes.GetData(), Index * 2);
		Minimum = FMath::Min(Minimum, Value);
		Maximum = FMath::Max(Maximum, Value);
		Sum += Value;
	}
	const float Normalizer = 1.0f / 65535.0f;
	DensityMinimum = VoxelCount > 0 ? Minimum * Normalizer : 0.0f;
	DensityMaximum = VoxelCount > 0 ? Maximum * Normalizer : 0.0f;
	DensityMean = VoxelCount > 0 ? static_cast<float>(static_cast<double>(Sum) / VoxelCount) * Normalizer : 0.0f;
	if (CompleteVolumeFrames == 1 || CompleteVolumeFrames % 100 == 0)
	{
		UE_LOG(
			LogTemp,
			Display,
			TEXT("SkySim: CLD2 완성 frame=%u grid=%dx%dx%d density[max=%.6f, mean=%.6f, scale=%.6f, bias=%.6f]"),
			Frame.FrameId,
			DensityGridSize.X,
			DensityGridSize.Y,
			DensityGridSize.Z,
			DensityMaximum,
			DensityMean,
			DensityValueScale,
			DensityValueBias);
	}

}

void ASkySimSystem::UpdateEnvironmentLighting()
{
	if (SunLightComponent == nullptr || MoonLightComponent == nullptr || SkyLightComponent == nullptr ||
		SkyAtmosphereComponent == nullptr || WeatherFogComponent == nullptr)
	{
		return;
	}

	SunLightComponent->SetVisibility(bEnableEnvironmentLighting, true);
	MoonLightComponent->SetVisibility(bEnableEnvironmentLighting && bEnableMoonLight, true);
	SkyLightComponent->SetVisibility(bEnableEnvironmentLighting, true);
	SkyAtmosphereComponent->SetVisibility(bEnableEnvironmentLighting, true);
	const double CurrentPlatformSeconds = FPlatformTime::Seconds();
	const bool bHasFreshSkyState = bHasSkyState && LastSkyStateReceivePlatformSeconds >= 0.0 &&
		CurrentPlatformSeconds - LastSkyStateReceivePlatformSeconds <= 2.0;
	UpdateWeatherFog(
		bHasFreshSkyState ? VisibilityMeters : CustomVisibilityMeters,
		bHasFreshSkyState ? RelativeHumidity : CustomRelativeHumidity);
	if (!bEnableEnvironmentLighting)
	{
		return;
	}

	SkyLightComponent->SetIntensity(FMath::Max(0.0f, SkyLightIntensity));
	if (!bHasFreshSkyState)
	{
		if (bEditorPreviewClockInitialized)
		{
			UpdatePreviewLightingAtUtc(
				EditorPreviewUtcUnixSeconds,
				RuntimeClockLatitudeDegrees,
				RuntimeClockLongitudeDegrees);
		}
		else
		{
			UpdatePreviewLightingFromControls();
		}
		return;
	}

	const float EffectiveSunIlluminance =
		FMath::Max(0.0f, SunIlluminanceLux) * FMath::Max(0.0f, SunIlluminanceMultiplier);
	const float EffectiveMoonIlluminance =
		FMath::Max(0.0f, MoonIlluminanceLux) * FMath::Max(0.0f, MoonIlluminanceMultiplier);
	const bool bMoonIsPrimaryForwardLight =
		bEnableMoonLight && EffectiveMoonIlluminance > EffectiveSunIlluminance;
	SunLightComponent->ForwardShadingPriority = bMoonIsPrimaryForwardLight ? 0 : 1;
	MoonLightComponent->ForwardShadingPriority = bMoonIsPrimaryForwardLight ? 1 : 0;
	SunLightComponent->SetIntensity(EffectiveSunIlluminance);
	SunLightComponent->SetLightColor(SunTint);
	SunLightComponent->SetVolumetricScatteringIntensity(FMath::Max(0.0f, SunVolumetricScatteringIntensity));
	const FVector SunDirectionWorld = GetActorTransform().TransformVectorNoScale(SunDirectionEnu).GetSafeNormal();
	if (!SunDirectionWorld.IsNearlyZero())
	{
		// SKS1 points from the observer toward the sun; a directional light points along propagation.
		SunLightComponent->SetWorldRotation((-SunDirectionWorld).Rotation());
	}
	UpdateMoonLighting(MoonDirectionEnu, MoonIlluminanceLux);
}

void ASkySimSystem::UpdateMoonLighting(const FVector& DirectionEnu, float IlluminanceLux)
{
	if (MoonLightComponent == nullptr)
	{
		return;
	}
	const bool bDirectionFinite = FMath::IsFinite(DirectionEnu.X) && FMath::IsFinite(DirectionEnu.Y) &&
		FMath::IsFinite(DirectionEnu.Z);
	const bool bValid = bDirectionFinite && FMath::IsFinite(IlluminanceLux) &&
		!DirectionEnu.IsNearlyZero();
	MoonLightComponent->SetVisibility(bEnableEnvironmentLighting && bEnableMoonLight && bValid, true);
	if (!bEnableEnvironmentLighting || !bEnableMoonLight || !bValid)
	{
		return;
	}

	MoonLightComponent->SetIntensity(
		FMath::Max(0.0f, IlluminanceLux) * FMath::Max(0.0f, MoonIlluminanceMultiplier));
	MoonLightComponent->SetLightColor(MoonTint);
	MoonLightComponent->SetCastShadows(bMoonCastsShadows);
	MoonLightComponent->SetVolumetricScatteringIntensity(
		FMath::Max(0.0f, MoonVolumetricScatteringIntensity));
	const FVector DirectionWorld = GetActorTransform().TransformVectorNoScale(DirectionEnu).GetSafeNormal();
	if (!DirectionWorld.IsNearlyZero())
	{
		// Like the sun vector, SKS1 moon direction points from the observer toward the source.
		MoonLightComponent->SetWorldRotation((-DirectionWorld).Rotation());
	}
}

void ASkySimSystem::UpdateWeatherFog(float InVisibilityMeters, float InRelativeHumidity)
{
	if (WeatherFogComponent == nullptr)
	{
		return;
	}
	const bool bInputsValid = FMath::IsFinite(InVisibilityMeters) && FMath::IsFinite(InRelativeHumidity) &&
		InVisibilityMeters >= 1.0f && InVisibilityMeters <= 200000.0f &&
		InRelativeHumidity >= 0.0f && InRelativeHumidity <= 1.0f;
	WeatherFogComponent->SetVisibility(bEnableWeatherFog && bInputsValid, true);
	if (!bEnableWeatherFog || !bInputsValid)
	{
		return;
	}

	// Meteorological visibility uses a 2% contrast threshold: beta = -ln(0.02) / V.
	// UE stores exponential-fog density as 1000 times reciprocal world units.
	const float UnitsPerMeter = FMath::Max(1.0f, FMath::IsFinite(UnrealUnitsPerMeter) ? UnrealUnitsPerMeter : 100.0f);
	const float SaturationT = FMath::Clamp((InRelativeHumidity - 0.5f) * 2.0f, 0.0f, 1.0f);
	const float SmoothSaturation = SaturationT * SaturationT * (3.0f - 2.0f * SaturationT);
	const float HumidityMultiplier = 1.0f + FMath::Max(0.0f, FogHumidityDensityBoost) * SmoothSaturation;
	const float PhysicalFogDensity = 3912.023f / (InVisibilityMeters * UnitsPerMeter);
	const float FogDensity = FMath::Clamp(
		PhysicalFogDensity * FMath::Max(0.0f, FogDensityMultiplier) * HumidityMultiplier,
		0.0f,
		0.05f);
	WeatherFogComponent->SetFogDensity(FogDensity);
	WeatherFogComponent->SetFogHeightFalloff(FMath::Clamp(FogHeightFalloff, 0.001f, 2.0f));
	WeatherFogComponent->SetFogMaxOpacity(FMath::Clamp(FogMaximumOpacity, 0.0f, 1.0f));
	WeatherFogComponent->SetStartDistance(FMath::Max(0.0f, FogStartDistanceMeters) * UnitsPerMeter);
	WeatherFogComponent->SetFogInscatteringColor(FogInscatteringTint);
	WeatherFogComponent->SetVolumetricFog(bEnableVolumetricWeatherFog);
	WeatherFogComponent->SetVolumetricFogExtinctionScale(
		FMath::Clamp(VolumetricFogExtinctionScale, 0.0f, 20.0f));
	WeatherFogComponent->SetVolumetricFogDistance(
		FMath::Clamp(VolumetricFogViewDistanceKm, 0.25f, 50.0f) * 1000.0f * UnitsPerMeter);
}

void ASkySimSystem::UpdatePreviewLightingFromControls()
{
	if (SunLightComponent == nullptr || SkyLightComponent == nullptr || !bEnableEnvironmentLighting ||
		!FMath::IsFinite(UtcOffsetHours) || !FMath::IsFinite(ControlLatitudeDegrees) ||
		!FMath::IsFinite(ControlLongitudeDegrees) || ControlLatitudeDegrees < -90.0 ||
		ControlLatitudeDegrees > 90.0 || ControlLongitudeDegrees < -180.0 || ControlLongitudeDegrees > 180.0 ||
		ControlLocalDateTime.GetYear() < 1900 || ControlLocalDateTime.GetYear() > 2100)
	{
		return;
	}

	const FDateTime UtcDateTime = ControlLocalDateTime - FTimespan::FromHours(UtcOffsetHours);
	const double PreviewUtcUnix = static_cast<double>(UtcDateTime.ToUnixTimestamp());
	if (PreviewUtcUnix < MinimumSkyUtcUnixSeconds || PreviewUtcUnix > MaximumSkyUtcUnixSeconds)
	{
		return;
	}
	UpdatePreviewLightingAtUtc(PreviewUtcUnix, ControlLatitudeDegrees, ControlLongitudeDegrees);
}

void ASkySimSystem::ResetEditorPreviewClock()
{
	if (!FMath::IsFinite(UtcOffsetHours) || ControlLocalDateTime.GetYear() < 1900 ||
		ControlLocalDateTime.GetYear() > 2100)
	{
		bEditorPreviewClockInitialized = false;
		return;
	}
	const FDateTime UtcDateTime = ControlLocalDateTime - FTimespan::FromHours(UtcOffsetHours);
	const double AuthoringUtcUnixSeconds = FMath::Clamp(
		static_cast<double>(UtcDateTime.ToUnixTimestamp()),
		MinimumSkyUtcUnixSeconds,
		MaximumSkyUtcUnixSeconds);
	const double PlatformSeconds = FPlatformTime::Seconds();
	RuntimeClockBaseUtcUnixSeconds = AuthoringUtcUnixSeconds;
	RuntimeClockBasePlatformSeconds = PlatformSeconds;
	RuntimeClockTimeScale = FMath::Clamp(ControlTimeScale, -86400.0f, 86400.0f);
	RuntimeClockLatitudeDegrees = ControlLatitudeDegrees;
	RuntimeClockLongitudeDegrees = ControlLongitudeDegrees;
	bRuntimeClockHasServerReference = false;
	EditorPreviewUtcUnixSeconds = AuthoringUtcUnixSeconds;
	EditorPreviewLocalDateTime = ControlLocalDateTime;
	CurrentUtcDateTime = UtcDateTime;
	CurrentLocalDateTime = ControlLocalDateTime;
	CurrentEffectiveTimeScale = RuntimeClockTimeScale;
	RuntimeClockSource = TEXT("authoring_fallback");
	CurrentSunElevationDegrees = 0.0f;
	bEditorPreviewClockInitialized = true;
}

void ASkySimSystem::RebaseRuntimeClockFromServer(
	double ServerUtcUnix,
	float TimeScale,
	double LatitudeDegrees,
	double LongitudeDegrees,
	double ReceivePlatformSeconds)
{
	RuntimeClockBaseUtcUnixSeconds = FMath::Clamp(
		ServerUtcUnix,
		MinimumSkyUtcUnixSeconds,
		MaximumSkyUtcUnixSeconds);
	RuntimeClockBasePlatformSeconds = ReceivePlatformSeconds;
	RuntimeClockTimeScale = FMath::Clamp(TimeScale, -86400.0f, 86400.0f);
	RuntimeClockLatitudeDegrees = LatitudeDegrees;
	RuntimeClockLongitudeDegrees = LongitudeDegrees;
	bRuntimeClockHasServerReference = true;
	EditorPreviewUtcUnixSeconds = RuntimeClockBaseUtcUnixSeconds;
	const double WholeUtcSeconds = FMath::FloorToDouble(RuntimeClockBaseUtcUnixSeconds);
	CurrentUtcDateTime = FDateTime::FromUnixTimestamp(static_cast<int64>(WholeUtcSeconds)) +
		FTimespan::FromSeconds(RuntimeClockBaseUtcUnixSeconds - WholeUtcSeconds);
	CurrentLocalDateTime = CurrentUtcDateTime + FTimespan::FromHours(UtcOffsetHours);
	EditorPreviewLocalDateTime = CurrentLocalDateTime;
	CurrentEffectiveTimeScale = RuntimeClockTimeScale;
	RuntimeClockSource = TEXT("server_interpolated");
	bEditorPreviewClockInitialized = true;
}

void ASkySimSystem::RebaseRuntimeClockTimeScale(float TimeScale)
{
	if (!bEditorPreviewClockInitialized)
	{
		ResetEditorPreviewClock();
	}
	if (!bEditorPreviewClockInitialized || !FMath::IsFinite(TimeScale))
	{
		return;
	}

	const double PlatformSeconds = FPlatformTime::Seconds();
	bool bAdvance = true;
	if (GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::Editor)
	{
		const bool bHasFreshSkyState = bHasSkyState && LastSkyStateReceivePlatformSeconds >= 0.0 &&
			PlatformSeconds - LastSkyStateReceivePlatformSeconds <= 2.0;
		bAdvance = bPreviewInEditor && (bAnimateTimeInEditor || bHasFreshSkyState);
	}
	RuntimeClockBaseUtcUnixSeconds = EvaluateRuntimeClockUtc(PlatformSeconds, bAdvance);
	RuntimeClockBasePlatformSeconds = PlatformSeconds;
	RuntimeClockTimeScale = FMath::Clamp(TimeScale, -86400.0f, 86400.0f);
	EditorPreviewUtcUnixSeconds = RuntimeClockBaseUtcUnixSeconds;
}

double ASkySimSystem::EvaluateRuntimeClockUtc(double PlatformSeconds, bool bAdvance) const
{
	if (!bEditorPreviewClockInitialized || RuntimeClockBasePlatformSeconds < 0.0 || !bAdvance)
	{
		return FMath::Clamp(
			RuntimeClockBaseUtcUnixSeconds,
			MinimumSkyUtcUnixSeconds,
			MaximumSkyUtcUnixSeconds);
	}
	const double ElapsedPlatformSeconds = FMath::Max(0.0, PlatformSeconds - RuntimeClockBasePlatformSeconds);
	return FMath::Clamp(
		RuntimeClockBaseUtcUnixSeconds + ElapsedPlatformSeconds * static_cast<double>(RuntimeClockTimeScale),
		MinimumSkyUtcUnixSeconds,
		MaximumSkyUtcUnixSeconds);
}

void ASkySimSystem::UpdateRuntimeClockAndLighting()
{
	if (!bEditorPreviewClockInitialized)
	{
		ResetEditorPreviewClock();
	}
	if (!bEditorPreviewClockInitialized)
	{
		return;
	}

	const bool bEditorWorld = GetWorld() != nullptr && GetWorld()->WorldType == EWorldType::Editor;
	const double PlatformSeconds = FPlatformTime::Seconds();
	const bool bHasFreshSkyState = bHasSkyState && LastSkyStateReceivePlatformSeconds >= 0.0 &&
		PlatformSeconds - LastSkyStateReceivePlatformSeconds <= 2.0;
	const bool bPreviewEnabled = !bEditorWorld || bPreviewInEditor;
	const bool bAdvance = bPreviewEnabled && (!bEditorWorld || bAnimateTimeInEditor || bHasFreshSkyState);

	EditorPreviewUtcUnixSeconds = bAdvance
		? EvaluateRuntimeClockUtc(PlatformSeconds, true)
		: FMath::Clamp(
			EditorPreviewUtcUnixSeconds,
			MinimumSkyUtcUnixSeconds,
			MaximumSkyUtcUnixSeconds);
	const bool bAtClockBoundary =
		(EditorPreviewUtcUnixSeconds <= MinimumSkyUtcUnixSeconds && RuntimeClockTimeScale < 0.0f) ||
		(EditorPreviewUtcUnixSeconds >= MaximumSkyUtcUnixSeconds && RuntimeClockTimeScale > 0.0f);
	if (!bAdvance || bAtClockBoundary)
	{
		// Rebase while intentionally paused so enabling preview later does not
		// catch up all of the wall time that elapsed while it was disabled.
		RuntimeClockBaseUtcUnixSeconds = EditorPreviewUtcUnixSeconds;
		RuntimeClockBasePlatformSeconds = PlatformSeconds;
	}
	if (bAtClockBoundary)
	{
		RuntimeClockTimeScale = 0.0f;
	}

	const double WholeUtcSeconds = FMath::FloorToDouble(EditorPreviewUtcUnixSeconds);
	CurrentUtcDateTime = FDateTime::FromUnixTimestamp(static_cast<int64>(WholeUtcSeconds)) +
		FTimespan::FromSeconds(EditorPreviewUtcUnixSeconds - WholeUtcSeconds);
	CurrentLocalDateTime = CurrentUtcDateTime + FTimespan::FromHours(UtcOffsetHours);
	EditorPreviewLocalDateTime = CurrentLocalDateTime;
	CurrentEffectiveTimeScale = bAdvance ? RuntimeClockTimeScale : 0.0f;
	if (!bPreviewEnabled || (!bAdvance && !bHasFreshSkyState))
	{
		RuntimeClockSource = TEXT("editor_paused");
	}
	else if (bHasFreshSkyState)
	{
		RuntimeClockSource = bAdvance ? TEXT("server_interpolated") : TEXT("server_packet");
	}
	else
	{
		RuntimeClockSource = bRuntimeClockHasServerReference
			? TEXT("server_fallback")
			: TEXT("authoring_fallback");
	}

	const FVector RuntimeSunDirection = UpdateSunRotationAtUtc(
		EditorPreviewUtcUnixSeconds,
		RuntimeClockLatitudeDegrees,
		RuntimeClockLongitudeDegrees);
	if (bPreviewEnabled && !bHasFreshSkyState && !RuntimeSunDirection.IsNearlyZero())
	{
		const float FallbackLightingBlend = bRuntimeClockHasServerReference &&
			RuntimeClockBasePlatformSeconds >= 0.0
			? FMath::Clamp(
				static_cast<float>(PlatformSeconds - RuntimeClockBasePlatformSeconds - 2.0),
				0.0f,
				1.0f)
			: 1.0f;
		UpdateWeatherFog(
			bRuntimeClockHasServerReference ? VisibilityMeters : CustomVisibilityMeters,
			bRuntimeClockHasServerReference ? RelativeHumidity : CustomRelativeHumidity);
		UpdatePreviewLightingAtUtc(
			EditorPreviewUtcUnixSeconds,
			RuntimeClockLatitudeDegrees,
			RuntimeClockLongitudeDegrees,
			FallbackLightingBlend);
	}

	if (bShowDebugOverlay && PlatformSeconds >= NextRuntimeClockDiagnosticPlatformSeconds)
	{
		NextRuntimeClockDiagnosticPlatformSeconds = PlatformSeconds + 2.0;
		UE_LOG(
			LogTemp,
			Display,
			TEXT("SkySim clock: source=%s utc=%s local=%s scale=%.3f sun_elevation_deg=%.3f"),
			*RuntimeClockSource,
			*CurrentUtcDateTime.ToIso8601(),
			*CurrentLocalDateTime.ToIso8601(),
			CurrentEffectiveTimeScale,
			CurrentSunElevationDegrees);
	}
}

FVector ASkySimSystem::UpdateSunRotationAtUtc(
	double PreviewUtcUnix,
	double LatitudeDegrees,
	double LongitudeDegrees)
{
	if (!FMath::IsFinite(PreviewUtcUnix) || !FMath::IsFinite(LatitudeDegrees) ||
		!FMath::IsFinite(LongitudeDegrees) || LatitudeDegrees < -90.0 || LatitudeDegrees > 90.0 ||
		LongitudeDegrees < -180.0 || LongitudeDegrees > 180.0)
	{
		return FVector::ZeroVector;
	}

	const FVector PreviewSunDirection = CalculateSunDirectionEnu(
		PreviewUtcUnix,
		LatitudeDegrees,
		LongitudeDegrees);
	CurrentSunElevationDegrees = static_cast<float>(FMath::RadiansToDegrees(
		FMath::Asin(FMath::Clamp(static_cast<double>(PreviewSunDirection.Z), -1.0, 1.0))));
	if (SunLightComponent != nullptr && bEnableEnvironmentLighting)
	{
		const FVector PreviewSunDirectionWorld =
			GetActorTransform().TransformVectorNoScale(PreviewSunDirection).GetSafeNormal();
		if (!PreviewSunDirectionWorld.IsNearlyZero())
		{
			SunLightComponent->SetWorldRotation((-PreviewSunDirectionWorld).Rotation());
		}
	}
	return PreviewSunDirection;
}

void ASkySimSystem::UpdatePreviewLightingAtUtc(
	double PreviewUtcUnix,
	double LatitudeDegrees,
	double LongitudeDegrees,
	float FallbackBlend)
{
	if (SunLightComponent == nullptr || SkyLightComponent == nullptr || !bEnableEnvironmentLighting ||
		!FMath::IsFinite(PreviewUtcUnix) || !FMath::IsFinite(LatitudeDegrees) ||
		!FMath::IsFinite(LongitudeDegrees))
	{
		return;
	}
	const FVector PreviewSunDirection = UpdateSunRotationAtUtc(
		PreviewUtcUnix,
		LatitudeDegrees,
		LongitudeDegrees);
	if (PreviewSunDirection.IsNearlyZero())
	{
		return;
	}
	const float DaylightFactor = FMath::Sqrt(FMath::Clamp(static_cast<float>(PreviewSunDirection.Z), 0.0f, 1.0f));
	const float NightFactor = FMath::Sqrt(FMath::Clamp(static_cast<float>(-PreviewSunDirection.Z), 0.0f, 1.0f));
	const float BlendAlpha = bRuntimeClockHasServerReference
		? FMath::Clamp(FallbackBlend, 0.0f, 1.0f)
		: 1.0f;
	const float PreviewSunIlluminanceLux =
		FMath::Max(0.0f, DefaultSunIlluminanceLux) * DaylightFactor;
	const float BlendedSunIlluminanceLux = FMath::Lerp(
		FMath::Max(0.0f, SunIlluminanceLux),
		PreviewSunIlluminanceLux,
		BlendAlpha);
	const float PreviewMoonIlluminanceLux = FMath::Max(0.0f, DefaultMoonIlluminanceLux) * NightFactor;
	const float BlendedMoonIlluminanceLux = FMath::Lerp(
		FMath::Max(0.0f, MoonIlluminanceLux),
		PreviewMoonIlluminanceLux,
		BlendAlpha);
	const float EffectiveSunIlluminance =
		BlendedSunIlluminanceLux * FMath::Max(0.0f, SunIlluminanceMultiplier);
	const float EffectiveMoonIlluminance =
		BlendedMoonIlluminanceLux * FMath::Max(0.0f, MoonIlluminanceMultiplier);
	const bool bMoonIsPrimaryForwardLight = bEnableMoonLight &&
		EffectiveMoonIlluminance > EffectiveSunIlluminance;
	SunLightComponent->ForwardShadingPriority = bMoonIsPrimaryForwardLight ? 0 : 1;
	if (MoonLightComponent != nullptr)
	{
		MoonLightComponent->ForwardShadingPriority = bMoonIsPrimaryForwardLight ? 1 : 0;
	}
	SunLightComponent->SetIntensity(EffectiveSunIlluminance);
	SunLightComponent->SetLightColor(SunTint);
	SunLightComponent->SetVolumetricScatteringIntensity(FMath::Max(0.0f, SunVolumetricScatteringIntensity));
	SkyLightComponent->SetIntensity(FMath::Max(0.0f, SkyLightIntensity));
	// Cross-fade away from the last live lunar state instead of popping to the
	// lightweight offline approximation at the two-second freshness boundary.
	FVector PreviewMoonDirection = -PreviewSunDirection;
	if (bRuntimeClockHasServerReference && !MoonDirectionEnu.IsNearlyZero() && BlendAlpha < 1.0f)
	{
		PreviewMoonDirection = FMath::Lerp(MoonDirectionEnu, PreviewMoonDirection, BlendAlpha).GetSafeNormal();
	}
	UpdateMoonLighting(PreviewMoonDirection, BlendedMoonIlluminanceLux);
}
