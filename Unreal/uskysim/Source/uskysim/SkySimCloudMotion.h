#pragma once

#include "SkySimDensityFrame.h"

struct FSkySimCloudMotionSample
{
	// Borrowed until the next PushFrame or BreakContinuity call.
	const FSkySimDensityFrame* Previous = nullptr;
	const FSkySimDensityFrame* Current = nullptr;
	float Alpha = 0.0f;
	double SimulationTime = 0.0;
	FVector AccumulatedDisplacementMeters = FVector::ZeroVector;

	bool IsValid() const { return Previous != nullptr && Current != nullptr; }
};

// Delayed receive-time playback of the CLD2 solver clock. Wind phase advances
// with the displayed simulation time and therefore holds when frame delivery stops.
class FSkySimCloudMotion
{
public:
	static constexpr int32 MaxBufferedFrames = 16;

	bool PushFrame(const FSkySimDensityFrame& Frame, bool bContinuous);
	void BreakContinuity();
	FSkySimCloudMotionSample Sample(double NowPlatformSeconds, double DelaySeconds,
		const FVector& MeanWindMetersPerSecond);
	int32 GetBufferedFrameCount() const { return Frames.Num(); }

private:
	TArray<FSkySimDensityFrame> Frames;
	double LastPlaybackTarget = 0.0;
	double LastSampleSimulationTime = 0.0;
	FVector LastValidWindMetersPerSecond = FVector::ZeroVector;
	FVector AccumulatedDisplacementMeters = FVector::ZeroVector;
	bool bHasPlaybackTarget = false;
	bool bHasMotionBaseline = false;
};
