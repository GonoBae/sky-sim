#include "SkySimCloudMotion.h"

namespace
{
bool IsFiniteWind(const FVector& Wind)
{
	return FMath::IsFinite(Wind.X) && FMath::IsFinite(Wind.Y) && FMath::IsFinite(Wind.Z);
}
}

bool FSkySimCloudMotion::PushFrame(const FSkySimDensityFrame& Frame, bool bContinuous)
{
	if (!Frame.IsValid())
	{
		return false;
	}
	// Copy before changing the buffer; the caller may pass a borrowed sample.
	FSkySimDensityFrame Incoming = Frame;
	if (!Frames.IsEmpty())
	{
		const FSkySimDensityFrame& Last = Frames.Last();
		const uint32 FrameDelta = Frame.FrameId - Last.FrameId;
		bContinuous = bContinuous && Frame.GridSize == Last.GridSize &&
			Frame.SimulationTime > Last.SimulationTime &&
			Frame.ReceivePlatformSeconds >= Last.ReceivePlatformSeconds &&
			FrameDelta != 0 && FrameDelta < 0x80000000u;
	}
	if (!bContinuous)
	{
		BreakContinuity();
	}
	if (Frames.Num() == MaxBufferedFrames)
	{
		// Losing the playback interval forces a forward clamp. Rebase the wind
		// integrator so that a buffer overrun does not become a large phase jump.
		if (bHasPlaybackTarget && LastPlaybackTarget < Frames[1].ReceivePlatformSeconds)
		{
			bHasMotionBaseline = false;
		}
		Frames.RemoveAt(0, 1, EAllowShrinking::No);
	}
	Frames.Add(MoveTemp(Incoming));
	return true;
}

void FSkySimCloudMotion::BreakContinuity()
{
	Frames.Reset();
	bHasPlaybackTarget = false;
	bHasMotionBaseline = false;
	LastValidWindMetersPerSecond = FVector::ZeroVector;
}

FSkySimCloudMotionSample FSkySimCloudMotion::Sample(
	double NowPlatformSeconds,
	double DelaySeconds,
	const FVector& MeanWindMetersPerSecond)
{
	FSkySimCloudMotionSample Result;
	Result.AccumulatedDisplacementMeters = AccumulatedDisplacementMeters;
	if (Frames.IsEmpty())
	{
		return Result;
	}

	double Target = bHasPlaybackTarget ? LastPlaybackTarget : Frames[0].ReceivePlatformSeconds;
	if (FMath::IsFinite(NowPlatformSeconds) && FMath::IsFinite(DelaySeconds))
	{
		const double RequestedTarget = NowPlatformSeconds - FMath::Max(0.0, DelaySeconds);
		if (FMath::IsFinite(RequestedTarget))
		{
			Target = bHasPlaybackTarget ? FMath::Max(RequestedTarget, LastPlaybackTarget) : RequestedTarget;
		}
	}
	LastPlaybackTarget = Target;
	bHasPlaybackTarget = true;

	// A same-receipt batch has no measurable internal interval. Use its last
	// frame at both sides of a bracket, including the exact receipt endpoint.
	int32 Lower = 0;
	while (Lower + 1 < Frames.Num() &&
		(Frames[Lower + 1].ReceivePlatformSeconds <= Target ||
		 Frames[Lower + 1].ReceivePlatformSeconds == Frames[Lower].ReceivePlatformSeconds))
	{
		++Lower;
	}
	Result.Previous = &Frames[Lower];
	Result.Current = &Frames[Lower];
	if (Lower + 1 < Frames.Num() && Target > Frames[Lower].ReceivePlatformSeconds)
	{
		int32 Upper = Lower + 1;
		while (Upper + 1 < Frames.Num() &&
			Frames[Upper + 1].ReceivePlatformSeconds == Frames[Upper].ReceivePlatformSeconds)
		{
			++Upper;
		}
		Result.Current = &Frames[Upper];
		const double Interval = Result.Current->ReceivePlatformSeconds - Result.Previous->ReceivePlatformSeconds;
		const double Fraction = (Target - Result.Previous->ReceivePlatformSeconds) / Interval;
		Result.Alpha = FMath::IsFinite(Fraction)
			? static_cast<float>(FMath::Clamp(Fraction, 0.0, 1.0)) : 0.0f;
	}

	const double Alpha = Result.Alpha;
	Result.SimulationTime = Result.Previous->SimulationTime * (1.0 - Alpha) +
		Result.Current->SimulationTime * Alpha;
	if (!FMath::IsFinite(Result.SimulationTime))
	{
		Result.Previous = Result.Current;
		Result.Alpha = 0.0f;
		Result.SimulationTime = Result.Current->SimulationTime;
	}

	const FVector Wind = IsFiniteWind(MeanWindMetersPerSecond)
		? MeanWindMetersPerSecond : LastValidWindMetersPerSecond;
	if (bHasMotionBaseline)
	{
		const double SimulationDelta = Result.SimulationTime - LastSampleSimulationTime;
		if (FMath::IsFinite(SimulationDelta) && SimulationDelta > 0.0)
		{
			const FVector MeanWind = LastValidWindMetersPerSecond * 0.5 + Wind * 0.5;
			const FVector NextDisplacement = AccumulatedDisplacementMeters + MeanWind * SimulationDelta;
			if (IsFiniteWind(NextDisplacement))
			{
				AccumulatedDisplacementMeters = NextDisplacement;
			}
		}
	}
	LastSampleSimulationTime = Result.SimulationTime;
	LastValidWindMetersPerSecond = Wind;
	bHasMotionBaseline = true;
	Result.AccumulatedDisplacementMeters = AccumulatedDisplacementMeters;
	return Result;
}
