#include "Modules/ModuleManager.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Factories/MaterialFactoryNew.h"
#include "MaterialEditingLibrary.h"
#include "Engine/VolumeTexture.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionAppendVector.h"
#include "Materials/MaterialExpressionClamp.h"
#include "Materials/MaterialExpressionComponentMask.h"
#include "Materials/MaterialExpressionConstant2Vector.h"
#include "Materials/MaterialExpressionLinearInterpolate.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionOneMinus.h"
#include "Materials/MaterialExpressionSaturate.h"
#include "Materials/MaterialExpressionScalarParameter.h"
#include "Materials/MaterialExpressionSubtract.h"
#include "Materials/MaterialExpressionTextureSampleParameterVolume.h"
#include "Materials/MaterialExpressionTransformPosition.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "Materials/MaterialExpressionWorldPosition.h"
#include "Misc/PackageName.h"
#include "UObject/MetaData.h"
#include "UObject/SavePackage.h"

namespace
{
	const TCHAR* SkySimVolumeMaterialPath = TEXT("/Game/SkySim/M_SkySimVolume.M_SkySimVolume");

	template <typename T>
	T* AddExpression(UMaterial* Material, int32 X, int32 Y)
	{
		return Cast<T>(UMaterialEditingLibrary::CreateMaterialExpression(Material, T::StaticClass(), X, Y));
	}

	UMaterialExpressionComponentMask* AddXYOffset(UMaterial* Material, const TCHAR* Name, int32 X, int32 Y)
	{
		UMaterialExpressionVectorParameter* Parameter = AddExpression<UMaterialExpressionVectorParameter>(Material, X, Y);
		UMaterialExpressionComponentMask* XY = AddExpression<UMaterialExpressionComponentMask>(Material, X + 180, Y);
		if (Parameter == nullptr || XY == nullptr)
		{
			return nullptr;
		}
		Parameter->ParameterName = Name;
		Parameter->DefaultValue = FLinearColor(0.0f, 0.0f, 0.0f, 0.0f);
		XY->R = true;
		XY->G = true;
		XY->B = false;
		XY->A = false;
		XY->Input.Connect(0, Parameter);
		return XY;
	}

	UMaterialExpressionAppendVector* AddOffsetCoordinates(UMaterial* Material, UMaterialExpression* BaseXY,
		UMaterialExpression* OffsetXY, UMaterialExpression* DensityZ, int32 X, int32 Y)
	{
		UMaterialExpressionAdd* Horizontal = AddExpression<UMaterialExpressionAdd>(Material, X, Y);
		UMaterialExpressionAppendVector* Coordinates = AddExpression<UMaterialExpressionAppendVector>(Material, X + 180, Y);
		if (BaseXY == nullptr || OffsetXY == nullptr || DensityZ == nullptr || Horizontal == nullptr || Coordinates == nullptr)
		{
			return nullptr;
		}
		Horizontal->A.Connect(0, BaseXY);
		Horizontal->B.Connect(0, OffsetXY);
		Coordinates->A.Connect(0, Horizontal);
		Coordinates->B.Connect(0, DensityZ);
		return Coordinates;
	}

	void CreateSkySimVolumeMaterial()
	{
		static const TCHAR* GeneratedVersionKey = TEXT("SkySimGeneratedVersion");
		static const TCHAR* GeneratedVersion = TEXT("15");
		UMaterial* Material = LoadObject<UMaterial>(nullptr, SkySimVolumeMaterialPath);
		if (Material != nullptr && FString(Material->GetOutermost()->GetMetaData().GetValue(Material, GeneratedVersionKey)) == GeneratedVersion)
		{
			return;
		}

		if (Material == nullptr)
		{
			UMaterialFactoryNew* Factory = NewObject<UMaterialFactoryNew>();
			IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();
			Material = Cast<UMaterial>(AssetTools.CreateAsset(
				TEXT("M_SkySimVolume"),
				TEXT("/Game/SkySim"),
				UMaterial::StaticClass(),
				Factory));
		}
		if (Material == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("SkySim: 볼륨 머티리얼 에셋을 생성하지 못했습니다."));
			return;
		}

		Material->MaterialDomain = MD_Volume;
		Material->BlendMode = BLEND_Additive;
		Material->bUseMaterialAttributes = false;
		Material->bUsedWithHeterogeneousVolumes = true;
		UMaterialEditingLibrary::DeleteAllMaterialExpressions(Material);

