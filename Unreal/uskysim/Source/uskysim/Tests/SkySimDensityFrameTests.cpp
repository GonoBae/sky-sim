#include "../SkySimDensityFrame.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
FSkySimDensityFrame MakeDensityFrame(
	uint32 FrameId,
	double SimulationTime,
	double ReceivePlatformSeconds,
	FIntVector GridSize = FIntVector(2, 1, 1))
{
	FSkySimDensityFrame Frame;
	Frame.FrameId = FrameId;
	Frame.SimulationTime = SimulationTime;
	Frame.ReceivePlatformSeconds = ReceivePlatformSeconds;
	Frame.GridSize = GridSize;
	Frame.ValueScale = 2.5f;
	Frame.ValueBias = 0.125f;
	Frame.Bytes.SetNumUninitialized(GridSize.X * GridSize.Y * GridSize.Z * 2);
	for (int32 Index = 0; Index < Frame.Bytes.Num(); ++Index)
	{
		Frame.Bytes[Index] = static_cast<uint8>(FrameId + Index);
	}
	return Frame;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimDensityFrameMetadataTest,
	"SkySim.Cloud.DensityFrame.PreservesMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityFrameMetadataTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrameHistory History;
	FSkySimDensityFrame Frame = MakeDensityFrame(41, 12.75, 100.25, FIntVector(2, 3, 1));
	const TArray<uint8> ExpectedBytes = Frame.Bytes;
	TestTrue(TEXT("Publish accepts a complete frame"), History.Publish(MoveTemp(Frame)));
	const FSkySimDensityFrame& Current = History.GetCurrent();
	TestTrue(TEXT("Current remains valid"), Current.IsValid());
	TestEqual(TEXT("Frame ID"), Current.FrameId, uint32(41));
	TestEqual(TEXT("Simulation time"), Current.SimulationTime, 12.75);
	TestEqual(TEXT("Receive platform time"), Current.ReceivePlatformSeconds, 100.25);
	TestEqual(TEXT("Grid size"), Current.GridSize, FIntVector(2, 3, 1));
	TestEqual(TEXT("Value scale"), Current.ValueScale, 2.5f);
	TestEqual(TEXT("Value bias"), Current.ValueBias, 0.125f);
	TestTrue(TEXT("Decoded bytes are preserved exactly"), Current.Bytes == ExpectedBytes);
	TestFalse(TEXT("First publish has no previous frame"), History.GetPrevious().IsValid());
	TestEqual(TEXT("First revision"), History.GetRevision(), uint64(1));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimDensityFrameHistoryTest,
	"SkySim.Cloud.DensityFrame.CompatibleHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityFrameHistoryTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrameHistory History;
	FSkySimDensityFrame First = MakeDensityFrame(10, 1.0, 100.0);
	const TArray<uint8> FirstBytes = First.Bytes;
	TestTrue(TEXT("First publish"), History.Publish(MoveTemp(First)));
	FSkySimDensityFrame Second = MakeDensityFrame(11, 1.2, 100.2);
	Second.ValueScale = 4.0f;
	Second.ValueBias = 0.25f;
	const TArray<uint8> SecondBytes = Second.Bytes;
	TestTrue(TEXT("Second publish"), History.Publish(MoveTemp(Second)));
	TestTrue(TEXT("Previous is valid"), History.GetPrevious().IsValid());
	TestEqual(TEXT("Previous frame ID"), History.GetPrevious().FrameId, uint32(10));
	TestTrue(TEXT("Previous bytes survive publication"), History.GetPrevious().Bytes == FirstBytes);
	TestEqual(TEXT("Previous scale is not overwritten"), History.GetPrevious().ValueScale, 2.5f);
	TestEqual(TEXT("Previous bias is not overwritten"), History.GetPrevious().ValueBias, 0.125f);
	TestEqual(TEXT("Previous simulation time"), History.GetPrevious().SimulationTime, 1.0);
	TestEqual(TEXT("Previous receive time"), History.GetPrevious().ReceivePlatformSeconds, 100.0);
	TestEqual(TEXT("Current frame ID"), History.GetCurrent().FrameId, uint32(11));
	TestTrue(TEXT("Current has the second bytes"), History.GetCurrent().Bytes == SecondBytes);
	TestEqual(TEXT("Current scale"), History.GetCurrent().ValueScale, 4.0f);
	TestEqual(TEXT("Current bias"), History.GetCurrent().ValueBias, 0.25f);
	TestEqual(TEXT("Second revision"), History.GetRevision(), uint64(2));
	TestTrue(TEXT("Third publish"), History.Publish(MakeDensityFrame(12, 1.4, 100.4)));
	TestEqual(TEXT("Previous advances to the second frame"), History.GetPrevious().FrameId, uint32(11));
	TestTrue(TEXT("Second bytes remain in previous"), History.GetPrevious().Bytes == SecondBytes);
	TestEqual(TEXT("Third revision"), History.GetRevision(), uint64(3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimDensityFrameInvalidPublishTest,
	"SkySim.Cloud.DensityFrame.InvalidPublish",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityFrameInvalidPublishTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrameHistory History;
	TestTrue(TEXT("First publish"), History.Publish(MakeDensityFrame(10, 1.0, 100.0)));
	TestTrue(TEXT("Second publish"), History.Publish(MakeDensityFrame(11, 1.2, 100.2)));
	const TArray<uint8> CurrentBytes = History.GetCurrent().Bytes;
	const TArray<uint8> PreviousBytes = History.GetPrevious().Bytes;
	FSkySimDensityFrame Invalid = MakeDensityFrame(12, 1.4, 100.4);
	Invalid.Bytes.Reset();
	TestFalse(TEXT("Incomplete frame is rejected"), History.Publish(MoveTemp(Invalid)));
	TestEqual(TEXT("Rejected publish does not advance revision"), History.GetRevision(), uint64(2));
	TestEqual(TEXT("Current ID is retained"), History.GetCurrent().FrameId, uint32(11));
	TestTrue(TEXT("Current bytes are retained"), History.GetCurrent().Bytes == CurrentBytes);
	TestEqual(TEXT("Previous ID is retained"), History.GetPrevious().FrameId, uint32(10));
	TestTrue(TEXT("Previous bytes are retained"), History.GetPrevious().Bytes == PreviousBytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimDensityFrameDiscontinuityTest,
	"SkySim.Cloud.DensityFrame.Discontinuities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityFrameDiscontinuityTest::RunTest(const FString& Parameters)
{
	const TCHAR* Labels[] = {
		TEXT("Grid change"), TEXT("Backward receive time")
	};
	TArray<FSkySimDensityFrame> Frames;
	Frames.Add(MakeDensityFrame(12, 3.0, 102.0, FIntVector(1, 2, 1)));
	Frames.Add(MakeDensityFrame(12, 3.0, 100.0));
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		FSkySimDensityFrameHistory History;
		History.Publish(MakeDensityFrame(10, 1.0, 100.0));
		History.Publish(MakeDensityFrame(11, 2.0, 101.0));
		const uint32 ExpectedId = Frames[Index].FrameId;
		TestTrue(FString::Printf(TEXT("%s: valid incoming frame is published"), Labels[Index]),
			History.Publish(MoveTemp(Frames[Index])));
		TestTrue(FString::Printf(TEXT("%s: current stays valid"), Labels[Index]), History.GetCurrent().IsValid());
		TestEqual(FString::Printf(TEXT("%s: current is the incoming frame"), Labels[Index]),
			History.GetCurrent().FrameId, ExpectedId);
		TestFalse(FString::Printf(TEXT("%s: previous is cleared"), Labels[Index]), History.GetPrevious().IsValid());
		TestEqual(FString::Printf(TEXT("%s: revision advances"), Labels[Index]), History.GetRevision(), uint64(3));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimDensityFrameOrderedAcceptanceTest,
	"SkySim.Cloud.DensityFrame.OrderedAcceptance",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityFrameOrderedAcceptanceTest::RunTest(const FString& Parameters)
{
	const TCHAR* Labels[] = {
		TEXT("Backward simulation time"), TEXT("Equal simulation time"),
		TEXT("Duplicate frame ID"), TEXT("Old frame ID"),
		TEXT("Ambiguous half-range frame ID"), TEXT("Old frame with different grid")
	};
	TArray<FSkySimDensityFrame> Frames;
	Frames.Add(MakeDensityFrame(12, 1.0, 102.0));
	Frames.Add(MakeDensityFrame(12, 2.0, 102.0));
	Frames.Add(MakeDensityFrame(11, 3.0, 102.0));
	Frames.Add(MakeDensityFrame(10, 3.0, 102.0));
	Frames.Add(MakeDensityFrame(0x8000000bu, 3.0, 102.0));
	Frames.Add(MakeDensityFrame(10, 3.0, 102.0, FIntVector(1, 2, 1)));
	FSkySimDensityFrameHistory History;
	History.Publish(MakeDensityFrame(10, 1.0, 100.0));
	History.Publish(MakeDensityFrame(11, 2.0, 101.0));
	const TArray<uint8> CurrentBytes = History.GetCurrent().Bytes;
	const TArray<uint8> PreviousBytes = History.GetPrevious().Bytes;
	for (int32 Index = 0; Index < Frames.Num(); ++Index)
	{
		TestFalse(FString::Printf(TEXT("%s: late completion is rejected"), Labels[Index]),
			History.Publish(MoveTemp(Frames[Index])));
		TestEqual(FString::Printf(TEXT("%s: latest ID is retained"), Labels[Index]),
			History.GetCurrent().FrameId, uint32(11));
		TestEqual(FString::Printf(TEXT("%s: latest solver time is retained"), Labels[Index]),
			History.GetCurrent().SimulationTime, 2.0);
		TestEqual(FString::Printf(TEXT("%s: latest receive time is retained"), Labels[Index]),
			History.GetCurrent().ReceivePlatformSeconds, 101.0);
		TestTrue(FString::Printf(TEXT("%s: latest bytes are retained"), Labels[Index]),
			History.GetCurrent().Bytes == CurrentBytes);
		TestEqual(FString::Printf(TEXT("%s: previous ID is retained"), Labels[Index]),
			History.GetPrevious().FrameId, uint32(10));
		TestTrue(FString::Printf(TEXT("%s: previous bytes are retained"), Labels[Index]),
			History.GetPrevious().Bytes == PreviousBytes);
		TestEqual(FString::Printf(TEXT("%s: revision does not change"), Labels[Index]), History.GetRevision(), uint64(2));
	}
	TestTrue(TEXT("A later ordered frame is still accepted"), History.Publish(MakeDensityFrame(12, 3.0, 103.0)));
	TestEqual(TEXT("New frame pairs with last accepted frame"), History.GetPrevious().FrameId, uint32(11));
	TestTrue(TEXT("Rejected frames did not replace pair data"), History.GetPrevious().Bytes == CurrentBytes);
	TestEqual(TEXT("Only accepted frames increment revision"), History.GetRevision(), uint64(3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimDensityFrameWrapTest,
	"SkySim.Cloud.DensityFrame.FrameIdWrap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityFrameWrapTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrameHistory History;
	TestTrue(TEXT("Publish maximum ID"), History.Publish(MakeDensityFrame(0xffffffffu, 1.0, 100.0)));
	TestTrue(TEXT("Publish wrapped ID"), History.Publish(MakeDensityFrame(0, 1.2, 100.2)));
	TestTrue(TEXT("Wrap retains continuity"), History.GetPrevious().IsValid());
	TestEqual(TEXT("Previous ID is the maximum uint32"), History.GetPrevious().FrameId, uint32(0xffffffffu));
	TestEqual(TEXT("Current ID is zero"), History.GetCurrent().FrameId, uint32(0));
	TestEqual(TEXT("Wrap advances revision"), History.GetRevision(), uint64(2));
	TestFalse(TEXT("Late pre-wrap frame is rejected"), History.Publish(MakeDensityFrame(0xffffffffu, 1.4, 100.4)));
	TestEqual(TEXT("Late pre-wrap frame cannot rewind current"), History.GetCurrent().FrameId, uint32(0));
	TestEqual(TEXT("Late pre-wrap frame preserves previous"), History.GetPrevious().FrameId, uint32(0xffffffffu));
	TestEqual(TEXT("Late pre-wrap frame preserves revision"), History.GetRevision(), uint64(2));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimDensityFrameBreakTest,
	"SkySim.Cloud.DensityFrame.BreakContinuity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityFrameBreakTest::RunTest(const FString& Parameters)
{
	FSkySimDensityFrameHistory History;
	History.Publish(MakeDensityFrame(10, 1.0, 100.0));
	History.Publish(MakeDensityFrame(11, 1.2, 100.2));
	const TArray<uint8> RetainedBytes = History.GetCurrent().Bytes;
	History.BreakContinuity();
	TestFalse(TEXT("Break clears previous"), History.GetPrevious().IsValid());
	TestTrue(TEXT("Break retains a valid current frame"), History.GetCurrent().IsValid());
	TestTrue(TEXT("Break retains current bytes"), History.GetCurrent().Bytes == RetainedBytes);
	TestEqual(TEXT("Break does not change revision"), History.GetRevision(), uint64(2));
	TestFalse(TEXT("Invalid publish after break is rejected"), History.Publish(FSkySimDensityFrame()));
	TestTrue(TEXT("Publish first frame from restarted sequence and solver clock"), History.Publish(MakeDensityFrame(0, 0.0, 100.4)));
	TestFalse(TEXT("Next session does not pair with retained old frame"), History.GetPrevious().IsValid());
	TestEqual(TEXT("Next session becomes current"), History.GetCurrent().FrameId, uint32(0));
	TestEqual(TEXT("Restarted solver time becomes current"), History.GetCurrent().SimulationTime, 0.0);
	TestFalse(TEXT("Break permits only one unchecked sequence"), History.Publish(MakeDensityFrame(0, 0.1, 100.5)));
	TestTrue(TEXT("Publish another new-session frame"), History.Publish(MakeDensityFrame(1, 0.2, 100.6)));
	TestTrue(TEXT("Continuity resumes within the new session"), History.GetPrevious().IsValid());
	TestEqual(TEXT("New-session previous ID"), History.GetPrevious().FrameId, uint32(0));
	TestEqual(TEXT("Only successful publications advance revision"), History.GetRevision(), uint64(4));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimDensityFrameValidationTest,
	"SkySim.Cloud.DensityFrame.Validation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimDensityFrameValidationTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("Default frame is invalid"), FSkySimDensityFrame().IsValid());
	FSkySimDensityFrame Frame = MakeDensityFrame(1, 1.0, 100.0);
	TestTrue(TEXT("Exact complete buffer is valid"), Frame.IsValid());
	Frame.Bytes.Add(0);
	TestFalse(TEXT("Odd byte count is invalid"), Frame.IsValid());
	Frame = MakeDensityFrame(1, 1.0, 100.0);
	Frame.GridSize = FIntVector(MAX_int32, MAX_int32, MAX_int32);
	TestFalse(TEXT("Dimensions whose voxel product overflows int64 are invalid"), Frame.IsValid());
	Frame.GridSize = FIntVector(MAX_int32, 1, MAX_int32);
	TestFalse(TEXT("Dimensions exceeding the buffer are invalid"), Frame.IsValid());
	Frame.GridSize = FIntVector(2, 0, 1);
	TestFalse(TEXT("Zero dimension is invalid"), Frame.IsValid());
	Frame.GridSize = FIntVector(2, -1, 1);
	TestFalse(TEXT("Negative dimension is invalid"), Frame.IsValid());
	Frame.GridSize = FIntVector(3, 1, 1);
	TestFalse(TEXT("Even but incomplete buffer is invalid"), Frame.IsValid());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
