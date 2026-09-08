#include "../SkySimCloudMotion.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include <limits>

namespace
{
FSkySimDensityFrame MakeMotionFrame(uint32 Id, double SimulationTime, double ReceiptTime)
{
	FSkySimDensityFrame Frame;
	Frame.FrameId = Id;
	Frame.SimulationTime = SimulationTime;
	Frame.ReceivePlatformSeconds = ReceiptTime;
	Frame.GridSize = FIntVector(1, 1, 1);
	Frame.ValueScale = 2.0f;
	Frame.ValueBias = 0.25f;
	Frame.Bytes.Add(static_cast<uint8>(Id));
	Frame.Bytes.Add(0);
	return Frame;
}

bool NearlyEqual(double A, double B)
{
	return FMath::Abs(A - B) <= 0.00001;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimCloudMotionEndpointsTest,
	"SkySim.Cloud.Motion.EndpointsAndBlend",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudMotionEndpointsTest::RunTest(const FString& Parameters)
{
	FSkySimCloudMotion Motion;
	TestFalse(TEXT("Empty stream has no sample"), Motion.Sample(10.0, 0.3, FVector::ZeroVector).IsValid());
	FSkySimDensityFrame First = MakeMotionFrame(1, 0.0, 10.0);
	TestTrue(TEXT("First frame accepted"), Motion.PushFrame(First, false));
	First.Bytes[0] = 99;
	TestTrue(TEXT("Second frame accepted"), Motion.PushFrame(MakeMotionFrame(2, 10.0, 11.0), true));
	const FSkySimCloudMotionSample Oldest = Motion.Sample(9.0, 0.0, FVector::ZeroVector);
	TestTrue(TEXT("Oldest endpoint is valid"), Oldest.IsValid());
	TestTrue(TEXT("Before history uses one endpoint"), Oldest.Previous == Oldest.Current);
	TestEqual(TEXT("Oldest endpoint ID"), Oldest.Current->FrameId, uint32(1));
	TestEqual(TEXT("Push owns a copy of source bytes"), Oldest.Current->Bytes[0], uint8(1));
	const FSkySimCloudMotionSample Half = Motion.Sample(10.5, 0.0, FVector::ZeroVector);
	TestEqual(TEXT("Blend starts at first frame"), Half.Previous->FrameId, uint32(1));
	TestEqual(TEXT("Blend ends at second frame"), Half.Current->FrameId, uint32(2));
	TestEqual(TEXT("Half blend"), Half.Alpha, 0.5f);
	TestEqual(TEXT("Simulation time follows the blend"), Half.SimulationTime, 5.0);
	TestEqual(TEXT("Current decoding scale survives"), Half.Current->ValueScale, 2.0f);
	TestEqual(TEXT("Previous decoding bias survives"), Half.Previous->ValueBias, 0.25f);
	const FSkySimCloudMotionSample Latest = Motion.Sample(12.0, 0.0, FVector::ZeroVector);
	TestTrue(TEXT("After history holds one endpoint"), Latest.Previous == Latest.Current);
	TestEqual(TEXT("Latest endpoint ID"), Latest.Current->FrameId, uint32(2));
	TestEqual(TEXT("No simulation extrapolation"), Latest.SimulationTime, 10.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimCloudMotionJitterTest,
	"SkySim.Cloud.Motion.BufferedJitterAndDelay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudMotionJitterTest::RunTest(const FString& Parameters)
{
	FSkySimCloudMotion Motion;
	Motion.PushFrame(MakeMotionFrame(1, 0.0, 100.0), false);
	Motion.PushFrame(MakeMotionFrame(2, 0.2, 100.22), true);
	Motion.PushFrame(MakeMotionFrame(3, 0.4, 100.45), true);
	const FSkySimCloudMotionSample BeforeArrival = Motion.Sample(100.50, 0.30, FVector::ZeroVector);
	const double BeforeTime = BeforeArrival.SimulationTime;
	const float BeforeAlpha = BeforeArrival.Alpha;
	TestTrue(TEXT("Delay uses the actual receipt interval"), NearlyEqual(BeforeTime, 0.2 * 0.20 / 0.22));
	Motion.PushFrame(MakeMotionFrame(4, 0.6, 100.67), true);
	const FSkySimCloudMotionSample AfterArrival = Motion.Sample(100.50, 0.30, FVector::ZeroVector);
	TestEqual(TEXT("A new arrival does not shift an already covered target"), AfterArrival.SimulationTime, BeforeTime);
	TestEqual(TEXT("A new arrival preserves covered blend weight"), AfterArrival.Alpha, BeforeAlpha);
	TestEqual(TEXT("Covered previous ID is unchanged"), AfterArrival.Previous->FrameId, uint32(1));
	TestEqual(TEXT("Covered current ID is unchanged"), AfterArrival.Current->FrameId, uint32(2));
	const double AdvancedTime = Motion.Sample(100.70, 0.30, FVector::ZeroVector).SimulationTime;
	TestTrue(TEXT("Playback advances inside the next jittered interval"), NearlyEqual(AdvancedTime, 0.2 + 0.2 * 0.18 / 0.23));
	TestEqual(TEXT("Increasing delay cannot rewind playback"),
		Motion.Sample(100.75, 0.60, FVector::ZeroVector).SimulationTime, AdvancedTime);
	TestEqual(TEXT("A backward platform sample cannot rewind playback"),
		Motion.Sample(100.68, 0.30, FVector::ZeroVector).SimulationTime, AdvancedTime);
	TestTrue(TEXT("Decreasing delay can advance playback"),
		Motion.Sample(100.75, 0.10, FVector::ZeroVector).SimulationTime > AdvancedTime);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimCloudMotionStallTest,
	"SkySim.Cloud.Motion.StallHoldsPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudMotionStallTest::RunTest(const FString& Parameters)
{
	FSkySimCloudMotion Motion;
	Motion.PushFrame(MakeMotionFrame(1, 0.0, 10.0), false);
	Motion.PushFrame(MakeMotionFrame(2, 10.0, 11.0), true);
	Motion.Sample(10.0, 0.0, FVector(2.0, 0.0, 0.0));
	const FSkySimCloudMotionSample ReachedEnd = Motion.Sample(11.0, 0.0, FVector(2.0, 0.0, 0.0));
	TestEqual(TEXT("Motion uses ten solver seconds, not one wall second"), ReachedEnd.AccumulatedDisplacementMeters.X, 20.0);
	const FSkySimCloudMotionSample Stalled = Motion.Sample(1000.0, 0.0, FVector(200.0, 0.0, 0.0));
	TestEqual(TEXT("Stalled solver time is held"), Stalled.SimulationTime, ReachedEnd.SimulationTime);
	TestEqual(TEXT("Wind changes during stall do not move phase"),
		Stalled.AccumulatedDisplacementMeters, ReachedEnd.AccumulatedDisplacementMeters);
	Motion.PushFrame(MakeMotionFrame(3, 10.2, 1001.0), true);
	const FSkySimCloudMotionSample Resumed = Motion.Sample(1001.0, 0.0, FVector(2.0, 0.0, 0.0));
	TestEqual(TEXT("Resumed delivery holds to the newly available solver time"), Resumed.SimulationTime, 10.2);
	TestTrue(TEXT("Resuming never integrates the stalled wall-clock duration"),
		NearlyEqual(Resumed.AccumulatedDisplacementMeters.X, 20.0 + 0.2 * 101.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimCloudMotionRestartTest,
	"SkySim.Cloud.Motion.RestartPreservesPhase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudMotionRestartTest::RunTest(const FString& Parameters)
{
	FSkySimCloudMotion Motion;
	const FVector Wind(2.0, 0.0, 0.0);
	Motion.PushFrame(MakeMotionFrame(1, 0.0, 10.0), false);
	Motion.PushFrame(MakeMotionFrame(2, 10.0, 11.0), true);
	Motion.Sample(10.0, 0.0, Wind);
	TestEqual(TEXT("Initial motion"), Motion.Sample(11.0, 0.0, Wind).AccumulatedDisplacementMeters.X, 20.0);
	Motion.BreakContinuity();
	const FSkySimCloudMotionSample Disconnected = Motion.Sample(12.0, 0.0, Wind);
	TestFalse(TEXT("Disconnect clears frame references"), Disconnected.IsValid());
	TestEqual(TEXT("Disconnect keeps accumulated phase"), Disconnected.AccumulatedDisplacementMeters.X, 20.0);
	Motion.PushFrame(MakeMotionFrame(1, 1000.0, 20.0), false);
	TestEqual(TEXT("New stream does not integrate its unrelated time origin"),
		Motion.Sample(20.0, 0.0, Wind).AccumulatedDisplacementMeters.X, 20.0);
	Motion.PushFrame(MakeMotionFrame(2, 1010.0, 21.0), true);
	TestEqual(TEXT("Motion resumes within the new solver stream"),
		Motion.Sample(21.0, 0.0, Wind).AccumulatedDisplacementMeters.X, 40.0);
	Motion.PushFrame(MakeMotionFrame(1, 0.0, 1.0), false);
	const FSkySimCloudMotionSample RewoundStream = Motion.Sample(1.0, 0.0, Wind);
	TestEqual(TEXT("Discontinuity resets playback target"), RewoundStream.SimulationTime, 0.0);
	TestEqual(TEXT("Backward stream origin also preserves phase"), RewoundStream.AccumulatedDisplacementMeters.X, 40.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimCloudMotionWindTest,
	"SkySim.Cloud.Motion.ChangingWind",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudMotionWindTest::RunTest(const FString& Parameters)
{
	FSkySimCloudMotion Motion;
	Motion.PushFrame(MakeMotionFrame(1, 0.0, 10.0), false);
	Motion.PushFrame(MakeMotionFrame(2, 4.0, 14.0), true);
	Motion.Sample(10.0, 0.0, FVector(2.0, 0.0, 0.0));
	TestEqual(TEXT("Constant wind integrates linearly"),
		Motion.Sample(11.0, 0.0, FVector(2.0, 0.0, 0.0)).AccumulatedDisplacementMeters.X, 2.0);
	TestEqual(TEXT("Reversal uses the mean of endpoint winds"),
		Motion.Sample(12.0, 0.0, FVector(-2.0, 0.0, 0.0)).AccumulatedDisplacementMeters.X, 2.0);
	TestEqual(TEXT("Reversed wind returns continuously"),
		Motion.Sample(13.0, 0.0, FVector(-2.0, 0.0, 0.0)).AccumulatedDisplacementMeters.X, 0.0);
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const FSkySimCloudMotionSample InvalidWind = Motion.Sample(14.0, 0.0, FVector(NaN, 0.0, 0.0));
	TestEqual(TEXT("Nonfinite wind retains the last valid wind"), InvalidWind.AccumulatedDisplacementMeters.X, -2.0);
	TestFalse(TEXT("Invalid wind cannot poison the phase"), InvalidWind.AccumulatedDisplacementMeters.ContainsNaN());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimCloudMotionSkippedFramesTest,
	"SkySim.Cloud.Motion.EqualReceiptsAndSkippedFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudMotionSkippedFramesTest::RunTest(const FString& Parameters)
{
	FSkySimCloudMotion Motion;
	Motion.PushFrame(MakeMotionFrame(0xfffffffeu, 1.0, 10.0), false);
	Motion.PushFrame(MakeMotionFrame(1, 2.0, 10.0), true);
	Motion.PushFrame(MakeMotionFrame(5, 4.0, 11.0), true);
	TestEqual(TEXT("Skipped and wrapped IDs retain all valid frames"), Motion.GetBufferedFrameCount(), 3);
	const FSkySimCloudMotionSample AtReceipt = Motion.Sample(10.0, 0.0, FVector::ZeroVector);
	TestTrue(TEXT("Equal receipt endpoint is finite"), FMath::IsFinite(AtReceipt.SimulationTime));
	TestEqual(TEXT("Exact receipt consistently selects the latest duplicate"), AtReceipt.Current->FrameId, uint32(1));
	TestEqual(TEXT("Exact duplicate receipt uses the latest solver time"), AtReceipt.SimulationTime, 2.0);
	const FSkySimCloudMotionSample Between = Motion.Sample(10.5, 0.0, FVector::ZeroVector);
	TestEqual(TEXT("Interpolation skips a zero-length receipt interval"), Between.Alpha, 0.5f);
	TestEqual(TEXT("Skipped frame IDs do not control blend weight"), Between.SimulationTime, 3.0);
	TestEqual(TEXT("Latest duplicate receipt supplies the lower bracket"), Between.Previous->FrameId, uint32(1));
	FSkySimCloudMotion UpperDuplicates;
	UpperDuplicates.PushFrame(MakeMotionFrame(1, 0.0, 10.0), false);
	UpperDuplicates.PushFrame(MakeMotionFrame(2, 1.0, 11.0), true);
	UpperDuplicates.PushFrame(MakeMotionFrame(3, 2.0, 11.0), true);
	const FSkySimCloudMotionSample BeforeDuplicate = UpperDuplicates.Sample(10.5, 0.0, FVector::ZeroVector);
	TestEqual(TEXT("Upper bracket also selects the latest duplicate"), BeforeDuplicate.Current->FrameId, uint32(3));
	TestEqual(TEXT("Approaching duplicate receipt converges to its selected endpoint"), BeforeDuplicate.SimulationTime, 1.0);
	TestEqual(TEXT("Exact upper receipt matches the same duplicate"),
		UpperDuplicates.Sample(11.0, 0.0, FVector::ZeroVector).SimulationTime, 2.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimCloudMotionInvalidInputsTest,
	"SkySim.Cloud.Motion.InvalidInputs",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudMotionInvalidInputsTest::RunTest(const FString& Parameters)
{
	FSkySimCloudMotion Motion;
	const double NaN = std::numeric_limits<double>::quiet_NaN();
	const double Infinity = std::numeric_limits<double>::infinity();
	TestFalse(TEXT("Incomplete frame is rejected"), Motion.PushFrame(FSkySimDensityFrame(), false));
	Motion.PushFrame(MakeMotionFrame(1, 0.0, 10.0), false);
	Motion.PushFrame(MakeMotionFrame(2, 10.0, 11.0), true);
	FSkySimDensityFrame Invalid = MakeMotionFrame(3, NaN, 12.0);
	TestFalse(TEXT("Nonfinite solver time is rejected"), Motion.PushFrame(Invalid, false));
	Invalid = MakeMotionFrame(3, 20.0, Infinity);
	TestFalse(TEXT("Nonfinite receipt time is rejected"), Motion.PushFrame(Invalid, false));
	TestEqual(TEXT("Invalid input cannot clear the existing stream"), Motion.GetBufferedFrameCount(), 2);
	const FSkySimCloudMotionSample InitialInvalidTime = Motion.Sample(NaN, 0.3, FVector(Infinity, 0.0, 0.0));
	TestEqual(TEXT("No timing baseline falls back to the oldest frame"), InitialInvalidTime.SimulationTime, 0.0);
	TestEqual(TEXT("Invalid first wind leaves phase at zero"), InitialInvalidTime.AccumulatedDisplacementMeters, FVector::ZeroVector);
	const double ValidTime = Motion.Sample(10.5, 0.0, FVector::ZeroVector).SimulationTime;
	TestEqual(TEXT("Nonfinite platform time holds playback"), Motion.Sample(Infinity, 0.3, FVector::ZeroVector).SimulationTime, ValidTime);
	TestEqual(TEXT("Nonfinite delay holds playback"), Motion.Sample(11.0, NaN, FVector::ZeroVector).SimulationTime, ValidTime);
	TestEqual(TEXT("Negative delay is clamped to zero"), Motion.Sample(11.0, -10.0, FVector::ZeroVector).SimulationTime, 10.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FSkySimCloudMotionCapacityTest,
	"SkySim.Cloud.Motion.BoundedHistory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimCloudMotionCapacityTest::RunTest(const FString& Parameters)
{
	FSkySimCloudMotion Motion;
	Motion.PushFrame(MakeMotionFrame(1, 0.0, 100.0), false);
	Motion.Sample(100.0, 0.0, FVector(10.0, 0.0, 0.0));
	for (int32 Index = 1; Index < 40; ++Index)
	{
		Motion.PushFrame(MakeMotionFrame(Index + 1, Index, 100.0 + Index), true);
		TestTrue(TEXT("History never exceeds its frame bound"), Motion.GetBufferedFrameCount() <= FSkySimCloudMotion::MaxBufferedFrames);
	}
	TestEqual(TEXT("History retains sixteen frames"), Motion.GetBufferedFrameCount(), FSkySimCloudMotion::MaxBufferedFrames);
	const FSkySimCloudMotionSample Clamped = Motion.Sample(100.0, 0.0, FVector(10.0, 0.0, 0.0));
	TestEqual(TEXT("Lost playback interval clamps to oldest retained frame"), Clamped.SimulationTime, 24.0);
	TestEqual(TEXT("Capacity overrun does not become a wind phase jump"), Clamped.AccumulatedDisplacementMeters, FVector::ZeroVector);
	const FSkySimCloudMotionSample Advanced = Motion.Sample(125.0, 0.0, FVector(10.0, 0.0, 0.0));
	TestEqual(TEXT("Integration resumes from the retained baseline"), Advanced.AccumulatedDisplacementMeters.X, 10.0);
	Motion.PushFrame(MakeMotionFrame(1, 1000.0, 1.0), false);
	TestEqual(TEXT("Discontinuity flushes the bounded history"), Motion.GetBufferedFrameCount(), 1);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