		UMaterialExpressionWorldPosition* WorldPosition = AddExpression<UMaterialExpressionWorldPosition>(Material, -1200, -300);
		UMaterialExpressionTransformPosition* LocalPosition = AddExpression<UMaterialExpressionTransformPosition>(Material, -1000, -300);
		UMaterialExpressionVectorParameter* InvResolution = AddExpression<UMaterialExpressionVectorParameter>(Material, -1000, -100);
		UMaterialExpressionMultiply* VolumeCoordinates = AddExpression<UMaterialExpressionMultiply>(Material, -780, -260);
		UMaterialExpressionComponentMask* HorizontalCoordinates = AddExpression<UMaterialExpressionComponentMask>(Material, -560, -380);
		UMaterialExpressionComponentMask* VerticalCoordinate = AddExpression<UMaterialExpressionComponentMask>(Material, -560, -240);
		UMaterialExpressionScalarParameter* HorizontalTileCount = AddExpression<UMaterialExpressionScalarParameter>(Material, -560, -500);
		UMaterialExpressionMultiply* ContinuousHorizontalCoordinates = AddExpression<UMaterialExpressionMultiply>(Material, -320, -400);
		UMaterialExpressionVectorParameter* InvDensityTextureResolution = AddExpression<UMaterialExpressionVectorParameter>(Material, -320, -620);
		UMaterialExpressionComponentMask* InvDensityTextureResolutionZ = AddExpression<UMaterialExpressionComponentMask>(Material, -100, -620);
		UMaterialExpressionMultiply* HalfDensityTexelZ = AddExpression<UMaterialExpressionMultiply>(Material, 120, -620);
		UMaterialExpressionOneMinus* MaximumDensityZ = AddExpression<UMaterialExpressionOneMinus>(Material, 340, -620);
		UMaterialExpressionClamp* ClampedDensityZ = AddExpression<UMaterialExpressionClamp>(Material, 560, -580);
		UMaterialExpressionAppendVector* TiledVolumeCoordinates = AddExpression<UMaterialExpressionAppendVector>(Material, 120, -360);
		UMaterialExpressionAppendVector* ContinuousVolumeCoordinates = AddExpression<UMaterialExpressionAppendVector>(Material, -100, -220);
		UMaterialExpressionTextureSampleParameterVolume* DensitySample = AddExpression<UMaterialExpressionTextureSampleParameterVolume>(Material, 360, -320);
		UMaterialExpressionScalarParameter* DensityValueScale = AddExpression<UMaterialExpressionScalarParameter>(Material, 360, -120);
		UMaterialExpressionMultiply* ScaledDensity = AddExpression<UMaterialExpressionMultiply>(Material, 1360, -220);
		UMaterialExpressionScalarParameter* DensityValueBias = AddExpression<UMaterialExpressionScalarParameter>(Material, 1360, -60);
		UMaterialExpressionAdd* PhysicalDensity = AddExpression<UMaterialExpressionAdd>(Material, 1580, -180);
		UMaterialExpressionVectorParameter* DetailNoiseTiling = AddExpression<UMaterialExpressionVectorParameter>(Material, -100, 40);
		UMaterialExpressionVectorParameter* DetailPhaseOffset = AddExpression<UMaterialExpressionVectorParameter>(Material, -100, 140);
		UMaterialExpressionAdd* EffectiveDetailTiling = AddExpression<UMaterialExpressionAdd>(Material, 140, 80);
		UMaterialExpressionMultiply* DetailNoiseCoordinates = AddExpression<UMaterialExpressionMultiply>(Material, 360, 80);
		UMaterialExpressionTextureSampleParameterVolume* DetailNoiseSample = AddExpression<UMaterialExpressionTextureSampleParameterVolume>(Material, 580, 80);
		UMaterialExpressionOneMinus* InverseDetailNoise = AddExpression<UMaterialExpressionOneMinus>(Material, 800, 60);
		UMaterialExpressionScalarParameter* DetailErosionStrength = AddExpression<UMaterialExpressionScalarParameter>(Material, 800, 160);
		UMaterialExpressionMultiply* DetailErosion = AddExpression<UMaterialExpressionMultiply>(Material, 1020, 80);
		UMaterialExpressionVectorParameter* MacroVariationTiling = AddExpression<UMaterialExpressionVectorParameter>(Material, 140, 310);
		UMaterialExpressionMultiply* MacroNoiseCoordinates = AddExpression<UMaterialExpressionMultiply>(Material, 360, 310);
		UMaterialExpressionTextureSampleParameterVolume* MacroNoiseSample = AddExpression<UMaterialExpressionTextureSampleParameterVolume>(Material, 580, 310);
		UMaterialExpressionVectorParameter* WeatherMapOffset = AddExpression<UMaterialExpressionVectorParameter>(Material, 140, 440);
		UMaterialExpressionAdd* WeatherMapPosition = AddExpression<UMaterialExpressionAdd>(Material, 360, 440);
		UMaterialExpressionComponentMask* MacroNoiseRG = AddExpression<UMaterialExpressionComponentMask>(Material, 800, 210);
		UMaterialExpressionConstant2Vector* HalfVector = AddExpression<UMaterialExpressionConstant2Vector>(Material, 800, 120);
		UMaterialExpressionSubtract* SignedMacroNoise = AddExpression<UMaterialExpressionSubtract>(Material, 1020, 190);
		UMaterialExpressionScalarParameter* DensityCoordinateWarpStrength = AddExpression<UMaterialExpressionScalarParameter>(Material, 1020, 30);
		UMaterialExpressionMultiply* DensityCoordinateWarp = AddExpression<UMaterialExpressionMultiply>(Material, 1240, 190);
		UMaterialExpressionAdd* WarpedContinuousHorizontalCoordinates = AddExpression<UMaterialExpressionAdd>(Material, 1460, 190);
		UMaterialExpressionOneMinus* InverseMacroNoise = AddExpression<UMaterialExpressionOneMinus>(Material, 800, 290);
		UMaterialExpressionScalarParameter* MacroVariationStrength = AddExpression<UMaterialExpressionScalarParameter>(Material, 800, 390);
		UMaterialExpressionMultiply* MacroErosion = AddExpression<UMaterialExpressionMultiply>(Material, 1020, 310);
		UMaterialExpressionAdd* CombinedErosion = AddExpression<UMaterialExpressionAdd>(Material, 1240, 130);
		UMaterialExpressionSubtract* DetailedDensity = AddExpression<UMaterialExpressionSubtract>(Material, 1240, -100);
		UMaterialExpressionSaturate* ShapedDensity = AddExpression<UMaterialExpressionSaturate>(Material, 1580, -320);
		UMaterialExpressionVectorParameter* SecondaryPatternScale = AddExpression<UMaterialExpressionVectorParameter>(Material, 1240, 560);
		UMaterialExpressionComponentMask* SecondaryPatternScaleXY = AddExpression<UMaterialExpressionComponentMask>(Material, 1460, 560);
		UMaterialExpressionVectorParameter* SecondaryPatternOffset = AddExpression<UMaterialExpressionVectorParameter>(Material, 1240, 680);
		UMaterialExpressionComponentMask* SecondaryPatternOffsetXY = AddExpression<UMaterialExpressionComponentMask>(Material, 1460, 680);
		UMaterialExpressionMultiply* SecondaryScaledHorizontalCoordinates = AddExpression<UMaterialExpressionMultiply>(Material, 1680, 560);
		UMaterialExpressionAdd* SecondaryHorizontalCoordinates = AddExpression<UMaterialExpressionAdd>(Material, 1900, 560);
		UMaterialExpressionAppendVector* SecondaryVolumeCoordinates = AddExpression<UMaterialExpressionAppendVector>(Material, 2120, 520);
		UMaterialExpressionTextureSampleParameterVolume* SecondaryDensitySample = AddExpression<UMaterialExpressionTextureSampleParameterVolume>(Material, 2340, 500);
		UMaterialExpressionScalarParameter* SecondaryPatternBlendStrength = AddExpression<UMaterialExpressionScalarParameter>(Material, 1680, 760);
		UMaterialExpressionMultiply* SecondaryPatternBlend = AddExpression<UMaterialExpressionMultiply>(Material, 1900, 760);
		UMaterialExpressionLinearInterpolate* BlendedDensity = AddExpression<UMaterialExpressionLinearInterpolate>(Material, 2560, 420);
		UMaterialExpressionComponentMask* CurrentDensityOffsetXY = AddXYOffset(Material, TEXT("CurrentDensityOffset"), 1460, -740);
		UMaterialExpressionComponentMask* PreviousDensityOffsetXY = AddXYOffset(Material, TEXT("PreviousDensityOffset"), 1460, -880);
		UMaterialExpressionComponentMask* SecondaryMotionOffsetXY = AddXYOffset(Material, TEXT("SecondaryMotionOffset"), 1460, 900);
		UMaterialExpressionAdd* MovingSecondaryCoordinates = AddExpression<UMaterialExpressionAdd>(Material, 2120, 700);
		UMaterialExpressionAdd* CurrentPrimaryXY = AddExpression<UMaterialExpressionAdd>(Material, 1680, -580);
		UMaterialExpressionAdd* CurrentSecondaryXY = AddExpression<UMaterialExpressionAdd>(Material, 2340, 700);
		UMaterialExpressionAppendVector* PreviousPrimaryCoordinates = AddOffsetCoordinates(Material,
			WarpedContinuousHorizontalCoordinates, PreviousDensityOffsetXY, ClampedDensityZ, 1680, -900);
		UMaterialExpressionAppendVector* PreviousSecondaryCoordinates = AddOffsetCoordinates(Material,
			MovingSecondaryCoordinates, PreviousDensityOffsetXY, ClampedDensityZ, 2340, 920);
		UMaterialExpressionTextureSampleParameterVolume* PreviousDensitySample = AddExpression<UMaterialExpressionTextureSampleParameterVolume>(Material, 2120, -900);
		UMaterialExpressionTextureSampleParameterVolume* PreviousSecondaryDensitySample = AddExpression<UMaterialExpressionTextureSampleParameterVolume>(Material, 2780, 920);
		UMaterialExpressionLinearInterpolate* PreviousBlendedDensity = AddExpression<UMaterialExpressionLinearInterpolate>(Material, 3000, 700);
		UMaterialExpressionScalarParameter* DensityFrameBlend = AddExpression<UMaterialExpressionScalarParameter>(Material, 3000, 1000);
		UMaterialExpressionLinearInterpolate* TemporalDensity = AddExpression<UMaterialExpressionLinearInterpolate>(Material, 3220, 420);
		UMaterialExpressionScalarParameter* ExtinctionScale = AddExpression<UMaterialExpressionScalarParameter>(Material, 1580, -440);
		UMaterialExpressionMultiply* Extinction = AddExpression<UMaterialExpressionMultiply>(Material, 1800, -260);
		UMaterialExpressionVectorParameter* Albedo = AddExpression<UMaterialExpressionVectorParameter>(Material, 1800, -60);
		UMaterialExpressionScalarParameter* EmissionScale = AddExpression<UMaterialExpressionScalarParameter>(Material, 1580, 100);
		UMaterialExpressionMultiply* Emission = AddExpression<UMaterialExpressionMultiply>(Material, 1800, 100);
		UMaterialExpressionVectorParameter* EmissionColor = AddExpression<UMaterialExpressionVectorParameter>(Material, 1580, 260);
		UMaterialExpressionMultiply* ColoredEmission = AddExpression<UMaterialExpressionMultiply>(Material, 2020, 100);

