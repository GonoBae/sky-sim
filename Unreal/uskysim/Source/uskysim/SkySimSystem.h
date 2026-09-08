#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "SkySimCloudMotion.h"
#include "SkySimDensityFrame.h"
#include "SkySimDensityPresentation.h"
#include "SkySimVolumeReceiver.h"
#include "SkySimSystem.generated.h"

class FSocket;
class UDirectionalLightComponent;
class UExponentialHeightFogComponent;
class UHeterogeneousVolumeComponent;
class USceneComponent;
class USkyAtmosphereComponent;
class USkyLightComponent;
class UVolumeTexture;

struct FSkySimDensityRenderSlot
{
	FSkySimDensityPresentation Presentation;
	double SourceReceiveSeconds = -1.0;
	double SourceSimulationTime = -1.0;
	uint32 SourceFrameId = 0;
	uint64 SourceRevision = 0;
	uint64 UploadedRevision = 0;

	bool Matches(const FSkySimDensityFrame& Frame) const
	{
		return SourceRevision != 0 && SourceFrameId == Frame.FrameId &&
			SourceSimulationTime == Frame.SimulationTime && SourceReceiveSeconds == Frame.ReceivePlatformSeconds;
	}
};

UENUM(BlueprintType)
enum class ESkySimWeatherPreset : uint8
{
	Natural = 0 UMETA(DisplayName = "Natural"),
	Clear = 1 UMETA(DisplayName = "Clear"),
	Cumulus = 2 UMETA(DisplayName = "Cumulus"),
	Overcast = 3 UMETA(DisplayName = "Overcast"),
	Rain = 4 UMETA(DisplayName = "Rain"),
	Storm = 5 UMETA(DisplayName = "Storm"),
	Snow = 6 UMETA(DisplayName = "Snow"),
	Fog = 7 UMETA(DisplayName = "Fog")
};

UENUM(BlueprintType)
enum class ESkySimControlStatus : uint8
{
	Idle,
	Pending,
	Applied,
	Rejected,
	TimedOut,
	SendFailed
};

UENUM(BlueprintType)
enum class ESkySimCloudLayerType : uint8
{
	Stratiform UMETA(DisplayName = "Stratiform / Sheet"),
	Convective UMETA(DisplayName = "Convective / Cumulus"),
	Cirrus UMETA(DisplayName = "Cirrus / Ice"),
	Fog UMETA(DisplayName = "Fog / Ground Layer")
};

// One physically distinct editable cloud deck. Layer zero keeps its original
// scalar properties on ASkySimSystem for map compatibility; layers 1..3 use
// this struct so they remain compact and easy to author in Details.
USTRUCT(BlueprintType)
struct USKYSIM_API FSkySimCloudLayerSettings
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer", meta = (ToolTip = "Disabled layers are transmitted with zero coverage so all four protocol slots remain deterministic."))
	bool bEnabled = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer")
	ESkySimCloudLayerType Type = ESkySimCloudLayerType::Stratiform;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer", meta = (DisplayName = "Base Altitude (AGL)", ClampMin = "0.0", ClampMax = "99000.0", UIMin = "0.0", UIMax = "12000.0", Units = "m", ForceUnits = "m"))
	float BaseAltitudeAglMeters = 1800.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer", meta = (DisplayName = "Top Altitude (AGL)", ClampMin = "1.0", ClampMax = "99500.0", UIMin = "100.0", UIMax = "16000.0", Units = "m", ForceUnits = "m"))
	float TopAltitudeAglMeters = 3200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer", meta = (ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float Coverage = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer", meta = (DisplayName = "Optical Depth", ClampMin = "0.0", ClampMax = "500.0", UIMin = "0.0", UIMax = "60.0"))
	float OpticalDepth = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer", meta = (DisplayName = "Convective Activity", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float ConvectiveActivity = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer", meta = (DisplayName = "Liquid Fraction", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", AdvancedDisplay))
	float LiquidFraction = 0.9f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Cloud Layer", meta = (DisplayName = "Precipitation (mm/h)", ClampMin = "0.0", ClampMax = "300.0", UIMin = "0.0", UIMax = "100.0", AdvancedDisplay))
	float PrecipitationRateMmPerHour = 0.0f;
};

