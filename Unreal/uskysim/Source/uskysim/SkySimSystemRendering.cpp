#include "SkySimSystem.h"

#include "Components/HeterogeneousVolumeComponent.h"
#include "Engine/VolumeTexture.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "RHICommandList.h"
#include "TextureResource.h"

void ASkySimSystem::RefreshDensityRendering()
{
	const double Now = FPlatformTime::Seconds();
	const FVector Wind = bUseMeanWindCloudMotion ? MeanWindEnuMetersPerSecond : FVector::ZeroVector;
	const FSkySimCloudMotionSample Sample = CloudMotion.Sample(
		bIsReceiving ? Now : FrozenCloudPlaybackSeconds,
		bInterpolateCloudFrames ? CloudInterpolationDelaySeconds : 0.0, Wind);
	if (!Sample.IsValid())
	{
		return;
	}

	DensityTextureSlots.SetNum(2);
	// Reuse the old current texture as the new previous texture. Each received
	// endpoint is shaped/uploaded once; ordinary ticks only update uniforms.
	int32 PreviousSlot = 0;
	if (DensityRenderSlots[1].Matches(*Sample.Previous))
	{
		PreviousSlot = 1;
	}
	else if (!DensityRenderSlots[0].Matches(*Sample.Previous) &&
		DensityRenderSlots[0].Matches(*Sample.Current))
	{
		PreviousSlot = 1;
	}
	const int32 CurrentSlot = Sample.Previous == Sample.Current ? PreviousSlot : 1 - PreviousSlot;

	// Never partially overwrite a pair while a transient RHI resource is still
	// being created. Both uploads and all material bindings advance together.
	for (int32 Index = 0; Index < 2; ++Index)
	{
		if (DensityTextureSlots[Index] != nullptr)
		{
			const FTextureResource* Resource = DensityTextureSlots[Index]->GetResource();
			if (Resource == nullptr || !Resource->TextureRHI.IsValid())
			{
				return;
			}
		}
	}
	const uint64 PreviousUpload = DensityRenderSlots[PreviousSlot].UploadedRevision;
	const uint64 CurrentUpload = DensityRenderSlots[CurrentSlot].UploadedRevision;
	if (!PrepareDensityRenderSlot(*Sample.Previous, PreviousSlot) ||
		(CurrentSlot != PreviousSlot && !PrepareDensityRenderSlot(*Sample.Current, CurrentSlot)))
	{
		return;
	}
	const bool bRenderStateChanged = DensityVolumeTexture != DensityTextureSlots[CurrentSlot] ||
		PreviousDensityVolumeTexture != DensityTextureSlots[PreviousSlot] ||
		PreviousUpload != DensityRenderSlots[PreviousSlot].UploadedRevision ||
		CurrentUpload != DensityRenderSlots[CurrentSlot].UploadedRevision;
	DensityVolumeTexture = DensityTextureSlots[CurrentSlot];
	PreviousDensityVolumeTexture = DensityTextureSlots[PreviousSlot];
	CloudFrameBlend = Sample.Alpha;
	DisplayedCloudSimulationSeconds = Sample.SimulationTime;
	CloudDisplacementMeters = Sample.AccumulatedDisplacementMeters;
	const bool bUseCommonDensityReference = bStabilizeDensityScale || bUsePhysicalDensityScaleForRendering;
	DisplayDensityValueScale = bUseCommonDensityReference
		? FMath::Clamp(FMath::IsFinite(DensityReferenceScale) ? DensityReferenceScale : 8.0f, 0.001f, 100000.0f)
		: FMath::Lerp(Sample.Previous->ValueScale, Sample.Current->ValueScale, Sample.Alpha);
	DisplayDensityValueBias = bUseCommonDensityReference ? 0.0f
		: FMath::Lerp(Sample.Previous->ValueBias, Sample.Current->ValueBias, Sample.Alpha);

	// CLD2 uses cells/solver-second, with N-1 cells across the physical domain.
	// UTC and its time scale must not speed up this solver-time interpolation.
	FVector DomainMeters = bUseServerDomainSize ? ServerDomainExtentMeters
		: RenderVolumeSizeCm / FMath::Max(0.001f, UnrealUnitsPerMeter);
	FVector WindUv = FVector::ZeroVector;
	if (FMath::IsFinite(Wind.X) && FMath::IsFinite(Wind.Y) &&
		DomainMeters.X > 0.0 && DomainMeters.Y > 0.0)
	{
		WindUv.X = Wind.X / DomainMeters.X * (Sample.Current->GridSize.X - 1.0) / Sample.Current->GridSize.X;
		WindUv.Y = Wind.Y / DomainMeters.Y * (Sample.Current->GridSize.Y - 1.0) / Sample.Current->GridSize.Y;
	}
	const double FrameSeconds = FMath::Max(0.0, Sample.Current->SimulationTime - Sample.Previous->SimulationTime);
	PreviousCloudOffset = -WindUv * (Sample.Alpha * FrameSeconds);
	CurrentCloudOffset = WindUv * ((1.0 - Sample.Alpha) * FrameSeconds);
	if (bRenderStateChanged)
	{
		UpdateVolumeRenderer();
	}
	else
	{
		UpdateCloudMotionMaterialParameters();
	}
	if (Now >= NextCloudMotionDiagnosticSeconds)
	{
		UE_LOG(LogTemp, Display,
			TEXT("SkySim: cloud_motion prev=%u current=%u alpha=%.4f solver=%.6f buffered=%d phase_m=(%.4f,%.4f) stable_density=%d"),
			Sample.Previous->FrameId, Sample.Current->FrameId, CloudFrameBlend,
			DisplayedCloudSimulationSeconds, CloudMotion.GetBufferedFrameCount(),
			CloudDisplacementMeters.X, CloudDisplacementMeters.Y, bStabilizeDensityScale ? 1 : 0);
		NextCloudMotionDiagnosticSeconds = Now + 1.0;
	}
}