		if (WorldPosition == nullptr || LocalPosition == nullptr || InvResolution == nullptr || VolumeCoordinates == nullptr ||
			HorizontalCoordinates == nullptr || VerticalCoordinate == nullptr || HorizontalTileCount == nullptr ||
			ContinuousHorizontalCoordinates == nullptr || InvDensityTextureResolution == nullptr ||
			InvDensityTextureResolutionZ == nullptr || HalfDensityTexelZ == nullptr || MaximumDensityZ == nullptr || ClampedDensityZ == nullptr ||
			TiledVolumeCoordinates == nullptr || ContinuousVolumeCoordinates == nullptr || DensitySample == nullptr ||
			DensityValueScale == nullptr || ScaledDensity == nullptr || DensityValueBias == nullptr || PhysicalDensity == nullptr ||
			DetailNoiseTiling == nullptr || DetailPhaseOffset == nullptr || EffectiveDetailTiling == nullptr ||
			DetailNoiseCoordinates == nullptr || DetailNoiseSample == nullptr || InverseDetailNoise == nullptr ||
			DetailErosionStrength == nullptr || DetailErosion == nullptr || MacroVariationTiling == nullptr ||
			MacroNoiseCoordinates == nullptr || MacroNoiseSample == nullptr || WeatherMapOffset == nullptr ||
			WeatherMapPosition == nullptr || MacroNoiseRG == nullptr || HalfVector == nullptr ||
			SignedMacroNoise == nullptr || DensityCoordinateWarpStrength == nullptr || DensityCoordinateWarp == nullptr ||
			WarpedContinuousHorizontalCoordinates == nullptr || InverseMacroNoise == nullptr ||
			MacroVariationStrength == nullptr || MacroErosion == nullptr || CombinedErosion == nullptr || DetailedDensity == nullptr ||
			ShapedDensity == nullptr || SecondaryPatternScale == nullptr || SecondaryPatternScaleXY == nullptr ||
			SecondaryPatternOffset == nullptr || SecondaryPatternOffsetXY == nullptr ||
			SecondaryScaledHorizontalCoordinates == nullptr || SecondaryHorizontalCoordinates == nullptr ||
			SecondaryVolumeCoordinates == nullptr || SecondaryDensitySample == nullptr ||
			SecondaryPatternBlendStrength == nullptr || SecondaryPatternBlend == nullptr || BlendedDensity == nullptr ||
			CurrentDensityOffsetXY == nullptr || PreviousDensityOffsetXY == nullptr || SecondaryMotionOffsetXY == nullptr ||
			MovingSecondaryCoordinates == nullptr || CurrentPrimaryXY == nullptr || CurrentSecondaryXY == nullptr ||
			PreviousPrimaryCoordinates == nullptr || PreviousSecondaryCoordinates == nullptr ||
			PreviousDensitySample == nullptr || PreviousSecondaryDensitySample == nullptr || PreviousBlendedDensity == nullptr ||
			DensityFrameBlend == nullptr || TemporalDensity == nullptr || EmissionColor == nullptr || ColoredEmission == nullptr ||
			ExtinctionScale == nullptr ||
			Extinction == nullptr || Albedo == nullptr || EmissionScale == nullptr || Emission == nullptr)
		{
			UE_LOG(LogTemp, Error, TEXT("SkySim: 볼륨 머티리얼 노드를 생성하지 못했습니다."));
			return;
		}

