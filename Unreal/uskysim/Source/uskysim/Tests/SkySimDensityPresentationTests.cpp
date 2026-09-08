#include "../SkySimDensityPresentation.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include <initializer_list>
#include <limits>

namespace
{
	FSkySimDensityFrame MakePresentationTestFrame(FIntVector GridSize, std::initializer_list<uint16> Values)
	{
		FSkySimDensityFrame Frame;
		Frame.GridSize = GridSize;
		for (uint16 Value : Values)
		{
			Frame.Bytes.Add(static_cast<uint8>(Value));
			Frame.Bytes.Add(static_cast<uint8>(Value >> 8));
		}
		return Frame;
	}

	uint16 ReadPresentationTestValue(const TArray<uint8>& Bytes, int32 Index)
	{
		return static_cast<uint16>(Bytes[Index * 2]) | (static_cast<uint16>(Bytes[Index * 2 + 1]) << 8);
	}

	FSkySimDensityPresentationSettings IdentityPresentationSettings()
	{
		FSkySimDensityPresentationSettings Settings;
		Settings.ShapePower = 1.0f;
		Settings.Gain = 1.0f;
		return Settings;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimDensityPresentationIdentityTest,
	"SkySim.Density.Presentation.Identity", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityPresentationIdentityTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrame Frame = MakePresentationTestFrame(FIntVector(5, 1, 1), {0, 1, 32768, 65534, 65535});
	// Presentation keeps the existing normalized G16 interpretation; physical
	// metadata travels with the frame but does not change this shaping pass.
	Frame.ValueScale = 0.25f;
	Frame.ValueBias = 0.1f;
	FSkySimDensityPresentation Presentation;
	TestTrue(TEXT("First frame prepares output"), Presentation.Prepare(Frame, 1, IdentityPresentationSettings()));
	TestTrue(TEXT("Identity keeps every input byte"), Presentation.GetBytes() == Frame.Bytes);
	TestEqual(TEXT("Output grid matches source"), Presentation.GetGridSize(), Frame.GridSize);
	TestEqual(TEXT("First preparation revision"), Presentation.GetRevision(), uint64(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimDensityPresentationSpreadTest,
	"SkySim.Density.Presentation.SpreadEdges", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityPresentationSpreadTest::RunTest(const FString& Parameters)
{
	FSkySimDensityPresentation Presentation;
	FSkySimDensityPresentationSettings Settings = IdentityPresentationSettings();
	Settings.SpreadIterations = 1;
	Settings.SpreadStrength = 0.5f;
	const FSkySimDensityFrame CornerFrame = MakePresentationTestFrame(FIntVector(3, 2, 2),
		{65535, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
	if (!TestTrue(TEXT("Corner frame prepares"), Presentation.Prepare(CornerFrame, 1, Settings))) return false;
	const uint16 ExpectedCorner[] = {65535, 32768, 0, 32768, 32768, 0, 32768, 32768, 0, 32768, 32768, 0};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(ExpectedCorner); ++Index)
	{
		TestEqual(FString::Printf(TEXT("Corner voxel %d does not wrap across edges"), Index),
			ReadPresentationTestValue(Presentation.GetBytes(), Index), ExpectedCorner[Index]);
	}

	const FSkySimDensityFrame LineFrame = MakePresentationTestFrame(FIntVector(5, 1, 1), {0, 0, 65535, 0, 0});
	Settings.SpreadIterations = 2;
	if (!TestTrue(TEXT("Two spread iterations prepare"), Presentation.Prepare(LineFrame, 2, Settings))) return false;
	const uint16 ExpectedLine[] = {16384, 32768, 65535, 32768, 16384};
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(ExpectedLine); ++Index)
	{
		TestEqual(FString::Printf(TEXT("Two-pass line voxel %d"), Index),
			ReadPresentationTestValue(Presentation.GetBytes(), Index), ExpectedLine[Index]);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimDensityPresentationShapeGainTest,
	"SkySim.Density.Presentation.ShapeAndGain", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityPresentationShapeGainTest::RunTest(const FString& Parameters)
{
	const FSkySimDensityFrame Frame = MakePresentationTestFrame(FIntVector(3, 1, 1), {0, 32768, 65535});
	FSkySimDensityPresentation Presentation;
	FSkySimDensityPresentationSettings Settings = IdentityPresentationSettings();
	Settings.ShapePower = 2.0f;
	if (!TestTrue(TEXT("Squared shape prepares"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Squared half density"), ReadPresentationTestValue(Presentation.GetBytes(), 1), uint16(16384));
	Settings.ShapePower = 1.0f;
	Settings.Gain = 2.0f;
	if (!TestTrue(TEXT("Gain change prepares"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Gain keeps zero density"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(0));
	TestEqual(TEXT("Gain saturates half density"), ReadPresentationTestValue(Presentation.GetBytes(), 1), uint16(65535));
	TestEqual(TEXT("Gain saturates full density"), ReadPresentationTestValue(Presentation.GetBytes(), 2), uint16(65535));
	Settings.Gain = -1.0f;
	if (!TestTrue(TEXT("Negative gain clamps"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Clamped zero gain clears density"), ReadPresentationTestValue(Presentation.GetBytes(), 2), uint16(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimDensityPresentationCacheTest,
	"SkySim.Density.Presentation.Cache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityPresentationCacheTest::RunTest(const FString& Parameters)
{
	const FSkySimDensityFrame Frame = MakePresentationTestFrame(FIntVector(1, 1, 1), {32768});
	FSkySimDensityPresentation Presentation;
	FSkySimDensityPresentationSettings Settings = IdentityPresentationSettings();
	TestTrue(TEXT("Initial frame prepares"), Presentation.Prepare(Frame, 1, Settings));
	const uint8* CachedData = Presentation.GetBytes().GetData();
	TestFalse(TEXT("Repeated tick hits cache"), Presentation.Prepare(Frame, 1, Settings));
	TestTrue(TEXT("Cache hit retains output storage"), CachedData == Presentation.GetBytes().GetData());
	TestEqual(TEXT("Cache hit keeps revision"), Presentation.GetRevision(), uint64(1));
	Settings.Gain = 0.0f;
	TestTrue(TEXT("Settings invalidate same frame"), Presentation.Prepare(Frame, 1, Settings));
	TestEqual(TEXT("Settings preparation advances revision"), Presentation.GetRevision(), uint64(2));
	Settings.Gain = -2.0f;
	TestFalse(TEXT("Equivalent clamped settings hit cache"), Presentation.Prepare(Frame, 1, Settings));
	TestTrue(TEXT("New frame revision prepares"), Presentation.Prepare(Frame, 2, Settings));
	TestEqual(TEXT("New frame advances output revision"), Presentation.GetRevision(), uint64(3));
	Presentation.Reset();
	TestTrue(TEXT("Reset clears output"), Presentation.GetBytes().IsEmpty());
	TestEqual(TEXT("Reset clears grid"), Presentation.GetGridSize(), FIntVector::ZeroValue);
	TestTrue(TEXT("Reset invalidates cache"), Presentation.Prepare(Frame, 2, Settings));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimDensityPresentationInvalidInputTest,
	"SkySim.Density.Presentation.InvalidInput", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityPresentationInvalidInputTest::RunTest(const FString& Parameters)
{
	FSkySimDensityPresentation Presentation;
	const FSkySimDensityPresentationSettings Settings = IdentityPresentationSettings();
	const FSkySimDensityFrame ValidFrame = MakePresentationTestFrame(FIntVector(1, 1, 1), {65535});
	TestTrue(TEXT("Valid baseline prepares"), Presentation.Prepare(ValidFrame, 1, Settings));
	FSkySimDensityFrame InvalidFrame = ValidFrame;
	InvalidFrame.GridSize.X = 0;
	TestFalse(TEXT("Zero dimension rejects"), Presentation.Prepare(InvalidFrame, 1, Settings));
	TestTrue(TEXT("Invalid frame clears old bytes even with same revision"), Presentation.GetBytes().IsEmpty());
	TestEqual(TEXT("Invalid frame clears old grid"), Presentation.GetGridSize(), FIntVector::ZeroValue);
	TestEqual(TEXT("Invalid input does not prepare a revision"), Presentation.GetRevision(), uint64(1));
	InvalidFrame.GridSize = FIntVector(-1, 1, 1);
	TestFalse(TEXT("Negative dimension rejects"), Presentation.Prepare(InvalidFrame, 1, Settings));
	InvalidFrame.GridSize = FIntVector(MAX_int32, MAX_int32, MAX_int32);
	TestFalse(TEXT("Huge grid rejects without product overflow"), Presentation.Prepare(InvalidFrame, 1, Settings));
	InvalidFrame.GridSize = FIntVector(2, 1, 1);
	TestFalse(TEXT("Short buffer rejects"), Presentation.Prepare(InvalidFrame, 1, Settings));
	InvalidFrame = ValidFrame;
	InvalidFrame.Bytes.Add(0);
	TestFalse(TEXT("Odd byte count rejects"), Presentation.Prepare(InvalidFrame, 1, Settings));
	TestTrue(TEXT("Valid input after rejection rebuilds"), Presentation.Prepare(ValidFrame, 1, Settings));
	TestTrue(TEXT("Recovery restores correct output"), Presentation.GetBytes() == ValidFrame.Bytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimDensityPresentationPhysicalNormalizationTest,
	"SkySim.Density.Presentation.PhysicalNormalization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityPresentationPhysicalNormalizationTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrame Frame = MakePresentationTestFrame(FIntVector(5, 1, 1), {8192, 16384, 32768, 49152, 65534});
	Frame.ValueScale = 8.0f;
	FSkySimDensityFrame RescaledFrame = MakePresentationTestFrame(FIntVector(5, 1, 1), {4096, 8192, 16384, 24576, 32767});
	RescaledFrame.ValueScale = 16.0f;
	FSkySimDensityFrame BiasedFrame = MakePresentationTestFrame(FIntVector(5, 1, 1), {0, 4096, 12288, 20480, 28671});
	BiasedFrame.ValueScale = 16.0f;
	BiasedFrame.ValueBias = 16.0f * (4096.0f / 65535.0f);
	FSkySimDensityPresentationSettings Settings = IdentityPresentationSettings();
	Settings.bNormalizePhysicalDensity = true;
	Settings.DensityReferenceScale = 8.0f;
	FSkySimDensityPresentation Presentation;
	if (!TestTrue(TEXT("Physical baseline prepares"), Presentation.Prepare(Frame, 1, Settings))) return false;
	const TArray<uint8> BaselineBytes = Presentation.GetBytes();
	TestTrue(TEXT("Reference scale keeps baseline encoding"), BaselineBytes == Frame.Bytes);
	if (!TestTrue(TEXT("Different encoding scale prepares"), Presentation.Prepare(RescaledFrame, 2, Settings))) return false;
	TestTrue(TEXT("Equal physical density has equal displayed density across encoding scales"),
		Presentation.GetBytes() == BaselineBytes);
	if (!TestTrue(TEXT("Different encoding bias prepares"), Presentation.Prepare(BiasedFrame, 3, Settings))) return false;
	TestTrue(TEXT("Equal physical density has equal displayed density across encoding bias"),
		Presentation.GetBytes() == BaselineBytes);
	Settings.bNormalizePhysicalDensity = false;
	if (!TestTrue(TEXT("Disabling normalization prepares legacy presentation"), Presentation.Prepare(BiasedFrame, 3, Settings))) return false;
	TestTrue(TEXT("Disabled normalization preserves legacy encoded G16"), Presentation.GetBytes() == BiasedFrame.Bytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimDensityPresentationPhysicalCacheTest,
	"SkySim.Density.Presentation.PhysicalNormalizationCache", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityPresentationPhysicalCacheTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrame Frame = MakePresentationTestFrame(FIntVector(1, 1, 1), {32768});
	Frame.ValueScale = 8.0f;
	FSkySimDensityPresentationSettings Settings = IdentityPresentationSettings();
	Settings.bNormalizePhysicalDensity = true;
	FSkySimDensityPresentation Presentation;
	TestTrue(TEXT("Physical frame prepares"), Presentation.Prepare(Frame, 1, Settings));
	TestFalse(TEXT("Unchanged physical frame hits cache"), Presentation.Prepare(Frame, 1, Settings));
	Settings.DensityReferenceScale = 16.0f;
	if (!TestTrue(TEXT("Reference scale invalidates same-frame cache"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Doubled reference halves displayed density"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(16384));
	Frame.ValueScale = 16.0f;
	if (!TestTrue(TEXT("Encoding scale metadata invalidates physical cache"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Changed encoding scale is decoded"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(32768));
	Frame.ValueBias = -4.0f;
	if (!TestTrue(TEXT("Encoding bias metadata invalidates physical cache"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Changed encoding bias is decoded"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(16384));
	TestFalse(TEXT("Unchanged metadata hits cache"), Presentation.Prepare(Frame, 1, Settings));
	Settings.bNormalizePhysicalDensity = false;
	if (!TestTrue(TEXT("Mode flag invalidates physical cache"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Legacy presentation ignores physical metadata"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(32768));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimDensityPresentationPhysicalReferenceTest,
	"SkySim.Density.Presentation.PhysicalReferenceSanitization", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityPresentationPhysicalReferenceTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrame Frame = MakePresentationTestFrame(FIntVector(1, 1, 1), {32768});
	Frame.ValueScale = 8.0f;
	FSkySimDensityPresentationSettings Settings = IdentityPresentationSettings();
	Settings.bNormalizePhysicalDensity = true;
	FSkySimDensityPresentation Presentation;
	Settings.DensityReferenceScale = std::numeric_limits<float>::quiet_NaN();
	if (!TestTrue(TEXT("NaN reference prepares using default"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("NaN reference falls back to 8"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(32768));
	Settings.DensityReferenceScale = std::numeric_limits<float>::infinity();
	TestFalse(TEXT("Infinite reference uses same cached default"), Presentation.Prepare(Frame, 1, Settings));
	Settings.DensityReferenceScale = 0.0f;
	if (!TestTrue(TEXT("Zero reference clamps to positive minimum"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Minimum reference produces bounded saturation"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(65535));
	Settings.DensityReferenceScale = -1.0f;
	TestFalse(TEXT("Negative reference shares minimum reference cache"), Presentation.Prepare(Frame, 1, Settings));
	Settings.DensityReferenceScale = std::numeric_limits<float>::max();
	if (!TestTrue(TEXT("Excessive reference clamps to maximum"), Presentation.Prepare(Frame, 1, Settings))) return false;
	TestEqual(TEXT("Maximum reference keeps finite density"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(3));
	Settings.DensityReferenceScale = 100000.0f;
	TestFalse(TEXT("Clamped maximum uses cached output"), Presentation.Prepare(Frame, 1, Settings));
	Frame.ValueBias = -16.0f;
	if (!TestTrue(TEXT("Negative physical density prepares"), Presentation.Prepare(Frame, 2, Settings))) return false;
	TestEqual(TEXT("Negative physical density clamps to zero"), ReadPresentationTestValue(Presentation.GetBytes(), 0), uint16(0));
	return true;
}

#endif