bool ASkySimSystem::PrepareDensityRenderSlot(const FSkySimDensityFrame& Frame, int32 SlotIndex)
{
	FSkySimDensityRenderSlot& Slot = DensityRenderSlots[SlotIndex];
	if (!Slot.Matches(Frame))
	{
		Slot.SourceFrameId = Frame.FrameId;
		Slot.SourceSimulationTime = Frame.SimulationTime;
		Slot.SourceReceiveSeconds = Frame.ReceivePlatformSeconds;
		++Slot.SourceRevision;
	}
	FSkySimDensityPresentationSettings Settings;
	Settings.SpreadIterations = CloudSpreadIterations;
	Settings.SpreadStrength = CloudSpreadStrength;
	Settings.ShapePower = DensityShapePower;
	Settings.Gain = DensityPresentationGain;
	Settings.bNormalizePhysicalDensity = bStabilizeDensityScale || bUsePhysicalDensityScaleForRendering;
	Settings.DensityReferenceScale = DensityReferenceScale;
	Slot.Presentation.Prepare(Frame, Slot.SourceRevision, Settings);
	if (Slot.UploadedRevision != Slot.Presentation.GetRevision() || DensityTextureSlots[SlotIndex] == nullptr)
	{
		if (!UploadPresentedDensity(Slot.Presentation, DensityTextureSlots[SlotIndex]))
		{
			return false;
		}
		Slot.UploadedRevision = Slot.Presentation.GetRevision();
	}
	return true;
}

bool ASkySimSystem::UploadPresentedDensity(
	const FSkySimDensityPresentation& Presentation, TObjectPtr<UVolumeTexture>& Texture)
{
	const TArray<uint8>& PresentedDensityBytes = Presentation.GetBytes();
	const FIntVector UploadGridSize = Presentation.GetGridSize();
	if (PresentedDensityBytes.IsEmpty())
	{
		return false;
	}
	const bool bNeedsNewTexture = Texture == nullptr ||
		Texture->GetSizeX() != UploadGridSize.X || Texture->GetSizeY() != UploadGridSize.Y ||
		Texture->GetSizeZ() != UploadGridSize.Z || Texture->GetPixelFormat() != PF_G16;
	if (bNeedsNewTexture)
	{
		// CreateTransient uses this name for NewObject in the transient package.
		// Reusing a fixed name would replace the other endpoint (or another sky
		// actor's texture) in place and silently collapse interpolation to one field.
		const FName TextureName = MakeUniqueObjectName(
			GetTransientPackage(), UVolumeTexture::StaticClass(), TEXT("SkySimDensityVolume"));
		Texture = UVolumeTexture::CreateTransient(UploadGridSize.X, UploadGridSize.Y, UploadGridSize.Z,
			PF_G16, TextureName);
		if (Texture == nullptr || Texture->GetPlatformData() == nullptr || Texture->GetPlatformData()->Mips.IsEmpty())
		{
			Texture = nullptr;
			return false;
		}
		Texture->SRGB = false;
		Texture->NeverStream = true;
		Texture->Filter = TF_Bilinear;
		// Continuous XY wraps at tile seams; material Z stays inside half texels.
		Texture->AddressMode = TA_Wrap;
		FTexture2DMipMap& Mip = Texture->GetPlatformData()->Mips[0];
		void* MipData = Mip.BulkData.Lock(LOCK_READ_WRITE);
		FMemory::Memcpy(MipData, PresentedDensityBytes.GetData(), PresentedDensityBytes.Num());
		Mip.BulkData.Unlock();
		Texture->UpdateResource();
		return true;
	}
	FTextureResource* Resource = Texture->GetResource();
	if (Resource == nullptr || !Resource->TextureRHI.IsValid())
	{
		return false;
	}
	FTextureRHIRef TextureRHI = Resource->TextureRHI;
	TArray<uint8> UploadBytes = PresentedDensityBytes;
	ENQUEUE_RENDER_COMMAND(SkySimUpdateDensityVolume)(
		[TextureRHI, UploadBytes = MoveTemp(UploadBytes), UploadGridSize](FRHICommandListImmediate& RHICmdList)
		{
			const FUpdateTextureRegion3D Region(0, 0, 0, 0, 0, 0, UploadGridSize.X, UploadGridSize.Y, UploadGridSize.Z);
			RHICmdList.UpdateTexture3D(TextureRHI, 0, Region, UploadGridSize.X * 2,
				UploadGridSize.X * UploadGridSize.Y * 2, UploadBytes.GetData());
			RHICmdList.Transition(FRHITransitionInfo(TextureRHI, ERHIAccess::Unknown, ERHIAccess::SRVMask));
		});
	return true;
}