		LocalPosition->TransformSourceType = TRANSFORMPOSSOURCE_World;
		LocalPosition->TransformType = TRANSFORMPOSSOURCE_Local;
		InvResolution->ParameterName = TEXT("InvVolumeResolution");
		InvResolution->DefaultValue = FLinearColor(1.0f / 64.0f, 1.0f / 64.0f, 1.0f / 64.0f, 0.0f);
		HorizontalCoordinates->R = true;
		HorizontalCoordinates->G = true;
		HorizontalCoordinates->B = false;
		HorizontalCoordinates->A = false;
		VerticalCoordinate->R = false;
		VerticalCoordinate->G = false;
		VerticalCoordinate->B = true;
		VerticalCoordinate->A = false;
		HorizontalTileCount->ParameterName = TEXT("HorizontalTileCount");
		HorizontalTileCount->DefaultValue = 1.0f;
		InvDensityTextureResolution->ParameterName = TEXT("InvDensityTextureResolution");
		InvDensityTextureResolution->DefaultValue = FLinearColor(1.0f / 64.0f, 1.0f / 64.0f, 1.0f / 64.0f, 0.0f);
		InvDensityTextureResolutionZ->R = false;
		InvDensityTextureResolutionZ->G = false;
		InvDensityTextureResolutionZ->B = true;
		InvDensityTextureResolutionZ->A = false;
		HalfDensityTexelZ->ConstB = 0.5f;
		DensitySample->ParameterName = TEXT("DensityVolume");
		DensitySample->SamplerType = SAMPLERTYPE_Color;
		DensitySample->MipValueMode = TMVM_MipLevel;
		DensitySample->ConstMipValue = 0;
		DensitySample->Texture = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/DefaultVolumeTexture.DefaultVolumeTexture"));
		DensityValueScale->ParameterName = TEXT("DensityValueScale");
		DensityValueScale->DefaultValue = 1.0f;
		DensityValueBias->ParameterName = TEXT("DensityValueBias");
		DensityValueBias->DefaultValue = 0.0f;
		DetailNoiseTiling->ParameterName = TEXT("DetailNoiseTiling");
		DetailNoiseTiling->DefaultValue = FLinearColor(7.13f, 5.77f, 4.31f, 0.0f);
		DetailPhaseOffset->ParameterName = TEXT("DetailPhaseOffset");
		DetailPhaseOffset->DefaultValue = FLinearColor(0.37f, 0.61f, 0.17f, 0.0f);
		DetailNoiseSample->ParameterName = TEXT("DetailNoiseVolume");
		DetailNoiseSample->MipValueMode = TMVM_MipLevel;
		DetailNoiseSample->ConstMipValue = 2;
		UVolumeTexture* DetailNoiseTexture = LoadObject<UVolumeTexture>(
			nullptr,
			TEXT("/Engine/EngineSky/VolumetricClouds/T_VolumeNoiseErosion32.T_VolumeNoiseErosion32"));
		const bool bHasErosionNoise = DetailNoiseTexture != nullptr;
		if (!bHasErosionNoise)
		{
			UE_LOG(LogTemp, Warning, TEXT("SkySim: 엔진 erosion volume texture를 찾지 못해 detail erosion을 비활성화합니다."));
			DetailNoiseTexture = LoadObject<UVolumeTexture>(
				nullptr,
				TEXT("/Engine/EngineResources/DefaultVolumeTexture.DefaultVolumeTexture"));
		}
		DetailNoiseSample->Texture = DetailNoiseTexture;
		DetailNoiseSample->SamplerType = bHasErosionNoise ? SAMPLERTYPE_Masks : SAMPLERTYPE_Color;
		DetailErosionStrength->ParameterName = TEXT("DetailErosionStrength");
		DetailErosionStrength->DefaultValue = bHasErosionNoise ? 0.07f : 0.0f;
		MacroVariationTiling->ParameterName = TEXT("MacroVariationTiling");
		MacroVariationTiling->DefaultValue = FLinearColor(2.15f, 1.65f, 0.70f, 0.0f);
		WeatherMapOffset->ParameterName = TEXT("WeatherMapOffset");
		WeatherMapOffset->DefaultValue = FLinearColor::Black;
		MacroNoiseSample->ParameterName = TEXT("MacroVariationVolume");
		MacroNoiseSample->MipValueMode = TMVM_MipLevel;
		MacroNoiseSample->ConstMipValue = 0;
		MacroNoiseSample->Texture = DetailNoiseTexture;
		MacroNoiseSample->SamplerType = bHasErosionNoise ? SAMPLERTYPE_Masks : SAMPLERTYPE_Color;
		MacroVariationStrength->ParameterName = TEXT("MacroVariationStrength");
		MacroVariationStrength->DefaultValue = bHasErosionNoise ? 0.13f : 0.0f;
		MacroNoiseRG->R = true;
		MacroNoiseRG->G = true;
		MacroNoiseRG->B = false;
		MacroNoiseRG->A = false;
		HalfVector->R = 0.5f;
		HalfVector->G = 0.5f;
		DensityCoordinateWarpStrength->ParameterName = TEXT("DensityCoordinateWarpStrength");
		DensityCoordinateWarpStrength->DefaultValue = bHasErosionNoise ? 0.16f : 0.0f;
		SecondaryPatternScale->ParameterName = TEXT("SecondaryPatternScale");
		SecondaryPatternScale->DefaultValue = FLinearColor(0.83f, 1.137f, 1.0f, 0.0f);
		SecondaryPatternScaleXY->R = true;
		SecondaryPatternScaleXY->G = true;
		SecondaryPatternScaleXY->B = false;
		SecondaryPatternScaleXY->A = false;
		SecondaryPatternOffset->ParameterName = TEXT("SecondaryPatternOffset");
		SecondaryPatternOffset->DefaultValue = FLinearColor(0.37f, 0.61f, 0.0f, 0.0f);
		SecondaryPatternOffsetXY->R = true;
		SecondaryPatternOffsetXY->G = true;
		SecondaryPatternOffsetXY->B = false;
		SecondaryPatternOffsetXY->A = false;
		SecondaryDensitySample->ParameterName = TEXT("DensityVolumeSecondary");
		SecondaryDensitySample->SamplerType = SAMPLERTYPE_Color;
		SecondaryDensitySample->MipValueMode = TMVM_MipLevel;
		SecondaryDensitySample->ConstMipValue = 0;
		SecondaryDensitySample->Texture = DensitySample->Texture;
		PreviousDensitySample->ParameterName = TEXT("DensityVolumePrevious");
		PreviousDensitySample->SamplerType = SAMPLERTYPE_Color;
		PreviousDensitySample->MipValueMode = TMVM_MipLevel;
		PreviousDensitySample->ConstMipValue = 0;
		PreviousDensitySample->Texture = DensitySample->Texture;
		PreviousSecondaryDensitySample->ParameterName = TEXT("DensityVolumeSecondaryPrevious");
		PreviousSecondaryDensitySample->SamplerType = SAMPLERTYPE_Color;
		PreviousSecondaryDensitySample->MipValueMode = TMVM_MipLevel;
		PreviousSecondaryDensitySample->ConstMipValue = 0;
		PreviousSecondaryDensitySample->Texture = DensitySample->Texture;
		DensityFrameBlend->ParameterName = TEXT("DensityFrameBlend");
		DensityFrameBlend->DefaultValue = 1.0f;
		SecondaryPatternBlendStrength->ParameterName = TEXT("SecondaryPatternBlendStrength");
		SecondaryPatternBlendStrength->DefaultValue = 0.72f;
		ExtinctionScale->ParameterName = TEXT("ExtinctionScale");
		ExtinctionScale->DefaultValue = 0.1f;
		Albedo->ParameterName = TEXT("CloudAlbedo");
		Albedo->DefaultValue = FLinearColor(0.98f, 0.98f, 0.98f, 1.0f);
		EmissionScale->ParameterName = TEXT("DebugEmissionScale");
		EmissionScale->DefaultValue = 0.0f;
		EmissionColor->ParameterName = TEXT("EmissionColor");
		EmissionColor->DefaultValue = FLinearColor::White;

