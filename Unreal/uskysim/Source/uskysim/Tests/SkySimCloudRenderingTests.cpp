#include "../SkySimSystem.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Components/HeterogeneousVolumeComponent.h"
#include "Engine/Engine.h"
#include "Engine/VolumeTexture.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "RenderingThread.h"
#include "RHI.h"
#include "TextureResource.h"

namespace
{
FSkySimDensityFrame MakeRenderingTestFrame(uint32 Id, double SolverTime, double ReceiveTime,
	uint16 EncodedDensity, float ValueScale)
{
	FSkySimDensityFrame Frame;
	Frame.FrameId = Id;
	Frame.SimulationTime = SolverTime;
	Frame.ReceivePlatformSeconds = ReceiveTime;
	Frame.GridSize = FIntVector(2, 2, 2);
	Frame.ValueScale = ValueScale;
	Frame.Bytes.SetNumUninitialized(16);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Frame.Bytes[Index * 2] = static_cast<uint8>(EncodedDensity);
		Frame.Bytes[Index * 2 + 1] = static_cast<uint8>(EncodedDensity >> 8);
	}
	return Frame;
}
}

// This test intentionally exercises real UVolumeTexture resources and the live
// volume material. Pure CPU tests run separately under NullRHI.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimCloudRenderingIntegrationTest,
	"SkySim.Cloud.Rendering.Integration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudRenderingIntegrationTest::RunTest(const FString& Parameters)
{
	if (GUsingNullRHI || !FApp::CanEverRender())
	{
		AddInfo(TEXT("Skipped real-RHI cloud rendering test: run with -RenderOffscreen and without -NullRHI."));
		return true;
	}
	if (!TestNotNull(TEXT("Engine is available"), GEngine)) return false;

	UWorld::InitializationValues Initialization;
	Initialization.AllowAudioPlayback(false).RequiresHitProxies(false).CreatePhysicsScene(false)
		.CreateNavigation(false).CreateAISystem(false).ShouldSimulatePhysics(false)
		.SetTransactional(false).CreateFXSystem(false);
	const FName WorldName = MakeUniqueObjectName(GetTransientPackage(), UWorld::StaticClass(), TEXT("SkySimRenderingTest"));
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, WorldName, GetTransientPackage(),
		true, ERHIFeatureLevel::Num, &Initialization);
	if (!TestNotNull(TEXT("Transient test world is created"), World)) return false;
	GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
	ON_SCOPE_EXIT
	{
		World->DestroyWorld(false);
		GEngine->DestroyWorldContext(World);
		World->RemoveFromRoot();
		FlushRenderingCommands();
	};

	ASkySimSystem* Actor = World->SpawnActorDeferred<ASkySimSystem>(ASkySimSystem::StaticClass(),
		FTransform::Identity, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!TestNotNull(TEXT("Cloud actor is created"), Actor)) return false;
	// No BeginPlay or sockets are needed. Set authoring values before construction
	// so the test never drives the user's server or creates a wide render volume.
	Actor->bAutoStart = false;
	Actor->bAutoApplyCustomCumulus = false;
	Actor->bAutoApplySkyControlsInEditor = false;
	Actor->bSyncAuthoringSettingsOnConnect = false;
	Actor->bEnableEnvironmentLighting = false;
	Actor->bEnableWeatherFog = false;
	Actor->bEnableVolumetricWeatherFog = false;
	Actor->bShowDebugOverlay = false;
	Actor->bEnableWideCloudWorld = false;
	Actor->bUseServerDomainSize = false;
	Actor->RenderVolumeSizeCm = FVector(10000.0, 20000.0, 10000.0);
	Actor->UnrealUnitsPerMeter = 100.0f;
	Actor->DensityGridSize = FIntVector(2, 2, 2);
	Actor->CloudSpreadIterations = 0;
	Actor->DensityShapePower = 1.0f;
	Actor->DensityPresentationGain = 1.0f;
	Actor->bStabilizeDensityScale = true;
	Actor->DensityReferenceScale = 8.0f;
	Actor->bUsePhysicalDensityScaleForRendering = false;
	Actor->bInterpolateCloudFrames = true;
	Actor->CloudInterpolationDelaySeconds = 0.25f;
	Actor->bUseMeanWindCloudMotion = true;
	Actor->MeanWindEnuMetersPerSecond = FVector(4.0, 2.0, 0.0);
	Actor->MacroVariationTiling = FVector(2.0, 3.0, 1.0);
	Actor->DetailNoiseTiling = FVector(5.0, 7.0, 1.0);
	Actor->SecondaryPatternScale = FVector(0.5, 1.5, 1.0);
	Actor->FinishSpawning(FTransform::Identity);
	Actor->bIsReceiving = false;
	Actor->NextCloudMotionDiagnosticSeconds = TNumericLimits<double>::Max();
	if (!TestNotNull(TEXT("Volume component exists"), Actor->CloudVolumeComponent.Get())) return false;

	const auto TickAt = [Actor](double PlaybackSeconds)
	{
		Actor->FrozenCloudPlaybackSeconds = PlaybackSeconds;
		Actor->Tick(1.0f / 60.0f);
		FlushRenderingCommands();
	};
	const auto CheckPreparedDensity = [this, Actor](uint16 ExpectedValue, const TCHAR* Label)
	{
		for (int32 SlotIndex = 0; SlotIndex < 2; ++SlotIndex)
		{
			const TArray<uint8>& Bytes = Actor->DensityRenderSlots[SlotIndex].Presentation.GetBytes();
			if (!TestEqual(FString::Printf(TEXT("%s: slot %d byte count"), Label, SlotIndex), Bytes.Num(), 16)) continue;
			for (int32 Index = 0; Index < 8; ++Index)
			{
				const uint16 Value = static_cast<uint16>(Bytes[Index * 2]) |
					(static_cast<uint16>(Bytes[Index * 2 + 1]) << 8);
				TestEqual(FString::Printf(TEXT("%s: slot %d voxel %d"), Label, SlotIndex, Index), Value, ExpectedValue);
			}
		}
	};

	Actor->PublishCompletedFrame(MakeRenderingTestFrame(10, 0.0, 100.0, 32768, 8.0f));
	TickAt(100.0);
	UVolumeTexture* TextureA = Actor->DensityVolumeTexture;
	if (!TestNotNull(TEXT("First endpoint creates a texture"), TextureA)) return false;
	if (!TestNotNull(TEXT("First texture resource initializes"), TextureA->GetResource())) return false;
	TestTrue(TEXT("First texture has a real RHI resource"), TextureA->GetResource()->TextureRHI.IsValid());
	TestEqual(TEXT("Texture uses G16 density"), TextureA->GetPixelFormat(), PF_G16);
	TestTrue(TEXT("Single endpoint binds both temporal inputs"), Actor->PreviousDensityVolumeTexture == TextureA);

	Actor->PublishCompletedFrame(MakeRenderingTestFrame(11, 2.0, 101.0, 16384, 16.0f));
	TickAt(100.75);
	UVolumeTexture* TextureB = Actor->DensityVolumeTexture;
	if (!TestNotNull(TEXT("Second endpoint creates a texture"), TextureB)) return false;
	TestTrue(TEXT("Two endpoints have separate textures"), TextureA != TextureB);
	TestTrue(TEXT("First texture is reused as previous"), Actor->PreviousDensityVolumeTexture == TextureA);
	if (!TestNotNull(TEXT("Second texture resource initializes"), TextureB->GetResource())) return false;
	TestTrue(TEXT("Second texture has a real RHI resource"), TextureB->GetResource()->TextureRHI.IsValid());
	UMaterialInstanceDynamic* Material = Actor->CloudVolumeComponent->MaterialInstanceDynamic;
	if (!TestNotNull(TEXT("Live volume material instance exists"), Material)) return false;
	TestTrue(TEXT("Current texture is bound to the material"), Material->K2_GetTextureParameterValue(TEXT("DensityVolume")) == TextureB);
	TestTrue(TEXT("Previous texture is bound to the material"), Material->K2_GetTextureParameterValue(TEXT("DensityVolumePrevious")) == TextureA);
	TestTrue(TEXT("Previous secondary lookup uses previous texture"),
		Material->K2_GetTextureParameterValue(TEXT("DensityVolumeSecondaryPrevious")) == TextureA);
	TestEqual(TEXT("Delayed playback selects the midpoint"), Actor->CloudFrameBlend, 0.5f);
	TestEqual(TEXT("Material receives midpoint blend"), Material->K2_GetScalarParameterValue(TEXT("DensityFrameBlend")), 0.5f);
	TestEqual(TEXT("Displayed solver clock is interpolated"), Actor->DisplayedCloudSimulationSeconds, 1.0);
	TestTrue(TEXT("Previous endpoint samples upstream"), Material->K2_GetVectorParameterValue(TEXT("PreviousDensityOffset"))
		.Equals(FLinearColor(-0.02f, -0.005f, 0.0f, 0.0f), 0.000001f));
	TestTrue(TEXT("Current endpoint samples downstream"), Material->K2_GetVectorParameterValue(TEXT("CurrentDensityOffset"))
		.Equals(FLinearColor(0.02f, 0.005f, 0.0f, 0.0f), 0.000001f));
	TestTrue(TEXT("Regional noise moves with accumulated wind"), Material->K2_GetVectorParameterValue(TEXT("WeatherMapOffset"))
		.Equals(FLinearColor(-0.04f, -0.015f, 0.0f, 0.0f), 0.000001f));
	TestTrue(TEXT("Detail noise phase applies after tiling"), Material->K2_GetVectorParameterValue(TEXT("DetailPhaseOffset"))
		.Equals(FLinearColor(-0.1f, -0.035f, 0.0f, 0.0f), 0.000001f));
	TestTrue(TEXT("Secondary lookup compensates its spatial scale"), Material->K2_GetVectorParameterValue(TEXT("SecondaryMotionOffset"))
		.Equals(FLinearColor(0.01f, -0.0025f, 0.0f, 0.0f), 0.000001f));
	CheckPreparedDensity(32768, TEXT("Different CLD2 scales preserve equal physical density"));

	const uint64 FirstSlotRevision = Actor->DensityRenderSlots[0].Presentation.GetRevision();
	const uint64 SecondSlotRevision = Actor->DensityRenderSlots[1].Presentation.GetRevision();
	TickAt(101.0);
	TestEqual(TEXT("An ordinary tick advances the material blend"), Material->K2_GetScalarParameterValue(TEXT("DensityFrameBlend")), 0.75f);
	TestEqual(TEXT("An ordinary tick does not reshape slot zero"), Actor->DensityRenderSlots[0].Presentation.GetRevision(), FirstSlotRevision);
	TestEqual(TEXT("An ordinary tick does not reshape slot one"), Actor->DensityRenderSlots[1].Presentation.GetRevision(), SecondSlotRevision);
	TestTrue(TEXT("An ordinary tick retains the current texture"), Actor->DensityVolumeTexture == TextureB);

	Actor->PublishCompletedFrame(MakeRenderingTestFrame(12, 4.0, 102.0, 8192, 32.0f));
	TickAt(101.75);
	TestTrue(TEXT("New pair reuses old current texture as previous"), Actor->PreviousDensityVolumeTexture == TextureB);
	TestTrue(TEXT("New endpoint reuses the other texture slot"), Actor->DensityVolumeTexture == TextureA);
	TestTrue(TEXT("Material current binding swaps to reused texture"), Material->K2_GetTextureParameterValue(TEXT("DensityVolume")) == TextureA);
	TestTrue(TEXT("Material previous binding swaps to retained texture"), Material->K2_GetTextureParameterValue(TEXT("DensityVolumePrevious")) == TextureB);
	TestEqual(TEXT("Three received frames remain buffered"), Actor->CloudMotion.GetBufferedFrameCount(), 3);
	TestEqual(TEXT("Next pair midpoint solver clock"), Actor->DisplayedCloudSimulationSeconds, 3.0);
	TestEqual(TEXT("Retained endpoint is not reshaped"), Actor->DensityRenderSlots[1].Presentation.GetRevision(), SecondSlotRevision);
	CheckPreparedDensity(32768, TEXT("Third endpoint stays on the same physical reference"));

	Actor->StopReceiving();
	TestEqual(TEXT("Stopping retains frames for offline presentation edits"), Actor->CloudMotion.GetBufferedFrameCount(), 3);
	// The test drives receipt/playback clocks explicitly; production StopReceiving
	// captures FPlatformTime::Seconds instead of this synthetic value.
	TickAt(101.75);
	const uint64 BeforeGain0 = Actor->DensityRenderSlots[0].Presentation.GetRevision();
	const uint64 BeforeGain1 = Actor->DensityRenderSlots[1].Presentation.GetRevision();
	Actor->DensityPresentationGain = 0.5f;
	TickAt(101.75);
	TestEqual(TEXT("Offline gain edit reshapes slot zero"), Actor->DensityRenderSlots[0].Presentation.GetRevision(), BeforeGain0 + 1);
	TestEqual(TEXT("Offline gain edit reshapes slot one"), Actor->DensityRenderSlots[1].Presentation.GetRevision(), BeforeGain1 + 1);
	TestEqual(TEXT("Offline edit preserves the displayed clock"), Actor->DisplayedCloudSimulationSeconds, 3.0);
	TestEqual(TEXT("Offline edit preserves midpoint blend"), Actor->CloudFrameBlend, 0.5f);
	CheckPreparedDensity(16384, TEXT("Offline half-gain output"));

	// Releasing the test-owned resource is supported by UTexture. A subsequent
	// settings change must remain pending until UpdateResource restores it.
	TextureA->ReleaseResource();
	FlushRenderingCommands();
	const uint64 BeforeRetry0 = Actor->DensityRenderSlots[0].UploadedRevision;
	const uint64 BeforeRetry1 = Actor->DensityRenderSlots[1].UploadedRevision;
	Actor->DensityPresentationGain = 0.75f;
	TickAt(101.75);
	TestEqual(TEXT("Unavailable resource does not acknowledge slot zero upload"), Actor->DensityRenderSlots[0].UploadedRevision, BeforeRetry0);
	TestEqual(TEXT("Unavailable resource does not partially update slot one"), Actor->DensityRenderSlots[1].UploadedRevision, BeforeRetry1);
	TextureA->UpdateResource();
	FlushRenderingCommands();
	TickAt(101.75);
	TestEqual(TEXT("Ready resource retries slot zero upload"), Actor->DensityRenderSlots[0].UploadedRevision, BeforeRetry0 + 1);
	TestEqual(TEXT("Ready resource retries slot one upload"), Actor->DensityRenderSlots[1].UploadedRevision, BeforeRetry1 + 1);
	CheckPreparedDensity(24576, TEXT("Retried three-quarter-gain output"));

	Actor->bStabilizeDensityScale = false;
	Actor->bUsePhysicalDensityScaleForRendering = true;
	TickAt(101.75);
	TestEqual(TEXT("Physical rendering uses one common density scale"), Material->K2_GetScalarParameterValue(TEXT("DensityValueScale")), 8.0f);
	TestEqual(TEXT("Common-reference physical rendering has zero bias"), Material->K2_GetScalarParameterValue(TEXT("DensityValueBias")), 0.0f);
	CheckPreparedDensity(24576, TEXT("Physical mode keeps endpoint normalization enabled"));

	const FVector PhaseBeforeRestart = Actor->CloudDisplacementMeters;
	Actor->PublishCompletedFrame(MakeRenderingTestFrame(0, 0.0, 103.0, 32768, 8.0f));
	TickAt(103.25);
	TestEqual(TEXT("Restart resets the buffered interpolation segment"), Actor->CloudMotion.GetBufferedFrameCount(), 1);
	TestEqual(TEXT("Restart accepts frame ID zero after StopReceiving"), Actor->DensityFrames.GetCurrent().FrameId, uint32(0));
	TestEqual(TEXT("Restart uses the new solver clock"), Actor->DisplayedCloudSimulationSeconds, 0.0);
	TestTrue(TEXT("Restart binds a single endpoint without an old/new blend"), Actor->DensityVolumeTexture == Actor->PreviousDensityVolumeTexture);
	TestTrue(TEXT("Restart does not turn solver reset into a wind phase jump"), Actor->CloudDisplacementMeters.Equals(PhaseBeforeRestart));
	TestFalse(TEXT("Test never opened network reception"), Actor->bIsReceiving);
	TestNull(TEXT("Test never opened a control socket"), Actor->SkyControlSocket);
	AddInfo(TEXT("Real-RHI cloud integration verified texture reuse, material parameters, cache, offline edits and resource retry; no GPU G16 readback performed."));
	return true;
}

#endif
