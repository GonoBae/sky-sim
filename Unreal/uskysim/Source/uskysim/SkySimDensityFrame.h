#pragma once

#include "CoreMinimal.h"

// A complete, decoded density snapshot. SimulationTime is the CLD2 solver clock,
// not the SKS1 UTC clock. Keep decoding metadata beside the bytes at every stage.
struct FSkySimDensityFrame
{
	uint32 FrameId = 0;
	double SimulationTime = 0.0;
	double ReceivePlatformSeconds = 0.0;
	FIntVector GridSize = FIntVector::ZeroValue;
	float ValueScale = 1.0f;
	float ValueBias = 0.0f;
	TArray<uint8> Bytes;

	bool IsValid() const
	{
		if (GridSize.X <= 0 || GridSize.Y <= 0 || GridSize.Z <= 0 ||
			!FMath::IsFinite(SimulationTime) || !FMath::IsFinite(ReceivePlatformSeconds) ||
			!FMath::IsFinite(ValueScale) || !FMath::IsFinite(ValueBias))
		{
			return false;
		}
		// Bound each multiplication by the actual buffer, including malformed
		// dimensions that could otherwise overflow even a 64-bit voxel product.
		const int64 VoxelCount = Bytes.Num() / 2;
		const int64 PlaneSize = static_cast<int64>(GridSize.X) * GridSize.Y;
		return Bytes.Num() > 0 && Bytes.Num() % 2 == 0 && PlaneSize <= VoxelCount &&
			PlaneSize * GridSize.Z == VoxelCount;
	}
};

// Retains the two most recently published compatible frames. Late or duplicate
// completions never replace the current pair; a new session must break continuity.
class FSkySimDensityFrameHistory
{
public:
	bool Publish(FSkySimDensityFrame&& Frame)
	{
		if (!Frame.IsValid())
		{
			return false;
		}
		const uint32 FrameDelta = Frame.FrameId - Current.FrameId;
		const bool bHasCurrent = Current.IsValid();
		if (bHasCurrent && !bBreakBeforeNextFrame &&
			(FrameDelta == 0 || FrameDelta >= 0x80000000u || Frame.SimulationTime <= Current.SimulationTime))
		{
			return false;
		}
		const bool bContinuous = bHasCurrent && !bBreakBeforeNextFrame &&
			Frame.GridSize == Current.GridSize && Frame.ReceivePlatformSeconds >= Current.ReceivePlatformSeconds;
		Previous = bContinuous ? MoveTemp(Current) : FSkySimDensityFrame();
		Current = MoveTemp(Frame);
		bBreakBeforeNextFrame = false;
		++Revision;
		return true;
	}

	void BreakContinuity()
	{
		// Keep the displayed frame while permitting the next valid frame to start
		// a new sequence and solver clock, including a server restart at ID zero.
		Previous = FSkySimDensityFrame();
		bBreakBeforeNextFrame = true;
	}

	const FSkySimDensityFrame& GetCurrent() const { return Current; }
	const FSkySimDensityFrame& GetPrevious() const { return Previous; }
	uint64 GetRevision() const { return Revision; }

private:
	FSkySimDensityFrame Current;
	FSkySimDensityFrame Previous;
	uint64 Revision = 0;
	bool bBreakBeforeNextFrame = false;
};