		LocalPosition->Input.Connect(0, WorldPosition);
		VolumeCoordinates->A.Connect(0, LocalPosition);
		VolumeCoordinates->B.Connect(0, InvResolution);
		HorizontalCoordinates->Input.Connect(0, VolumeCoordinates);
		VerticalCoordinate->Input.Connect(0, VolumeCoordinates);
		ContinuousHorizontalCoordinates->A.Connect(0, HorizontalCoordinates);
		ContinuousHorizontalCoordinates->B.Connect(0, HorizontalTileCount);
		InvDensityTextureResolutionZ->Input.Connect(0, InvDensityTextureResolution);
		HalfDensityTexelZ->A.Connect(0, InvDensityTextureResolutionZ);
		MaximumDensityZ->Input.Connect(0, HalfDensityTexelZ);
		ClampedDensityZ->Input.Connect(0, VerticalCoordinate);
		ClampedDensityZ->Min.Connect(0, HalfDensityTexelZ);
		ClampedDensityZ->Max.Connect(0, MaximumDensityZ);
		// Keep XY continuous for texture wrap filtering, and constrain only Z.
		CurrentPrimaryXY->A.Connect(0, WarpedContinuousHorizontalCoordinates);
		CurrentPrimaryXY->B.Connect(0, CurrentDensityOffsetXY);
		TiledVolumeCoordinates->A.Connect(0, CurrentPrimaryXY);
		TiledVolumeCoordinates->B.Connect(0, ClampedDensityZ);
		ContinuousVolumeCoordinates->A.Connect(0, ContinuousHorizontalCoordinates);
		ContinuousVolumeCoordinates->B.Connect(0, VerticalCoordinate);
		DensitySample->Coordinates.Connect(0, TiledVolumeCoordinates);
		DetailNoiseCoordinates->A.Connect(0, ContinuousVolumeCoordinates);
		DetailNoiseCoordinates->B.Connect(0, DetailNoiseTiling);
		EffectiveDetailTiling->A.Connect(0, DetailNoiseCoordinates);
		EffectiveDetailTiling->B.Connect(0, DetailPhaseOffset);
		DetailNoiseSample->Coordinates.Connect(0, EffectiveDetailTiling);
		InverseDetailNoise->Input.Connect(1, DetailNoiseSample);
		DetailErosion->A.Connect(0, InverseDetailNoise);
		DetailErosion->B.Connect(0, DetailErosionStrength);
		// Both weather and detail phase offsets are applied after noise tiling.
		MacroNoiseCoordinates->A.Connect(0, VolumeCoordinates);
		MacroNoiseCoordinates->B.Connect(0, MacroVariationTiling);
		WeatherMapPosition->A.Connect(0, MacroNoiseCoordinates);
		WeatherMapPosition->B.Connect(0, WeatherMapOffset);
		MacroNoiseSample->Coordinates.Connect(0, WeatherMapPosition);
		MacroNoiseRG->Input.Connect(0, MacroNoiseSample);
		SignedMacroNoise->A.Connect(0, MacroNoiseRG);
		SignedMacroNoise->B.Connect(0, HalfVector);
		DensityCoordinateWarp->A.Connect(0, SignedMacroNoise);
		DensityCoordinateWarp->B.Connect(0, DensityCoordinateWarpStrength);
		WarpedContinuousHorizontalCoordinates->A.Connect(0, ContinuousHorizontalCoordinates);
		WarpedContinuousHorizontalCoordinates->B.Connect(0, DensityCoordinateWarp);
		InverseMacroNoise->Input.Connect(1, MacroNoiseSample);
		MacroErosion->A.Connect(0, InverseMacroNoise);
		MacroErosion->B.Connect(0, MacroVariationStrength);
		CombinedErosion->A.Connect(0, DetailErosion);
		CombinedErosion->B.Connect(0, MacroErosion);
		SecondaryPatternScaleXY->Input.Connect(0, SecondaryPatternScale);
		SecondaryPatternOffsetXY->Input.Connect(0, SecondaryPatternOffset);
		SecondaryScaledHorizontalCoordinates->A.Connect(0, WarpedContinuousHorizontalCoordinates);
		SecondaryScaledHorizontalCoordinates->B.Connect(0, SecondaryPatternScaleXY);
		SecondaryHorizontalCoordinates->A.Connect(0, SecondaryScaledHorizontalCoordinates);
		SecondaryHorizontalCoordinates->B.Connect(0, SecondaryPatternOffsetXY);
		// Endpoint offsets are density UVs, not inputs to SecondaryPatternScale.
		MovingSecondaryCoordinates->A.Connect(0, SecondaryHorizontalCoordinates);
		MovingSecondaryCoordinates->B.Connect(0, SecondaryMotionOffsetXY);
		CurrentSecondaryXY->A.Connect(0, MovingSecondaryCoordinates);
		CurrentSecondaryXY->B.Connect(0, CurrentDensityOffsetXY);
		SecondaryVolumeCoordinates->A.Connect(0, CurrentSecondaryXY);
		SecondaryVolumeCoordinates->B.Connect(0, ClampedDensityZ);
		SecondaryDensitySample->Coordinates.Connect(0, SecondaryVolumeCoordinates);
		PreviousDensitySample->Coordinates.Connect(0, PreviousPrimaryCoordinates);
		PreviousSecondaryDensitySample->Coordinates.Connect(0, PreviousSecondaryCoordinates);
		SecondaryPatternBlend->A.Connect(2, MacroNoiseSample);
		SecondaryPatternBlend->B.Connect(0, SecondaryPatternBlendStrength);
		BlendedDensity->A.Connect(1, DensitySample);
		BlendedDensity->B.Connect(1, SecondaryDensitySample);
		BlendedDensity->Alpha.Connect(0, SecondaryPatternBlend);
		PreviousBlendedDensity->A.Connect(1, PreviousDensitySample);
		PreviousBlendedDensity->B.Connect(1, PreviousSecondaryDensitySample);
		PreviousBlendedDensity->Alpha.Connect(0, SecondaryPatternBlend);
		TemporalDensity->A.Connect(0, PreviousBlendedDensity);
		TemporalDensity->B.Connect(0, BlendedDensity);
		TemporalDensity->Alpha.Connect(0, DensityFrameBlend);
		// Broad weather-map erosion and a transformed second live-density lookup
		// create clustered cloudy/clear regions without per-tile hard boundaries.
		DetailedDensity->A.Connect(0, TemporalDensity);
		DetailedDensity->B.Connect(0, CombinedErosion);
		ShapedDensity->Input.Connect(0, DetailedDensity);
		ScaledDensity->A.Connect(0, ShapedDensity);
		ScaledDensity->B.Connect(0, DensityValueScale);
		PhysicalDensity->A.Connect(0, ScaledDensity);
		PhysicalDensity->B.Connect(0, DensityValueBias);
		Extinction->A.Connect(0, PhysicalDensity);
		Extinction->B.Connect(0, ExtinctionScale);
		Emission->A.Connect(0, PhysicalDensity);
		Emission->B.Connect(0, EmissionScale);
		ColoredEmission->A.Connect(0, Emission);
		ColoredEmission->B.Connect(0, EmissionColor);
		Material->GetEditorOnlyData()->SubsurfaceColor.Connect(0, Extinction);
		Material->GetEditorOnlyData()->BaseColor.Connect(0, Albedo);
		Material->GetEditorOnlyData()->EmissiveColor.Connect(0, ColoredEmission);

		UMaterialEditingLibrary::RecompileMaterial(Material);
		Material->MarkPackageDirty();

		UPackage* Package = Material->GetOutermost();
		Package->GetMetaData().SetValue(Material, GeneratedVersionKey, GeneratedVersion);
		const FString Filename = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		SaveArgs.SaveFlags = SAVE_NoError;
		if (UPackage::SavePackage(Package, Material, *Filename, SaveArgs))
		{
			UE_LOG(LogTemp, Display, TEXT("SkySim: /Game/SkySim/M_SkySimVolume 머티리얼을 생성했습니다."));
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("SkySim: 볼륨 머티리얼 저장에 실패했습니다: %s"), *Filename);
		}
	}
}

class FSkySimEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		CreateSkySimVolumeMaterial();
	}
};

IMPLEMENT_MODULE(FSkySimEditorModule, uskysimEditor)