void ASkySimSystem::UpdateCloudMotionMaterialParameters()
{
	UMaterialInstanceDynamic* Material = CloudVolumeComponent != nullptr
		? CloudVolumeComponent->MaterialInstanceDynamic : nullptr;
	if (Material == nullptr)
	{
		return;
	}
	const auto AsColor = [](const FVector& Value)
	{
		return FLinearColor(Value.X, Value.Y, 0.0f, 0.0f);
	};
	const auto WrappedPhase = [](const FVector& Value)
	{
		// Wrap in final noise/sample space, never before non-integer tiling.
		return FVector(FMath::Fmod(Value.X, 1.0), FMath::Fmod(Value.Y, 1.0), 0.0);
	};
	const FVector BaseExtentMeters = bUseServerDomainSize ? ServerDomainExtentMeters
		: RenderVolumeSizeCm / FMath::Max(0.001f, UnrealUnitsPerMeter);
	FVector TileDisplacement = FVector::ZeroVector;
	if (BaseExtentMeters.X > 0.0 && BaseExtentMeters.Y > 0.0 && DensityGridSize.X > 0 && DensityGridSize.Y > 0)
	{
		TileDisplacement.X = CloudDisplacementMeters.X / BaseExtentMeters.X * (DensityGridSize.X - 1.0) / DensityGridSize.X;
		TileDisplacement.Y = CloudDisplacementMeters.Y / BaseExtentMeters.Y * (DensityGridSize.Y - 1.0) / DensityGridSize.Y;
	}
	const double WorldExtentMeters = FMath::Max(1.0, static_cast<double>(EffectiveCloudWorldExtentKm) * 1000.0);
	// The component always expands both axes by the same integer tile count.
	const double TileCount = FMath::Max(1.0, WorldExtentMeters /
		FMath::Max(1.0, FMath::Min(BaseExtentMeters.X, BaseExtentMeters.Y)));
	Material->SetScalarParameterValue(TEXT("DensityFrameBlend"), CloudFrameBlend);
	Material->SetScalarParameterValue(TEXT("DensityValueScale"),
		bUsePhysicalDensityScaleForRendering ? DisplayDensityValueScale : 1.0f);
	Material->SetScalarParameterValue(TEXT("DensityValueBias"),
		bUsePhysicalDensityScaleForRendering ? DisplayDensityValueBias : 0.0f);
	Material->SetVectorParameterValue(TEXT("PreviousDensityOffset"), AsColor(PreviousCloudOffset));
	Material->SetVectorParameterValue(TEXT("CurrentDensityOffset"), AsColor(CurrentCloudOffset));
	Material->SetVectorParameterValue(TEXT("WeatherMapOffset"),
		AsColor(WrappedPhase(-TileDisplacement / TileCount * MacroVariationTiling)));
	Material->SetVectorParameterValue(TEXT("DetailPhaseOffset"),
		AsColor(WrappedPhase(-TileDisplacement * DetailNoiseTiling)));
	// Compensate the de-tiling scale so secondary features move at the same
	// world speed as the primary field instead of at wind / SecondaryScale.
	Material->SetVectorParameterValue(TEXT("SecondaryMotionOffset"),
		AsColor(WrappedPhase(TileDisplacement * (FVector::OneVector - SecondaryPatternScale))));
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
			MaterialInstance->SetTextureParameterValue(TEXT("DensityVolumePrevious"), PreviousDensityVolumeTexture);
			MaterialInstance->SetTextureParameterValue(TEXT("DensityVolumeSecondaryPrevious"), PreviousDensityVolumeTexture);
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
			bUsePhysicalDensityScaleForRendering ? DisplayDensityValueScale : 1.0f);
		MaterialInstance->SetScalarParameterValue(
			TEXT("DensityValueBias"),
			bUsePhysicalDensityScaleForRendering ? DisplayDensityValueBias : 0.0f);
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
		MaterialInstance->SetVectorParameterValue(
			TEXT("DetailNoiseTiling"),
			FLinearColor(DetailNoiseTiling.X, DetailNoiseTiling.Y, DetailNoiseTiling.Z, 0.0f));
		UpdateCloudMotionMaterialParameters();
	}
}