UCLASS(BlueprintType, Blueprintable)
class USKYSIM_API ASkySimSystem : public AActor
{
	GENERATED_BODY()

public:
	ASkySimSystem();

	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintCallable, Category = "Sky Sim|Network")
	bool StartReceiving();

	UFUNCTION(BlueprintCallable, Category = "Sky Sim|Network")
	void StopReceiving();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Time and Location")
	void ApplyDateTimeAndLocation();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Time and Location")
	void ApplyTimeScale();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Time and Location")
	void PauseSkyTime();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Time and Location")
	void ResumeSkyTime();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Weather")
	void ApplyWeatherPreset();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Weather")
	void ReleaseWeatherToNatural();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Weather|Advanced")
	void ApplyCustomWeatherSettings();

	// Retained so existing maps and Blueprints continue to resolve the original
	// entry point. The function now applies all four authored layer slots.
	UFUNCTION(BlueprintCallable, Category = "Sky Sim|Cloud Authoring", meta = (DeprecatedFunction, DeprecationMessage = "Use ApplyCloudLayerSettings. The legacy function now applies all four layers."))
	void ApplyCustomCumulusSettings();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Cloud Authoring")
	void ApplyCloudLayerSettings();

	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Sky Sim|Network")
	void RequestSkyKeyframe();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Network", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 VolumePort = 7777;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Network", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 SkyStatePort = 7779;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Network")
	FString SkyControlHost = TEXT("127.0.0.1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Network", meta = (ClampMin = "1", ClampMax = "65535"))
	int32 SkyControlPort = 7780;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Network")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Editor Preview", meta = (ToolTip = "Receives live server sky and cloud frames in the level-editor viewport without entering Play mode."))
	bool bPreviewInEditor = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Editor Preview", meta = (EditCondition = "bPreviewInEditor", ToolTip = "Interpolates time between server packets and, when the server is unavailable, continues from the last effective time so sun movement remains visible in the editor."))
	bool bAnimateTimeInEditor = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Editor Preview", meta = (EditCondition = "bPreviewInEditor", ToolTip = "Automatically sends edited date, location, time-scale, weather preset, transition and advanced-weather values after a short debounce."))
	bool bAutoApplySkyControlsInEditor = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Editor Preview", meta = (EditCondition = "bPreviewInEditor", ToolTip = "Makes the placed actor authoritative on the initial connection and after a detected server restart. A normal socket rebind keeps the current runtime clock instead of resending the static authored start time."))
	bool bSyncAuthoringSettingsOnConnect = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Editor Preview", meta = (EditCondition = "bSyncAuthoringSettingsOnConnect", AdvancedDisplay, ToolTip = "Also reapplies the Advanced Weather block on connect. This intentionally changes Natural weather to a manual override."))
	bool bApplyAdvancedWeatherOnConnect = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Editor Preview")
	FDateTime EditorPreviewLocalDateTime;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Debug")
	bool bShowDebugOverlay = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky Sim|Rendering")
	TObjectPtr<UHeterogeneousVolumeComponent> CloudVolumeComponent;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Rendering")
	bool bEnableVolumeRendering = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Rendering", meta = (ClampMin = "1.0"))
	FVector RenderVolumeSizeCm = FVector(2000000.0, 2000000.0, 1400000.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering")
	bool bUseServerDomainSize = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (ToolTip = "Tiles the physical simulation volume across a much wider world-space cloud field without stretching away near detail."))
	bool bEnableWideCloudWorld = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (EditCondition = "bEnableWideCloudWorld", ClampMin = "20.0", ClampMax = "500.0", UIMin = "20.0", UIMax = "250.0", Units = "km"))
	float CloudWorldHorizontalExtentKm = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (EditCondition = "bEnableWideCloudWorld", ToolTip = "Chooses an integer tile count from the requested world extent and the live server domain."))
	bool bAutoHorizontalTileCount = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (EditCondition = "bEnableWideCloudWorld && !bAutoHorizontalTileCount", ClampMin = "1", ClampMax = "64"))
	int32 ManualHorizontalTileCount = 6;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (EditCondition = "bEnableWideCloudWorld", ClampMin = "0.25", ClampMax = "1.0", UIMin = "0.25", UIMax = "1.0", ToolTip = "Horizontal material-bake resolution across the wide field. 0.5 uses half the per-tile bake resolution; 1.0 preserves the original server-grid detail in every repeated tile."))
	float WideCloudSamplingQuality = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (Units = "km"))
	float EffectiveCloudWorldExtentKm = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (DisplayName = "Regional Clear-Sky Strength", ClampMin = "0.0", ClampMax = "0.35", UIMin = "0.0", UIMax = "0.25", ToolTip = "Carves broad, connected clear-air corridors through the cloud field instead of distributing clouds uniformly."))
	float MacroVariationStrength = 0.13f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (DisplayName = "Regional Coverage Scale", ClampMin = "0.01", ToolTip = "Low-frequency weather-map cycles across the complete cloud world. Unequal X/Y values create broad fronts and clear gaps."))
	FVector MacroVariationTiling = FVector(2.15, 1.65, 0.70);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (EditCondition = "bEnableWideCloudWorld", DisplayName = "Large-Scale Position Warp", ClampMin = "0.0", ClampMax = "0.45", UIMin = "0.0", UIMax = "0.3", ToolTip = "Smoothly displaces density coordinates in fractions of one server tile so distant cloud families do not align in rows."))
	float DensityCoordinateWarpStrength = 0.16f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (EditCondition = "bEnableWideCloudWorld", DisplayName = "Secondary Pattern Scale", ToolTip = "Non-integer X/Y scale used by the second density lookup. This breaks the exact 20 km repetition while retaining live simulation detail."))
	FVector SecondaryPatternScale = FVector(0.83, 1.137, 1.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (EditCondition = "bEnableWideCloudWorld", DisplayName = "Secondary Pattern Offset", ToolTip = "Phase offset for the secondary live-density lookup."))
	FVector SecondaryPatternOffset = FVector(0.37, 0.61, 0.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Wide Cloud World", meta = (EditCondition = "bEnableWideCloudWorld", DisplayName = "Pattern De-Tiling Blend", ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Blends a continuously transformed second density sample using the regional weather map. Zero keeps exact repetition; one maximizes de-tiling."))
	float SecondaryPatternBlendStrength = 0.72f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering", meta = (ClampMin = "0.001"))
	float UnrealUnitsPerMeter = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering", meta = (ClampMin = "0.0"))
	float ExtinctionScale = 0.1f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering")
	FLinearColor CloudAlbedo = FLinearColor(0.98f, 0.98f, 0.98f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering", meta = (ClampMin = "0.0"))
	float DebugEmissionScale = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering", meta = (ClampMin = "0.0", ClampMax = "1.0", ToolTip = "Fine edge breakup. Keep this below the regional clear-sky strength to avoid hollow or ring-shaped clouds."))
	float DetailErosionStrength = 0.07f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering", meta = (ClampMin = "1.0"))
	FVector DetailNoiseTiling = FVector(7.13, 5.77, 4.31);

	// Presentation-only shaping applied to the transient Unreal volume texture.
	// The decoded server field and its statistics remain unchanged.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Presentation", meta = (ClampMin = "0", ClampMax = "3"))
	int32 CloudSpreadIterations = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Presentation", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float CloudSpreadStrength = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Presentation", meta = (ClampMin = "0.1", ClampMax = "2.0"))
	float DensityShapePower = 0.92f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Presentation", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float DensityPresentationGain = 1.20f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Presentation", meta = (ToolTip = "Decodes each frame's physical density against a fixed display reference, avoiding brightness changes caused only by network encoding scale."))
	bool bStabilizeDensityScale = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Presentation", meta = (EditCondition = "bStabilizeDensityScale || bUsePhysicalDensityScaleForRendering", ClampMin = "0.001", ClampMax = "100000.0", UIMin = "1.0", UIMax = "64.0", ToolTip = "Physical density mapped to full displayed density before shaping. Higher values make clouds thinner; values above the reference saturate. Also used as the common reference for physical-scale interpolation. This is a visual control, not calibrated extinction."))
	float DensityReferenceScale = 8.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Motion", meta = (ToolTip = "Buffers complete cloud frames and blends them on the GPU in both realtime editor preview and Play."))
	bool bInterpolateCloudFrames = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Motion", meta = (EditCondition = "bInterpolateCloudFrames", ClampMin = "0.0", ClampMax = "1.0", Units = "s", ToolTip = "Playback delay to absorb network jitter. 0.3 seconds suits a 5 Hz stream; increase for slower streams. A stalled stream holds its final cloud frame."))
	float CloudInterpolationDelaySeconds = 0.30f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Motion", meta = (ToolTip = "Uses mean horizontal wind to align interpolation endpoints and advect regional/detail noise. This is an approximation, not full per-voxel velocity reprojection."))
	bool bUseMeanWindCloudMotion = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Rendering|Cloud Presentation", meta = (ToolTip = "Multiplies shaped display density by Density Reference Scale. Both interpolation endpoints are decoded using that common reference. Disabled by default: these values are not calibrated to Unreal inverse-centimetre extinction and may produce opaque clouds."))
	bool bUsePhysicalDensityScaleForRendering = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Time and Location", meta = (DisplayName = "Start / Authoring Local Date and Time", ToolTip = "Authored starting time sent to the server. Runtime time is shown in the read-only Runtime Clock category."))
	FDateTime ControlLocalDateTime = FDateTime(2026, 8, 26, 16, 0, 0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Time and Location", meta = (ClampMin = "-14.0", ClampMax = "14.0"))
	float UtcOffsetHours = 9.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Time and Location", meta = (ClampMin = "-90.0", ClampMax = "90.0", Units = "deg"))
	double ControlLatitudeDegrees = 37.5665;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Time and Location", meta = (ClampMin = "-180.0", ClampMax = "180.0", Units = "deg"))
	double ControlLongitudeDegrees = 126.9780;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Time and Location", meta = (ClampMin = "-500.0", ClampMax = "100000.0", Units = "m"))
	float ControlElevationMeters = 38.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Time and Location", meta = (ClampMin = "-86400.0", ClampMax = "86400.0"))
	float ControlTimeScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather", meta = (ToolTip = "Natural is the time-driven mode: clouds, wind, humidity and precipitation evolve continuously with server UTC. Other presets transition to an authored/manual weather state."))
	ESkySimWeatherPreset ControlWeatherPreset = ESkySimWeatherPreset::Natural;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather", meta = (ClampMin = "0.0", ClampMax = "86400.0", Units = "s"))
	float WeatherTransitionSeconds = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather", meta = (ClampMin = "1"))
	int32 WeatherSeed = 55;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Thermodynamics", meta = (DisplayName = "Surface Temperature", ClampMin = "-70.0", ClampMax = "60.0", Units = "C"))
	float CustomSurfaceTemperatureCelsius = 20.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Thermodynamics", meta = (DisplayName = "Sea-level Pressure (hPa)", ClampMin = "800.0", ClampMax = "1080.0"))
	float CustomSeaLevelPressureHpa = 1013.25f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Thermodynamics", meta = (DisplayName = "Relative Humidity", ClampMin = "0.01", ClampMax = "1.0", UIMin = "0.01", UIMax = "1.0"))
	float CustomRelativeHumidity = 0.65f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Visibility", meta = (DisplayName = "Visibility", ClampMin = "25.0", ClampMax = "200000.0", UIMin = "100.0", UIMax = "100000.0", Units = "m"))
	float CustomVisibilityMeters = 60000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Visibility", meta = (DisplayName = "Aerosol Optical Depth (550 nm)", ClampMin = "0.005", ClampMax = "3.0", UIMin = "0.005", UIMax = "1.0", AdvancedDisplay))
	float CustomAerosolOpticalDepth550Nm = 0.12f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Visibility", meta = (DisplayName = "Ozone (DU)", ClampMin = "100.0", ClampMax = "600.0", AdvancedDisplay))
	float CustomOzoneDobsonUnits = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Wind", meta = (DisplayName = "Mean Wind ENU", Units = "m/s", ToolTip = "East, north and up wind components in metres per second."))
	FVector CustomMeanWindEnuMetersPerSecond = FVector(4.0, 1.0, 0.0);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Wind", meta = (DisplayName = "Gust Speed", ClampMin = "0.0", ClampMax = "200.0", Units = "m/s", ToolTip = "Peak gust speed. It must be at least the mean-wind vector length."))
	float CustomGustSpeedMetersPerSecond = 7.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Precipitation", meta = (DisplayName = "Precipitation Rate (mm/h)", ClampMin = "0.0", ClampMax = "300.0"))
	float CustomPrecipitationRateMmPerHour = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Precipitation", meta = (DisplayName = "Snow Fraction", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float CustomSnowFraction = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Precipitation", meta = (DisplayName = "Surface Wetness", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float CustomSurfaceWetness = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Convection", meta = (DisplayName = "Convective Activity", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float CustomWeatherConvectiveActivity = 0.45f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Advanced|Convection", meta = (DisplayName = "Lightning Activity", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0"))
	float CustomLightningActivity = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring", meta = (DisplayName = "Auto Apply Cloud Layers", ToolTip = "Automatically sends all four authored cloud-layer slots after a short debounce. Applying authored layers intentionally changes Natural weather to a manual override; use Release Weather To Natural to resume time-driven weather."))
	bool bAutoApplyCustomCumulus = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Enabled"))
	bool bCustomCloudLayer0Enabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Type"))
	ESkySimCloudLayerType CustomCloudLayer0Type = ESkySimCloudLayerType::Convective;

	// Coverage is also the server's richness control: a single convective layer
	// requests ceil(Coverage * 20) deterministic cloud groups.
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Coverage", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.05", UIMax = "1.0", ToolTip = "Horizontal sky coverage. This also controls how many cumulus source groups the server requests."))
	float CustomCloudCoverage = 0.60f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Cloud Authoring", meta = (DisplayName = "Estimated Source Count", ToolTip = "Estimated source count for Layer 0 when it is Convective: ceil(Coverage * 20), clamped to 4-20. The server also applies a shared 32-source budget across all enabled layers."))
	int32 EstimatedCloudSourceCount = 12;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Base Altitude (AGL)", ClampMin = "0.0", ClampMax = "99000.0", UIMin = "100.0", UIMax = "5000.0", Units = "m", ForceUnits = "m", ToolTip = "Cloud-base height above local ground. SKC1 conversion uses the latest server elevation, falling back to Control Elevation before a server state arrives."))
	float CustomCloudBaseAltitudeAglMeters = 1200.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Top Altitude (AGL)", ClampMin = "1.0", ClampMax = "99500.0", UIMin = "500.0", UIMax = "10000.0", Units = "m", ForceUnits = "m", ToolTip = "Cloud-top height above the local ground. The base-to-top span controls the vertical cloud size."))
	float CustomCloudTopAltitudeAglMeters = 3600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Optical Depth", ClampMin = "0.0", ClampMax = "500.0", UIMin = "0.0", UIMax = "40.0", ToolTip = "Physical cloud optical depth. Higher values create denser, less transparent clouds."))
	float CustomCloudOpticalDepth = 7.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Convective Activity", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", ToolTip = "Controls vertical development, billowing and updraft strength."))
	float CustomCloudConvectiveActivity = 0.55f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Liquid Fraction", ClampMin = "0.0", ClampMax = "1.0", UIMin = "0.0", UIMax = "1.0", AdvancedDisplay, ToolTip = "Liquid-water phase fraction for the custom cumulus layer."))
	float CustomCloudLiquidFraction = 0.95f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring|Layer 0", meta = (DisplayName = "Precipitation (mm/h)", ClampMin = "0.0", ClampMax = "300.0", UIMin = "0.0", UIMax = "100.0", AdvancedDisplay))
	float CustomCloudPrecipitationRateMmPerHour = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring", meta = (DisplayName = "Layer 1 - Mid Deck"))
	FSkySimCloudLayerSettings CustomCloudLayer1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring", meta = (DisplayName = "Layer 2 - High Deck"))
	FSkySimCloudLayerSettings CustomCloudLayer2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Cloud Authoring", meta = (DisplayName = "Layer 3 - Storm / Optional"))
	FSkySimCloudLayerSettings CustomCloudLayer3;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Lighting")
	bool bEnableEnvironmentLighting = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky Sim|Lighting")
	TObjectPtr<UDirectionalLightComponent> SunLightComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting|Moon", meta = (ToolTip = "Creates a second atmosphere directional light driven by the SKS1 moon direction and ground illuminance."))
	bool bEnableMoonLight = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky Sim|Lighting|Moon")
	TObjectPtr<UDirectionalLightComponent> MoonLightComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting|Moon", meta = (ClampMin = "0.0", Units = "Lux", ToolTip = "Fallback full-moon illuminance used only when no recent SKS1 state is available."))
	float DefaultMoonIlluminanceLux = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting|Moon", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "20.0"))
	float MoonIlluminanceMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting|Moon")
	FLinearColor MoonTint = FLinearColor(0.70f, 0.78f, 1.0f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting|Moon", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0"))
	float MoonVolumetricScatteringIntensity = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting|Moon")
	bool bMoonCastsShadows = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky Sim|Lighting")
	TObjectPtr<USkyLightComponent> SkyLightComponent;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky Sim|Lighting")
	TObjectPtr<USkyAtmosphereComponent> SkyAtmosphereComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering", meta = (ToolTip = "Drives an Exponential Height Fog component from live SKS1 visibility and humidity, or from the authored Advanced Weather values during editor fallback preview."))
	bool bEnableWeatherFog = true;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Sky Sim|Weather|Fog Rendering")
	TObjectPtr<UExponentialHeightFogComponent> WeatherFogComponent;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering", meta = (EditCondition = "bEnableWeatherFog", ClampMin = "0.0", ClampMax = "8.0", UIMin = "0.0", UIMax = "4.0", ToolTip = "Multiplies the density derived from meteorological visibility. A value of 1 preserves the physical visibility conversion."))
	float FogDensityMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering", meta = (EditCondition = "bEnableWeatherFog", ClampMin = "0.0", ClampMax = "2.0", UIMin = "0.0", UIMax = "1.0", ToolTip = "Adds a smooth density boost as relative humidity approaches saturation; visibility remains the primary density control."))
	float FogHumidityDensityBoost = 0.35f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering", meta = (EditCondition = "bEnableWeatherFog", ClampMin = "0.001", ClampMax = "2.0", UIMin = "0.01", UIMax = "1.0"))
	float FogHeightFalloff = 0.20f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering", meta = (EditCondition = "bEnableWeatherFog", ClampMin = "0.0", ClampMax = "1.0"))
	float FogMaximumOpacity = 0.95f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering", meta = (EditCondition = "bEnableWeatherFog", ClampMin = "0.0", ClampMax = "100000.0", Units = "m"))
	float FogStartDistanceMeters = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering", meta = (EditCondition = "bEnableWeatherFog"))
	FLinearColor FogInscatteringTint = FLinearColor(0.72f, 0.80f, 0.90f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering|Volumetric", meta = (EditCondition = "bEnableWeatherFog"))
	bool bEnableVolumetricWeatherFog = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering|Volumetric", meta = (EditCondition = "bEnableWeatherFog && bEnableVolumetricWeatherFog", ClampMin = "0.0", ClampMax = "20.0", UIMin = "0.1", UIMax = "5.0"))
	float VolumetricFogExtinctionScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Weather|Fog Rendering|Volumetric", meta = (EditCondition = "bEnableWeatherFog && bEnableVolumetricWeatherFog", ClampMin = "0.25", ClampMax = "50.0", UIMin = "1.0", UIMax = "20.0", Units = "km"))
	float VolumetricFogViewDistanceKm = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Lighting", meta = (ClampMin = "0.0", Units = "Lux"))
	float DefaultSunIlluminanceLux = 75000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "5.0"))
	float SunIlluminanceMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting")
	FLinearColor SunTint = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Sky Sim|Lighting", meta = (ClampMin = "0.0", UIMin = "0.0", UIMax = "10.0"))
	float SunVolumetricScatteringIntensity = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Sky Sim|Lighting", meta = (ClampMin = "0.0"))
	float SkyLightIntensity = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Status")
	bool bIsReceiving = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Status")
	bool bHasSkyState = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Status")
	int32 SkyStateSequence = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Status")
	int32 LatestVolumeFrameId = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Status")
	int64 ValidSkyPackets = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Status")
	int64 ValidVolumePackets = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Status")
	int64 CompleteVolumeFrames = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Status")
	int64 RejectedVolumePackets = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Control Status")
	ESkySimControlStatus ControlStatus = ESkySimControlStatus::Idle;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Control Status")
	FString ControlStatusMessage = TEXT("Idle");

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Control Status")
	int64 LastControlSession = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Control Status")
	int64 LastControlSequence = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Control Status")
	int32 LastControlResult = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Volume")
	bool bHasCompleteVolumeFrame = false;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Volume")
	FIntVector DensityGridSize = FIntVector::ZeroValue;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Volume")
	float DensityMinimum = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Volume")
	float DensityMaximum = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Volume")
	float DensityMean = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Volume")
	float DensityValueScale = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Volume")
	float DensityValueBias = 0.0f;

	UPROPERTY(VisibleAnywhere, Category = "Sky Sim|Volume", Transient)
	TObjectPtr<UVolumeTexture> DensityVolumeTexture = nullptr;

	UPROPERTY(VisibleAnywhere, Transient, Category = "Sky Sim|Volume")
	TObjectPtr<UVolumeTexture> PreviousDensityVolumeTexture = nullptr;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Rendering|Cloud Motion")
	float CloudFrameBlend = 1.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Rendering|Cloud Motion")
	double DisplayedCloudSimulationSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Weather")
	double UtcUnixSeconds = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Weather")
	float RelativeHumidity = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Weather")
	float VisibilityMeters = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Weather")
	FVector MeanWindEnuMetersPerSecond = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Server State")
	FDateTime ServerUtcDateTime;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Server State")
	FDateTime ServerLocalDateTime;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Server State")
	double ServerLatitudeDegrees = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Server State")
	double ServerLongitudeDegrees = 0.0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Server State")
	float ServerElevationMeters = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Server State")
	float ServerTimeScale = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Runtime Clock", meta = (DisplayName = "Current UTC Date and Time"))
	FDateTime CurrentUtcDateTime;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Runtime Clock", meta = (DisplayName = "Current Local Date and Time"))
	FDateTime CurrentLocalDateTime;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Runtime Clock", meta = (DisplayName = "Current Effective Time Scale"))
	float CurrentEffectiveTimeScale = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Runtime Clock", meta = (DisplayName = "Clock Source", ToolTip = "server_interpolated, server_fallback, authoring_fallback, server_packet, or editor_paused."))
	FString RuntimeClockSource = TEXT("authoring_fallback");

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Runtime Clock", meta = (DisplayName = "Current Sun Elevation", Units = "deg"))
	float CurrentSunElevationDegrees = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Server State")
	FVector ServerDomainExtentMeters = FVector::ZeroVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Server State")
	int32 ServerCloudLayerCount = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Celestial")
	FVector SunDirectionEnu = FVector::UpVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Celestial")
	float SunIlluminanceLux = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Celestial")
	FVector MoonDirectionEnu = FVector::UpVector;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Celestial")
	float MoonIlluminatedFraction = 0.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Sky Sim|Celestial", meta = (Units = "Lux"))
	float MoonIlluminanceLux = 0.0f;

