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
#include "Engine/VolumeTexture.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Crc.h"
#include "Misc/Guid.h"
#include "RHICommandList.h"
#include "RenderCommandFence.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "TextureResource.h"
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
	RefreshEstimatedCloudSourceCount();
	UpdateEnvironmentLighting();
	UpdateVolumeRenderer();
}

void ASkySimSystem::BeginPlay()
{
	Super::BeginPlay();

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
		ScheduleAuthoringSync();
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
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, ControlElevationMeters) ||
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
		PropertyName == GET_MEMBER_NAME_CHECKED(ASkySimSystem, DensityPresentationGain))
	{
		UpdateDensityVolumeTexture();
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
	if (bEditorWorld && bPreviewInEditor && bAnimateTimeInEditor)
	{
		const double Now = FPlatformTime::Seconds();
		const bool bHasRecentServerState = bHasSkyState && LastSkyStateReceivePlatformSeconds >= 0.0 &&
			Now - LastSkyStateReceivePlatformSeconds <= 2.0;
		if (!bHasRecentServerState)
		{
			if (!bEditorPreviewClockInitialized)
			{
				ResetEditorPreviewClock();
			}
			EditorPreviewUtcUnixSeconds = FMath::Clamp(
				EditorPreviewUtcUnixSeconds + static_cast<double>(DeltaSeconds) * ControlTimeScale,
				MinimumSkyUtcUnixSeconds,
				MaximumSkyUtcUnixSeconds);
			const int64 LocalUnixSeconds = static_cast<int64>(FMath::FloorToDouble(
				EditorPreviewUtcUnixSeconds + static_cast<double>(UtcOffsetHours) * 3600.0));
			EditorPreviewLocalDateTime = FDateTime::FromUnixTimestamp(LocalUnixSeconds);
			UpdateWeatherFog(CustomVisibilityMeters, CustomRelativeHumidity);
			UpdatePreviewLightingAtUtc(EditorPreviewUtcUnixSeconds);
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
			ApplyDateTimeAndLocation();
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
	if (!bIsReceiving)
	{
		return;
	}

	DrainSocket(SkyStateSocket, true);
	DrainSocket(VolumeSocket, false);
	PruneStaleVolumeFrames();

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
	bAwaitingInitialServerSync = true;

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
	VolumeFrames.Reset();
	bAwaitingInitialServerSync = true;
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

	PendingControlPacket.Reset();
	PendingControlSequence = 0;
	PendingControlSendAttempts = 0;
}

void ASkySimSystem::ApplyDateTimeAndLocation()
{
	bSkyControlsApplyScheduled = false;
	if (!FMath::IsFinite(UtcOffsetHours) || UtcOffsetHours < -14.0f || UtcOffsetHours > 14.0f ||
		ControlLocalDateTime.GetYear() < 1900 || ControlLocalDateTime.GetYear() > 2100)
	{
		ControlStatus = ESkySimControlStatus::Rejected;
		ControlStatusMessage = TEXT("Local date/time or UTC offset is outside the supported range");
		return;
	}
	const FDateTime UtcDateTime = ControlLocalDateTime - FTimespan::FromHours(UtcOffsetHours);
	const double ControlUtcUnix = static_cast<double>(UtcDateTime.ToUnixTimestamp());
	UpdatePreviewLightingFromControls();
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
}

void ASkySimSystem::ApplyTimeScale()
{
	if (!FMath::IsNearlyZero(ControlTimeScale))
	{
		LastNonZeroTimeScale = ControlTimeScale;
	}
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

void ASkySimSystem::ScheduleAuthoringSync()
{
	const double DueTime = FPlatformTime::Seconds() + 0.05;
	bAwaitingInitialServerSync = false;
	bSkyControlsApplyScheduled = true;
	SkyControlsApplyDuePlatformSeconds = DueTime;
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
	const uint32 PreviousSkyStateSequence = static_cast<uint32>(FMath::Max(0, SkyStateSequence));
	const bool bSequenceMovedFarBackward = bHasSkyState && NewSkyStateSequence < PreviousSkyStateSequence &&
		PreviousSkyStateSequence - NewSkyStateSequence > 32U;
	const bool bRestartedAfterReceiveGap = bHasSkyState && LastSkyStateReceivePlatformSeconds >= 0.0 &&
		ReceivePlatformSeconds - LastSkyStateReceivePlatformSeconds > 5.0 &&
		NewSkyStateSequence < PreviousSkyStateSequence;
	// A slow editor frame or shader compile can delay packets for several
	// seconds without restarting the server. Reapplying the authored clock on
	// every such gap repeatedly rewinds the sun and Natural weather. Only a
	// genuine sequence reset is treated as a server reconnect.
	const bool bServerSequenceRestarted = bSequenceMovedFarBackward || bRestartedAfterReceiveGap;
	const bool bShouldSynchronizeAuthoring = bSyncAuthoringSettingsOnConnect &&
		(bAwaitingInitialServerSync || bServerSequenceRestarted);
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

	SkyStateSequence = static_cast<int32>(NewSkyStateSequence);
	LatestVolumeFrameId = static_cast<int32>(ReadU32(Data, 20));
	UtcUnixSeconds = NewUtcUnixSeconds;
	ServerUtcDateTime = FDateTime::FromUnixTimestamp(static_cast<int64>(FMath::FloorToDouble(NewUtcUnixSeconds)));
	ServerLocalDateTime = ServerUtcDateTime + FTimespan::FromHours(UtcOffsetHours);
	ServerLatitudeDegrees = NewLatitudeDegrees;
	ServerLongitudeDegrees = NewLongitudeDegrees;
	ServerElevationMeters = NewElevationMeters;
	ServerTimeScale = NewTimeScale;
	ServerDomainExtentMeters = NewDomainExtentMeters;
	ServerCloudLayerCount = NewCloudLayerCount;
	RelativeHumidity = NewRelativeHumidity;
	VisibilityMeters = NewVisibilityMeters;
	MeanWindEnuMetersPerSecond = NewMeanWind;
	SunDirectionEnu = NewSun;
	SunIlluminanceLux = NewSunIlluminanceLux;
	MoonDirectionEnu = NewMoon;
	MoonIlluminatedFraction = NewMoonIlluminatedFraction;
	MoonIlluminanceLux = NewMoonIlluminanceLux;
	bHasSkyState = true;
	LastSkyStateReceivePlatformSeconds = ReceivePlatformSeconds;
	HandleControlAcknowledgement(NewControlSession, NewControlSequence, NewControlResult);
	if (bShouldSynchronizeAuthoring)
	{
		ScheduleAuthoringSync();
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
	if (NumBytes < 64 || !HasMagic(Data, "CLD2") || ReadU16(Data, 4) != 2 || ReadU16(Data, 6) != 64)
	{
		++RejectedVolumePackets;
		return false;
	}

	const uint32 FrameId = ReadU32(Data, 8);
	const uint32 FieldCrc32 = ReadU32(Data, 12);
	const double SimulationTime = ReadF64(Data, 16);
	const uint16 GridX = ReadU16(Data, 24);
	const uint16 GridY = ReadU16(Data, 26);
	const uint16 GridZ = ReadU16(Data, 28);
	const uint8 VoxelFormat = Data[30];
	const uint8 FieldId = Data[31];
	const uint8 ChannelCount = Data[32];
	const uint8 Compression = Data[33];
	const uint16 PayloadBytes = ReadU16(Data, 38);
	const uint16 ChunkIndex = ReadU16(Data, 34);
	const uint16 ChunkCount = ReadU16(Data, 36);
	const uint16 FieldMask = ReadU16(Data, 40);
	const uint16 Flags = ReadU16(Data, 42);
	const uint32 PayloadOffset = ReadU32(Data, 44);
	const uint32 EncodedBytes = ReadU32(Data, 48);
	const uint32 DecodedBytes = ReadU32(Data, 52);
	const float ValueScale = ReadF32(Data, 56);
	const float ValueBias = ReadF32(Data, 60);

	static constexpr uint32 MaxPayloadBytes = 1200;
	static constexpr uint32 MaxFieldBytes = 64 * 1024 * 1024;
	static constexpr uint16 SupportedFieldMask = 0x001f;
	static constexpr uint16 MacroFieldMask = 0x000f;
	const uint16 FieldBit = FieldId >= 1 && FieldId <= 5 ? static_cast<uint16>(1u << (FieldId - 1u)) : 0;
	const uint32 BytesPerChannel = (VoxelFormat == 1 || VoxelFormat == 3) ? 1u :
		(VoxelFormat == 2 || VoxelFormat == 4) ? 2u : 0u;
	const uint8 ExpectedFormat[5] = {2, 4, 1, 1, 1};
	const uint8 ExpectedChannels[5] = {1, 3, 1, 1, 1};
	const uint64 ExpectedDecodedBytes = static_cast<uint64>(GridX) * GridY * GridZ * ChannelCount * BytesPerChannel;
	const uint32 ExpectedChunkCount = EncodedBytes == 0 ? 0 : (EncodedBytes + MaxPayloadBytes - 1u) / MaxPayloadBytes;
	const uint32 ExpectedPayloadBytes = PayloadOffset < EncodedBytes ? FMath::Min(MaxPayloadBytes, EncodedBytes - PayloadOffset) : 0;
	const uint8 OccupancyBrick = static_cast<uint8>(Flags >> 8);
	const bool bFlagsValid = (Flags & 0x00feu) == 0u &&
		((FieldId == 5 && (OccupancyBrick == 2 || OccupancyBrick == 4 || OccupancyBrick == 8)) ||
		 (FieldId != 5 && OccupancyBrick == 0));

	if (FieldBit == 0 || VoxelFormat != ExpectedFormat[FieldId - 1] || ChannelCount != ExpectedChannels[FieldId - 1] ||
		Compression > 1 || !FMath::IsFinite(SimulationTime) || !FMath::IsFinite(ValueScale) || !FMath::IsFinite(ValueBias) ||
		GridX == 0 || GridY == 0 || GridZ == 0 || BytesPerChannel == 0 || ExpectedDecodedBytes != DecodedBytes ||
		EncodedBytes == 0 || EncodedBytes > MaxFieldBytes || DecodedBytes > MaxFieldBytes ||
		FieldMask == 0 || (FieldMask & ~SupportedFieldMask) != 0 || (FieldMask & FieldBit) == 0 ||
		(FieldId == 5 && (FieldMask & MacroFieldMask) == 0) || !bFlagsValid ||
		PayloadBytes > MaxPayloadBytes || NumBytes != 64 + PayloadBytes || ChunkCount != ExpectedChunkCount ||
		ChunkIndex >= ChunkCount || PayloadOffset != static_cast<uint32>(ChunkIndex) * MaxPayloadBytes ||
		PayloadBytes != ExpectedPayloadBytes || PayloadOffset + PayloadBytes > EncodedBytes)
	{
		++RejectedVolumePackets;
		return false;
	}

	FVolumeFrameAssembly* Frame = VolumeFrames.Find(FrameId);
	if (Frame == nullptr)
	{
		if (VolumeFrames.Num() >= 8)
		{
			PruneStaleVolumeFrames();
			if (VolumeFrames.Num() >= 8)
			{
				double OldestTime = TNumericLimits<double>::Max();
				uint32 OldestId = 0;
				for (const TPair<uint32, FVolumeFrameAssembly>& Pair : VolumeFrames)
				{
					if (Pair.Value.LastReceivedPlatformSeconds < OldestTime)
					{
						OldestTime = Pair.Value.LastReceivedPlatformSeconds;
						OldestId = Pair.Key;
					}
				}
				VolumeFrames.Remove(OldestId);
			}
		}

		FVolumeFrameAssembly NewFrame;
		NewFrame.FrameId = FrameId;
		NewFrame.SimulationTime = SimulationTime;
		NewFrame.FieldMask = FieldMask;
		NewFrame.LastReceivedPlatformSeconds = FPlatformTime::Seconds();
		Frame = &VolumeFrames.Add(FrameId, MoveTemp(NewFrame));
	}
	else if (Frame->FieldMask != FieldMask || Frame->SimulationTime != SimulationTime)
	{
		VolumeFrames.Remove(FrameId);
		++RejectedVolumePackets;
		return false;
	}

	for (const TPair<uint8, FVolumeFieldAssembly>& Pair : Frame->Fields)
	{
		const FVolumeFieldAssembly& Other = Pair.Value;
		if (FieldId <= 4 && Pair.Key <= 4 && Other.GridSize != FIntVector(GridX, GridY, GridZ))
		{
			VolumeFrames.Remove(FrameId);
			++RejectedVolumePackets;
			return false;
		}
		if (FieldId == 5 && Pair.Key <= 4)
		{
			const FIntVector ExpectedSize(
				(Other.GridSize.X + OccupancyBrick - 1) / OccupancyBrick,
				(Other.GridSize.Y + OccupancyBrick - 1) / OccupancyBrick,
				(Other.GridSize.Z + OccupancyBrick - 1) / OccupancyBrick);
			if (ExpectedSize != FIntVector(GridX, GridY, GridZ))
			{
				VolumeFrames.Remove(FrameId);
				++RejectedVolumePackets;
				return false;
			}
		}
		if (FieldId <= 4 && Pair.Key == 5)
		{
			const uint8 OtherBrick = static_cast<uint8>(Other.Flags >> 8);
			const FIntVector ExpectedSize(
				(GridX + OtherBrick - 1) / OtherBrick,
				(GridY + OtherBrick - 1) / OtherBrick,
				(GridZ + OtherBrick - 1) / OtherBrick);
			if (ExpectedSize != Other.GridSize)
			{
				VolumeFrames.Remove(FrameId);
				++RejectedVolumePackets;
				return false;
			}
		}
	}

	FVolumeFieldAssembly* Field = Frame->Fields.Find(FieldId);
	if (Field == nullptr)
	{
		FVolumeFieldAssembly NewField;
		NewField.GridSize = FIntVector(GridX, GridY, GridZ);
		NewField.FieldCrc32 = FieldCrc32;
		NewField.VoxelFormat = VoxelFormat;
		NewField.FieldId = FieldId;
		NewField.ChannelCount = ChannelCount;
		NewField.Compression = Compression;
		NewField.ChunkCount = ChunkCount;
		NewField.Flags = Flags;
		NewField.EncodedBytes = EncodedBytes;
		NewField.DecodedBytes = DecodedBytes;
		NewField.ValueScale = ValueScale;
		NewField.ValueBias = ValueBias;
		NewField.Encoded.SetNumZeroed(static_cast<int32>(EncodedBytes));
		NewField.ReceivedChunks.Init(false, ChunkCount);
		Field = &Frame->Fields.Add(FieldId, MoveTemp(NewField));
	}
	else if (Field->GridSize != FIntVector(GridX, GridY, GridZ) || Field->FieldCrc32 != FieldCrc32 ||
		Field->VoxelFormat != VoxelFormat || Field->ChannelCount != ChannelCount || Field->Compression != Compression ||
		Field->ChunkCount != ChunkCount || Field->Flags != Flags || Field->EncodedBytes != EncodedBytes ||
		Field->DecodedBytes != DecodedBytes || Field->ValueScale != ValueScale || Field->ValueBias != ValueBias)
	{
		VolumeFrames.Remove(FrameId);
		++RejectedVolumePackets;
		return false;
	}

	const uint8* Payload = Data + 64;
	if (Field->ReceivedChunks[ChunkIndex])
	{
		if (FMemory::Memcmp(Field->Encoded.GetData() + PayloadOffset, Payload, PayloadBytes) != 0)
		{
			VolumeFrames.Remove(FrameId);
			++RejectedVolumePackets;
			return false;
		}
	}
	else
	{
		FMemory::Memcpy(Field->Encoded.GetData() + PayloadOffset, Payload, PayloadBytes);
		Field->ReceivedChunks[ChunkIndex] = true;
		++Field->ReceivedChunkCount;
	}

	if (Field->ReceivedChunkCount == Field->ChunkCount && !Field->IsComplete() && !DecodeCompletedField(*Field))
	{
		VolumeFrames.Remove(FrameId);
		++RejectedVolumePackets;
		return false;
	}

	Frame->LastReceivedPlatformSeconds = FPlatformTime::Seconds();
	++ValidVolumePackets;
	uint16 CompleteMask = 0;
	for (const TPair<uint8, FVolumeFieldAssembly>& Pair : Frame->Fields)
	{
		if (Pair.Value.IsComplete())
		{
			CompleteMask |= static_cast<uint16>(1u << (Pair.Key - 1u));
		}
	}
	if (CompleteMask == Frame->FieldMask)
	{
		PublishCompletedFrame(FrameId);
	}
	return true;
}

bool ASkySimSystem::DecodeCompletedField(FVolumeFieldAssembly& Field) const
{
	if (Field.Compression == 0)
	{
		if (Field.EncodedBytes != Field.DecodedBytes)
		{
			return false;
		}
		Field.Decoded = MoveTemp(Field.Encoded);
	}
	else
	{
		Field.Decoded.Reset(static_cast<int32>(Field.DecodedBytes));
		int32 InputPosition = 0;
		while (InputPosition < Field.Encoded.Num())
		{
			const uint8 Control = Field.Encoded[InputPosition++];
			if ((Control & 0x80u) != 0)
			{
				if (InputPosition >= Field.Encoded.Num())
				{
					return false;
				}
				const int32 RunLength = (Control & 0x7fu) + 3;
				if (Field.Decoded.Num() > static_cast<int32>(Field.DecodedBytes) - RunLength)
				{
					return false;
				}
				Field.Decoded.AddUninitialized(RunLength);
				FMemory::Memset(Field.Decoded.GetData() + Field.Decoded.Num() - RunLength, Field.Encoded[InputPosition++], RunLength);
			}
			else
			{
				const int32 LiteralLength = Control + 1;
				if (InputPosition > Field.Encoded.Num() - LiteralLength || Field.Decoded.Num() > static_cast<int32>(Field.DecodedBytes) - LiteralLength)
				{
					return false;
				}
				Field.Decoded.Append(Field.Encoded.GetData() + InputPosition, LiteralLength);
				InputPosition += LiteralLength;
			}
		}
		if (Field.Decoded.Num() != static_cast<int32>(Field.DecodedBytes))
		{
			return false;
		}
		Field.Encoded.Reset();
	}

	Field.ReceivedChunks.Empty();
	return FCrc::MemCrc32(Field.Decoded.GetData(), Field.Decoded.Num()) == Field.FieldCrc32;
}

void ASkySimSystem::PublishCompletedFrame(uint32 FrameId)
{
	FVolumeFrameAssembly* Frame = VolumeFrames.Find(FrameId);
	if (Frame == nullptr)
	{
		return;
	}
	FVolumeFieldAssembly* Density = Frame->Fields.Find(1);
	if (Density == nullptr || Density->VoxelFormat != 2 || Density->ChannelCount != 1 || !Density->IsComplete())
	{
		VolumeFrames.Remove(FrameId);
		return;
	}

	DensityGridSize = Density->GridSize;
	DensityValueScale = Density->ValueScale;
	DensityValueBias = Density->ValueBias;
	LatestDensityBytes = MoveTemp(Density->Decoded);
	LatestVolumeFrameId = static_cast<int32>(FrameId);
	bHasCompleteVolumeFrame = true;
	++CompleteVolumeFrames;

	uint16 Minimum = TNumericLimits<uint16>::Max();
	uint16 Maximum = 0;
	uint64 Sum = 0;
	const int32 VoxelCount = LatestDensityBytes.Num() / 2;
	for (int32 Index = 0; Index < VoxelCount; ++Index)
	{
		const uint16 Value = ReadU16(LatestDensityBytes.GetData(), Index * 2);
		Minimum = FMath::Min(Minimum, Value);
		Maximum = FMath::Max(Maximum, Value);
		Sum += Value;
	}
	const float Normalizer = 1.0f / 65535.0f;
	DensityMinimum = VoxelCount > 0 ? Minimum * Normalizer : 0.0f;
	DensityMaximum = VoxelCount > 0 ? Maximum * Normalizer : 0.0f;
	DensityMean = VoxelCount > 0 ? static_cast<float>(static_cast<double>(Sum) / VoxelCount) * Normalizer : 0.0f;
	UpdateDensityVolumeTexture();
	if (CompleteVolumeFrames == 1 || CompleteVolumeFrames % 100 == 0)
	{
		UE_LOG(
			LogTemp,
			Display,
			TEXT("SkySim: CLD2 완성 frame=%u grid=%dx%dx%d density[max=%.6f, mean=%.6f, scale=%.6f, bias=%.6f]"),
			FrameId,
			DensityGridSize.X,
			DensityGridSize.Y,
			DensityGridSize.Z,
			DensityMaximum,
			DensityMean,
			DensityValueScale,
			DensityValueBias);
	}

	VolumeFrames.Remove(FrameId);
}

void ASkySimSystem::BuildPresentedDensityBytes()
{
	const int32 SizeX = DensityGridSize.X;
	const int32 SizeY = DensityGridSize.Y;
	const int32 SizeZ = DensityGridSize.Z;
	const int32 VoxelCount = SizeX * SizeY * SizeZ;
	if (SizeX <= 0 || SizeY <= 0 || SizeZ <= 0 || LatestDensityBytes.Num() != VoxelCount * 2)
	{
		PresentedDensityBytes.Reset();
		return;
	}

	DensityPresentationWorking.SetNumUninitialized(VoxelCount);
	DensityPresentationScratchA.SetNumUninitialized(VoxelCount);
	DensityPresentationScratchB.SetNumUninitialized(VoxelCount);
	PresentedDensityBytes.SetNumUninitialized(VoxelCount * 2);

	constexpr float InverseUInt16Maximum = 1.0f / 65535.0f;
	for (int32 Index = 0; Index < VoxelCount; ++Index)
	{
		DensityPresentationWorking[Index] = ReadU16(LatestDensityBytes.GetData(), Index * 2) * InverseUInt16Maximum;
	}

	const auto GetIndex = [SizeX, SizeY](int32 X, int32 Y, int32 Z)
	{
		return (Z * SizeY + Y) * SizeX + X;
	};
	const auto MaxFilterAxis = [SizeX, SizeY, SizeZ, &GetIndex](
		const TArray<float>& Source, TArray<float>& Destination, int32 Axis)
	{
		for (int32 Z = 0; Z < SizeZ; ++Z)
		{
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				for (int32 X = 0; X < SizeX; ++X)
				{
					float Maximum = Source[GetIndex(X, Y, Z)];
					if (Axis == 0)
					{
						if (X > 0) Maximum = FMath::Max(Maximum, Source[GetIndex(X - 1, Y, Z)]);
						if (X + 1 < SizeX) Maximum = FMath::Max(Maximum, Source[GetIndex(X + 1, Y, Z)]);
					}
					else if (Axis == 1)
					{
						if (Y > 0) Maximum = FMath::Max(Maximum, Source[GetIndex(X, Y - 1, Z)]);
						if (Y + 1 < SizeY) Maximum = FMath::Max(Maximum, Source[GetIndex(X, Y + 1, Z)]);
					}
					else
					{
						if (Z > 0) Maximum = FMath::Max(Maximum, Source[GetIndex(X, Y, Z - 1)]);
						if (Z + 1 < SizeZ) Maximum = FMath::Max(Maximum, Source[GetIndex(X, Y, Z + 1)]);
					}
					Destination[GetIndex(X, Y, Z)] = Maximum;
				}
			}
		}
	};

	const int32 SpreadIterations = FMath::Clamp(CloudSpreadIterations, 0, 3);
	const float SpreadStrength = FMath::Clamp(CloudSpreadStrength, 0.0f, 1.0f);
	for (int32 Iteration = 0; Iteration < SpreadIterations && SpreadStrength > 0.0f; ++Iteration)
	{
		// Three separable max passes yield a rounded 3x3x3 neighborhood without
		// adding any texture samples to the heterogeneous-volume ray marcher.
		MaxFilterAxis(DensityPresentationWorking, DensityPresentationScratchA, 0);
		MaxFilterAxis(DensityPresentationScratchA, DensityPresentationScratchB, 1);
		MaxFilterAxis(DensityPresentationScratchB, DensityPresentationScratchA, 2);
		for (int32 Index = 0; Index < VoxelCount; ++Index)
		{
			DensityPresentationWorking[Index] = FMath::Max(
				DensityPresentationWorking[Index], DensityPresentationScratchA[Index] * SpreadStrength);
		}
	}

	const float ShapePower = FMath::Clamp(DensityShapePower, 0.1f, 2.0f);
	const float PresentationGain = FMath::Clamp(DensityPresentationGain, 0.0f, 4.0f);
	for (int32 Index = 0; Index < VoxelCount; ++Index)
	{
		const float ShapedDensity = FMath::Clamp(
			FMath::Pow(FMath::Max(DensityPresentationWorking[Index], 0.0f), ShapePower) * PresentationGain,
			0.0f,
			1.0f);
		WriteU16(PresentedDensityBytes, Index * 2, static_cast<uint16>(FMath::RoundToInt(ShapedDensity * 65535.0f)));
	}
}

void ASkySimSystem::UpdateDensityVolumeTexture()
{
	if (DensityGridSize.X <= 0 || DensityGridSize.Y <= 0 || DensityGridSize.Z <= 0 ||
		LatestDensityBytes.Num() != DensityGridSize.X * DensityGridSize.Y * DensityGridSize.Z * 2)
	{
		return;
	}
	BuildPresentedDensityBytes();
	if (PresentedDensityBytes.Num() != LatestDensityBytes.Num())
	{
		return;
	}

	const bool bNeedsNewTexture = DensityVolumeTexture == nullptr ||
		DensityVolumeTexture->GetSizeX() != DensityGridSize.X ||
		DensityVolumeTexture->GetSizeY() != DensityGridSize.Y ||
		DensityVolumeTexture->GetSizeZ() != DensityGridSize.Z ||
		DensityVolumeTexture->GetPixelFormat() != PF_G16;

	if (bNeedsNewTexture)
	{
		DensityVolumeTexture = UVolumeTexture::CreateTransient(
			DensityGridSize.X,
			DensityGridSize.Y,
			DensityGridSize.Z,
			PF_G16,
			TEXT("SkySimDensityVolume"));
		if (DensityVolumeTexture == nullptr || DensityVolumeTexture->GetPlatformData() == nullptr ||
			DensityVolumeTexture->GetPlatformData()->Mips.IsEmpty())
		{
			DensityVolumeTexture = nullptr;
			return;
		}

		DensityVolumeTexture->SRGB = false;
		DensityVolumeTexture->NeverStream = true;
		DensityVolumeTexture->Filter = TF_Bilinear;
		// The material samples continuous, warped XY coordinates so Wrap blends the
		// last and first density texels at every 20 km seam. Its Z coordinate is
		// clamped to half-texel bounds separately, preventing top-to-ground leakage.
		DensityVolumeTexture->AddressMode = TA_Wrap;
		FTexture2DMipMap& Mip = DensityVolumeTexture->GetPlatformData()->Mips[0];
		void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(MipData, PresentedDensityBytes.GetData(), PresentedDensityBytes.Num());
		Mip.BulkData.Unlock();
		DensityVolumeTexture->UpdateResource();
		UpdateVolumeRenderer();
		return;
	}

	FTextureResource* Resource = DensityVolumeTexture->GetResource();
	if (Resource == nullptr || !Resource->TextureRHI.IsValid())
	{
		return;
	}

	FTextureRHIRef TextureRHI = Resource->TextureRHI;
	TArray<uint8> UploadBytes = PresentedDensityBytes;
	const FIntVector UploadSize = DensityGridSize;
	ENQUEUE_RENDER_COMMAND(SkySimUpdateDensityVolume)(
		[TextureRHI, UploadBytes = MoveTemp(UploadBytes), UploadSize](FRHICommandListImmediate& RHICmdList)
		{
			const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, UploadSize.X, UploadSize.Y, UploadSize.Z);
			RHICmdList.UpdateTexture3D(
				TextureRHI,
				0,
				Region,
				UploadSize.X * 2,
				UploadSize.X * UploadSize.Y * 2,
				UploadBytes.GetData());
			RHICmdList.Transition(FRHITransitionInfo(TextureRHI, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		});
	UpdateVolumeRenderer();
}

void ASkySimSystem::UpdateVolumeRenderer()
{
	if (CloudVolumeComponent == nullptr)
	{
		return;
	}

	CloudVolumeComponent->SetVisibility(bEnableVolumeRendering, true);
	if (!bEnableVolumeRendering)
	{
		return;
	}

	UMaterialInterface* AssignedVolumeMaterial = CloudVolumeComponent->GetMaterial(0);
	if (AssignedVolumeMaterial == nullptr)
	{
		AssignedVolumeMaterial = LoadObject<UMaterialInterface>(
			nullptr,
			TEXT("/Game/SkySim/M_SkySimVolume.M_SkySimVolume"));
		if (AssignedVolumeMaterial != nullptr)
		{
			CloudVolumeComponent->SetMaterial(0, AssignedVolumeMaterial);
		}
	}

	// A material serialized on the placed actor can reach BeginPlay before the
	// component has created its internal MID. Create it deterministically here
	// so the transient density texture and all scalar parameters are bound on
	// the very first complete network frame.
	if (CloudVolumeComponent->MaterialInstanceDynamic == nullptr && AssignedVolumeMaterial != nullptr)
	{
		UMaterialInstanceDynamic* VolumeMaterialInstance =
			UMaterialInstanceDynamic::Create(AssignedVolumeMaterial, this);
		if (VolumeMaterialInstance != nullptr)
		{
			CloudVolumeComponent->SetMaterial(0, VolumeMaterialInstance);
		}
	}

	FVector BaseVolumeSizeCm = RenderVolumeSizeCm;
	if (bUseServerDomainSize && ServerDomainExtentMeters.X > 0.0 &&
		ServerDomainExtentMeters.Y > 0.0 && ServerDomainExtentMeters.Z > 0.0)
	{
		BaseVolumeSizeCm = ServerDomainExtentMeters * FMath::Max(0.001f, UnrealUnitsPerMeter);
	}
	int32 HorizontalTileCount = 1;
	if (bEnableWideCloudWorld)
	{
		if (bAutoHorizontalTileCount)
		{
			const double RequestedExtentCm =
				static_cast<double>(FMath::Clamp(CloudWorldHorizontalExtentKm, 20.0f, 500.0f)) * 100000.0;
			const double BaseHorizontalExtentCm = FMath::Max(1.0, FMath::Min(BaseVolumeSizeCm.X, BaseVolumeSizeCm.Y));
			HorizontalTileCount = FMath::Clamp(
				FMath::CeilToInt(RequestedExtentCm / BaseHorizontalExtentCm), 1, 64);
		}
		else
		{
			HorizontalTileCount = FMath::Clamp(ManualHorizontalTileCount, 1, 64);
		}
	}
	FVector EffectiveVolumeSizeCm = BaseVolumeSizeCm;
	EffectiveVolumeSizeCm.X *= HorizontalTileCount;
	EffectiveVolumeSizeCm.Y *= HorizontalTileCount;
	EffectiveCloudWorldExtentKm = static_cast<float>(
		FMath::Min(EffectiveVolumeSizeCm.X, EffectiveVolumeSizeCm.Y) / 100000.0);
	const float SamplingQuality = HorizontalTileCount > 1
		? FMath::Clamp(WideCloudSamplingQuality, 0.25f, 1.0f)
		: 1.0f;
	// The component resolution below already accounts for tile count and quality.
	// Dividing the ray step by those values again would double-compensate.
	CloudVolumeComponent->StepFactor = 1.0f;
	CloudVolumeComponent->ShadowStepFactor = 2.0f;
	// VolumeResolution controls traversal, occupancy and lighting-cache
	// discretization. Keeping it at the 64^3 server grid while stretching the
	// component to 120 km quantizes the whole sky into 1.875 km blocks. Allocate
	// horizontal render voxels for the visible tile count (scaled by authored
	// quality) so each repeated 20 km tile retains its source-grid detail.
	const int32 HorizontalBakeMultiplier = FMath::Clamp(
		FMath::RoundToInt(HorizontalTileCount * SamplingQuality),
		1,
		HorizontalTileCount);
	FIntVector RenderVolumeResolution = DensityGridSize;
	RenderVolumeResolution.X = FMath::Clamp(
		DensityGridSize.X * HorizontalBakeMultiplier, 1, 1024);
	RenderVolumeResolution.Y = FMath::Clamp(
		DensityGridSize.Y * HorizontalBakeMultiplier, 1, 1024);

	if (DensityGridSize.X > 0 && DensityGridSize.Y > 0 && DensityGridSize.Z > 0)
	{
		CloudVolumeComponent->SetVolumeResolution(RenderVolumeResolution);
		const FVector GridSize(RenderVolumeResolution);
		CloudVolumeComponent->SetRelativeScale3D(EffectiveVolumeSizeCm / GridSize);
		// The simulation origin is the centre of the horizontal domain and its
		// vertical origin is the ground/domain floor.
		CloudVolumeComponent->SetRelativeLocation(
			FVector(-0.5 * EffectiveVolumeSizeCm.X, -0.5 * EffectiveVolumeSizeCm.Y, 0.0));
	}

	UMaterialInstanceDynamic* MaterialInstance = CloudVolumeComponent->MaterialInstanceDynamic;
	if (MaterialInstance != nullptr)
	{
		if (DensityVolumeTexture != nullptr)
		{
			MaterialInstance->SetTextureParameterValue(TEXT("DensityVolume"), DensityVolumeTexture);
			MaterialInstance->SetTextureParameterValue(TEXT("DensityVolumeSecondary"), DensityVolumeTexture);
		}
		if (RenderVolumeResolution.X > 0 && RenderVolumeResolution.Y > 0 && RenderVolumeResolution.Z > 0)
		{
			MaterialInstance->SetVectorParameterValue(
				TEXT("InvVolumeResolution"),
				FLinearColor(
					1.0f / RenderVolumeResolution.X,
					1.0f / RenderVolumeResolution.Y,
					1.0f / RenderVolumeResolution.Z,
					0.0f));
			MaterialInstance->SetVectorParameterValue(
				TEXT("InvDensityTextureResolution"),
				FLinearColor(
					1.0f / DensityGridSize.X,
					1.0f / DensityGridSize.Y,
					1.0f / DensityGridSize.Z,
					0.0f));
		}
		MaterialInstance->SetScalarParameterValue(TEXT("ExtinctionScale"), ExtinctionScale);
		MaterialInstance->SetScalarParameterValue(
			TEXT("DensityValueScale"),
			bUsePhysicalDensityScaleForRendering ? DensityValueScale : 1.0f);
		MaterialInstance->SetScalarParameterValue(
			TEXT("DensityValueBias"),
			bUsePhysicalDensityScaleForRendering ? DensityValueBias : 0.0f);
		MaterialInstance->SetVectorParameterValue(TEXT("CloudAlbedo"), CloudAlbedo);
		MaterialInstance->SetScalarParameterValue(TEXT("DebugEmissionScale"), DebugEmissionScale);
		MaterialInstance->SetScalarParameterValue(TEXT("DetailErosionStrength"), DetailErosionStrength);
		MaterialInstance->SetScalarParameterValue(TEXT("HorizontalTileCount"), static_cast<float>(HorizontalTileCount));
		MaterialInstance->SetScalarParameterValue(TEXT("MacroVariationStrength"), MacroVariationStrength);
		MaterialInstance->SetScalarParameterValue(
			TEXT("DensityCoordinateWarpStrength"), DensityCoordinateWarpStrength);
		MaterialInstance->SetScalarParameterValue(
			TEXT("SecondaryPatternBlendStrength"), SecondaryPatternBlendStrength);
		MaterialInstance->SetVectorParameterValue(
			TEXT("MacroVariationTiling"),
			FLinearColor(MacroVariationTiling.X, MacroVariationTiling.Y, MacroVariationTiling.Z, 0.0f));
		MaterialInstance->SetVectorParameterValue(
			TEXT("SecondaryPatternScale"),
			FLinearColor(SecondaryPatternScale.X, SecondaryPatternScale.Y, SecondaryPatternScale.Z, 0.0f));
		MaterialInstance->SetVectorParameterValue(
			TEXT("SecondaryPatternOffset"),
			FLinearColor(SecondaryPatternOffset.X, SecondaryPatternOffset.Y, SecondaryPatternOffset.Z, 0.0f));
		const double WorldExtentMeters = FMath::Max(1.0, static_cast<double>(EffectiveCloudWorldExtentKm) * 1000.0);
		const double WeatherAdvectionScale = 0.18;
		const double WeatherOffsetX = FMath::Fmod(
			UtcUnixSeconds * static_cast<double>(MeanWindEnuMetersPerSecond.X) * WeatherAdvectionScale /
				WorldExtentMeters,
			1.0);
		const double WeatherOffsetY = FMath::Fmod(
			UtcUnixSeconds * static_cast<double>(MeanWindEnuMetersPerSecond.Y) * WeatherAdvectionScale /
				WorldExtentMeters,
			1.0);
		MaterialInstance->SetVectorParameterValue(
			TEXT("WeatherMapOffset"),
			FLinearColor(
				static_cast<float>(WeatherOffsetX),
				static_cast<float>(WeatherOffsetY),
				0.0f,
				0.0f));
		MaterialInstance->SetVectorParameterValue(
			TEXT("DetailNoiseTiling"),
			FLinearColor(DetailNoiseTiling.X, DetailNoiseTiling.Y, DetailNoiseTiling.Z, 0.0f));
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
		UpdatePreviewLightingFromControls();
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
	UpdatePreviewLightingAtUtc(PreviewUtcUnix);
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
	EditorPreviewUtcUnixSeconds = FMath::Clamp(
		static_cast<double>(UtcDateTime.ToUnixTimestamp()),
		MinimumSkyUtcUnixSeconds,
		MaximumSkyUtcUnixSeconds);
	EditorPreviewLocalDateTime = ControlLocalDateTime;
	bEditorPreviewClockInitialized = true;
}

void ASkySimSystem::UpdatePreviewLightingAtUtc(double PreviewUtcUnix)
{
	if (SunLightComponent == nullptr || SkyLightComponent == nullptr || !bEnableEnvironmentLighting ||
		!FMath::IsFinite(PreviewUtcUnix) || !FMath::IsFinite(ControlLatitudeDegrees) ||
		!FMath::IsFinite(ControlLongitudeDegrees))
	{
		return;
	}
	const FVector PreviewSunDirection = CalculateSunDirectionEnu(
		PreviewUtcUnix, ControlLatitudeDegrees, ControlLongitudeDegrees);
	const FVector PreviewSunDirectionWorld =
		GetActorTransform().TransformVectorNoScale(PreviewSunDirection).GetSafeNormal();
	if (!PreviewSunDirectionWorld.IsNearlyZero())
	{
		SunLightComponent->SetWorldRotation((-PreviewSunDirectionWorld).Rotation());
	}
	const float DaylightFactor = FMath::Sqrt(FMath::Clamp(static_cast<float>(PreviewSunDirection.Z), 0.0f, 1.0f));
	const float NightFactor = FMath::Sqrt(FMath::Clamp(static_cast<float>(-PreviewSunDirection.Z), 0.0f, 1.0f));
	const bool bMoonIsPrimaryForwardLight = bEnableMoonLight && NightFactor > DaylightFactor;
	SunLightComponent->ForwardShadingPriority = bMoonIsPrimaryForwardLight ? 0 : 1;
	if (MoonLightComponent != nullptr)
	{
		MoonLightComponent->ForwardShadingPriority = bMoonIsPrimaryForwardLight ? 1 : 0;
	}
	SunLightComponent->SetIntensity(
		FMath::Max(0.0f, DefaultSunIlluminanceLux) * FMath::Max(0.0f, SunIlluminanceMultiplier) * DaylightFactor);
	SunLightComponent->SetLightColor(SunTint);
	SunLightComponent->SetVolumetricScatteringIntensity(FMath::Max(0.0f, SunVolumetricScatteringIntensity));
	SkyLightComponent->SetIntensity(FMath::Max(0.0f, SkyLightIntensity));
	// Offline editor fallback keeps a useful night preview without pretending to
	// reproduce the server's lunar ephemeris. Live SKS1 always replaces this.
	UpdateMoonLighting(-PreviewSunDirection, FMath::Max(0.0f, DefaultMoonIlluminanceLux) * NightFactor);
}

void ASkySimSystem::PruneStaleVolumeFrames()
{
	const double Now = FPlatformTime::Seconds();
	for (auto Iterator = VolumeFrames.CreateIterator(); Iterator; ++Iterator)
	{
		if (Now - Iterator.Value().LastReceivedPlatformSeconds > 1.0)
		{
			Iterator.RemoveCurrent();
		}
	}
}