protected:
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Destroyed() override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
	virtual bool ShouldTickIfViewportsOnly() const override;
#endif

private:
#if WITH_DEV_AUTOMATION_TESTS
	friend class FSkySimCloudRenderingIntegrationTest;
#endif
	UPROPERTY()
	TObjectPtr<USceneComponent> SceneRoot;

	FSocket* VolumeSocket = nullptr;
	FSocket* SkyStateSocket = nullptr;
	FSocket* SkyControlSocket = nullptr;
	TArray<uint8> ReceiveBuffer;
	TArray<uint8> PendingControlPacket;
	uint32 ControlSessionId = 0;
	uint32 NextControlSequence = 0;
	uint32 PendingControlSequence = 0;
	double LastControlSendPlatformSeconds = 0.0;
	int32 PendingControlSendAttempts = 0;
	float LastNonZeroTimeScale = 1.0f;
	bool bCustomCumulusApplyScheduled = false;
	double CustomCumulusApplyDuePlatformSeconds = 0.0;
	bool bSkyControlsApplyScheduled = false;
	double SkyControlsApplyDuePlatformSeconds = 0.0;
	bool bUseRuntimeUtcForScheduledSkyControls = false;
	bool bRuntimeClockResyncPending = false;
	uint32 RuntimeClockResyncControlSequence = 0;
	bool bTimeScaleApplyScheduled = false;
	double TimeScaleApplyDuePlatformSeconds = 0.0;
	bool bTimeScaleOverridePending = false;
	uint32 TimeScaleControlSequence = 0;
	bool bWeatherPresetApplyScheduled = false;
	double WeatherPresetApplyDuePlatformSeconds = 0.0;
	bool bCustomWeatherApplyScheduled = false;
	double CustomWeatherApplyDuePlatformSeconds = 0.0;
	double NextEditorReceiverAttemptPlatformSeconds = 0.0;
	double LastSkyStateReceivePlatformSeconds = -1.0;
	double EditorPreviewUtcUnixSeconds = 0.0;
	bool bEditorPreviewClockInitialized = false;
	double RuntimeClockBaseUtcUnixSeconds = 0.0;
	double RuntimeClockBasePlatformSeconds = -1.0;
	float RuntimeClockTimeScale = 1.0f;
	double RuntimeClockLatitudeDegrees = 0.0;
	double RuntimeClockLongitudeDegrees = 0.0;
	bool bRuntimeClockHasServerReference = false;
	double NextRuntimeClockDiagnosticPlatformSeconds = 0.0;
	uint32 LastAcceptedSkyStateSequence = 0;
	int32 ConsecutiveBackwardSkyPackets = 0;
	bool bAwaitingInitialServerSync = true;
#if WITH_EDITOR
	bool bEditorReceiverPausedForPIE = false;
#endif

	FSkySimVolumeReceiver VolumeReceiver;
	FSkySimDensityFrameHistory DensityFrames;
	FSkySimCloudMotion CloudMotion;
	double FrozenCloudPlaybackSeconds = 0.0;
	FSkySimDensityRenderSlot DensityRenderSlots[2];
	UPROPERTY(Transient)
	TArray<TObjectPtr<UVolumeTexture>> DensityTextureSlots;
	FVector CloudDisplacementMeters = FVector::ZeroVector;
	FVector PreviousCloudOffset = FVector::ZeroVector;
	FVector CurrentCloudOffset = FVector::ZeroVector;
	float DisplayDensityValueScale = 1.0f;
	float DisplayDensityValueBias = 0.0f;
	double NextCloudMotionDiagnosticSeconds = 0.0;

	FSocket* CreateBoundSocket(const TCHAR* DebugName, int32 Port) const;
	bool EnsureControlSocket();
	void CloseControlSocket();
	bool SendSkyControlCommand(uint8 Opcode, uint8 EvolutionMode, uint32 TransitionMilliseconds,
		uint32 Preset, uint64 ApplyMask, uint64 ClearMask, uint32 Seed,
		double UtcUnix, double Latitude, double Longitude, float Elevation, float TimeScale,
		const TCHAR* Description);
	bool SendCloudLayerControl();
	bool SendCustomWeatherControl();
	void RefreshEstimatedCloudSourceCount();
	void ScheduleAuthoringSync(bool bPreserveRuntimeUtc = false, double PreservedUtcUnixSeconds = 0.0);
	void ResetEditorPreviewClock();
	void RebaseRuntimeClockFromServer(double ServerUtcUnix, float TimeScale, double LatitudeDegrees,
		double LongitudeDegrees, double ReceivePlatformSeconds);
	void RebaseRuntimeClockTimeScale(float TimeScale);
	double EvaluateRuntimeClockUtc(double PlatformSeconds, bool bAdvance) const;
	void UpdateRuntimeClockAndLighting();
	FVector UpdateSunRotationAtUtc(double PreviewUtcUnix, double LatitudeDegrees, double LongitudeDegrees);
	void UpdatePreviewLightingAtUtc(double PreviewUtcUnix, double LatitudeDegrees, double LongitudeDegrees,
		float FallbackBlend = 1.0f);
	bool SendPendingControlPacket();
	void PumpControlRetry();
	void HandleControlAcknowledgement(uint32 Session, uint32 Sequence, uint8 Result);
	void DrainSocket(FSocket* Socket, bool bSkyState);
	bool ParseSkyState(const uint8* Data, int32 NumBytes);
	bool ParseVolumePacket(const uint8* Data, int32 NumBytes);
	void PublishCompletedFrame(FSkySimDensityFrame&& CompletedFrame);
	void RefreshDensityRendering();
	bool PrepareDensityRenderSlot(const FSkySimDensityFrame& Frame, int32 SlotIndex);
	bool UploadPresentedDensity(const FSkySimDensityPresentation& Presentation, TObjectPtr<UVolumeTexture>& Texture);
	void UpdateCloudMotionMaterialParameters();
	void UpdateVolumeRenderer();
	void UpdateEnvironmentLighting();
	void UpdatePreviewLightingFromControls();
	void UpdateMoonLighting(const FVector& DirectionEnu, float IlluminanceLux);
	void UpdateWeatherFog(float InVisibilityMeters, float InRelativeHumidity);
};
