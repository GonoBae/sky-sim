#include "SkySimDensityPresentation.h"

namespace
{
	FSkySimDensityPresentationSettings NormalizeSettings(const FSkySimDensityPresentationSettings& Settings)
	{
		FSkySimDensityPresentationSettings Result;
		Result.SpreadIterations = FMath::Clamp(Settings.SpreadIterations, 0, 3);
		Result.SpreadStrength = FMath::Clamp(
			FMath::IsFinite(Settings.SpreadStrength) ? Settings.SpreadStrength : Result.SpreadStrength, 0.0f, 1.0f);
		Result.ShapePower = FMath::Clamp(
			FMath::IsFinite(Settings.ShapePower) ? Settings.ShapePower : Result.ShapePower, 0.1f, 2.0f);
		Result.Gain = FMath::Clamp(FMath::IsFinite(Settings.Gain) ? Settings.Gain : Result.Gain, 0.0f, 4.0f);
		Result.bNormalizePhysicalDensity = Settings.bNormalizePhysicalDensity;
		Result.DensityReferenceScale = FMath::Clamp(
			FMath::IsFinite(Settings.DensityReferenceScale) ? Settings.DensityReferenceScale : Result.DensityReferenceScale,
			0.001f, 100000.0f);
		return Result;
	}

	bool SettingsEqual(const FSkySimDensityPresentationSettings& A, const FSkySimDensityPresentationSettings& B)
	{
		return A.SpreadIterations == B.SpreadIterations && A.SpreadStrength == B.SpreadStrength &&
			A.ShapePower == B.ShapePower && A.Gain == B.Gain &&
			A.bNormalizePhysicalDensity == B.bNormalizePhysicalDensity &&
			A.DensityReferenceScale == B.DensityReferenceScale;
	}
}

bool FSkySimDensityPresentation::Prepare(const FSkySimDensityFrame& Frame, uint64 FrameRevision,
	const FSkySimDensityPresentationSettings& Settings)
{
	if (!Frame.IsValid())
	{
		Bytes.Reset();
		GridSize = FIntVector::ZeroValue;
		bHasCachedPreparation = false;
		return false;
	}

	const FSkySimDensityPresentationSettings NormalizedSettings = NormalizeSettings(Settings);
	if (bHasCachedPreparation && CachedFrameRevision == FrameRevision && GridSize == Frame.GridSize &&
		SettingsEqual(CachedSettings, NormalizedSettings) &&
		(!NormalizedSettings.bNormalizePhysicalDensity ||
			(CachedValueScale == Frame.ValueScale && CachedValueBias == Frame.ValueBias)))
	{
		return false;
	}

	const int32 SizeX = Frame.GridSize.X;
	const int32 SizeY = Frame.GridSize.Y;
	const int32 SizeZ = Frame.GridSize.Z;
	const int32 VoxelCount = Frame.Bytes.Num() / 2;
	Working.SetNumUninitialized(VoxelCount);
	Bytes.SetNumUninitialized(Frame.Bytes.Num());

	constexpr float InverseUInt16Maximum = 1.0f / 65535.0f;
	for (int32 Index = 0; Index < VoxelCount; ++Index)
	{
		const uint16 Value = static_cast<uint16>(Frame.Bytes[Index * 2]) |
			(static_cast<uint16>(Frame.Bytes[Index * 2 + 1]) << 8);
		if (NormalizedSettings.bNormalizePhysicalDensity)
		{
			// CLD2 can choose a different encoding scale for every frame. Decode
			// physical density before using a fixed display reference so unchanged
			// cloud mass does not brighten/darken with the frame's peak density.
			// Double arithmetic also avoids overflow for finite scale/bias inputs.
			const double PhysicalDensity = (static_cast<double>(Value) / 65535.0) * Frame.ValueScale + Frame.ValueBias;
			Working[Index] = static_cast<float>(FMath::Clamp(
				PhysicalDensity / NormalizedSettings.DensityReferenceScale, 0.0, 1.0));
		}
		else
		{
			Working[Index] = Value * InverseUInt16Maximum;
		}
	}

	const auto GetIndex = [SizeX, SizeY](int32 X, int32 Y, int32 Z)
	{
		return (Z * SizeY + Y) * SizeX + X;
	};
	const auto MaxFilterAxis = [SizeX, SizeY, SizeZ, &GetIndex](
		const TArray<float>& Source, TArray<float>& Destination, int32 Axis)
	{
		for (int32 Z = 0; Z < SizeZ; ++Z)
		{
			for (int32 Y = 0; Y < SizeY; ++Y)
			{
				for (int32 X = 0; X < SizeX; ++X)
				{
					float Maximum = Source[GetIndex(X, Y, Z)];
					if (Axis == 0)
					{
						if (X > 0) Maximum = FMath::Max(Maximum, Source[GetIndex(X - 1, Y, Z)]);
						if (X + 1 < SizeX) Maximum = FMath::Max(Maximum, Source[GetIndex(X + 1, Y, Z)]);
					}
					else if (Axis == 1)
					{
						if (Y > 0) Maximum = FMath::Max(Maximum, Source[GetIndex(X, Y - 1, Z)]);
						if (Y + 1 < SizeY) Maximum = FMath::Max(Maximum, Source[GetIndex(X, Y + 1, Z)]);
					}
					else
					{
						if (Z > 0) Maximum = FMath::Max(Maximum, Source[GetIndex(X, Y, Z - 1)]);
						if (Z + 1 < SizeZ) Maximum = FMath::Max(Maximum, Source[GetIndex(X, Y, Z + 1)]);
					}
					Destination[GetIndex(X, Y, Z)] = Maximum;
				}
			}
		}
	};

	if (NormalizedSettings.SpreadIterations > 0 && NormalizedSettings.SpreadStrength > 0.0f)
	{
		ScratchA.SetNumUninitialized(VoxelCount);
		ScratchB.SetNumUninitialized(VoxelCount);
		for (int32 Iteration = 0; Iteration < NormalizedSettings.SpreadIterations; ++Iteration)
		{
			// Preserve the existing separable 3x3x3 max filter and its edge handling.
			MaxFilterAxis(Working, ScratchA, 0);
			MaxFilterAxis(ScratchA, ScratchB, 1);
			MaxFilterAxis(ScratchB, ScratchA, 2);
			for (int32 Index = 0; Index < VoxelCount; ++Index)
			{
				Working[Index] = FMath::Max(Working[Index], ScratchA[Index] * NormalizedSettings.SpreadStrength);
			}
		}
	}

	for (int32 Index = 0; Index < VoxelCount; ++Index)
	{
		const float ShapedDensity = FMath::Clamp(
			FMath::Pow(FMath::Max(Working[Index], 0.0f), NormalizedSettings.ShapePower) * NormalizedSettings.Gain,
			0.0f,
			1.0f);
		const uint16 Value = static_cast<uint16>(FMath::RoundToInt(ShapedDensity * 65535.0f));
		Bytes[Index * 2] = static_cast<uint8>(Value);
		Bytes[Index * 2 + 1] = static_cast<uint8>(Value >> 8);
	}

	GridSize = Frame.GridSize;
	CachedFrameRevision = FrameRevision;
	CachedSettings = NormalizedSettings;
	CachedValueScale = Frame.ValueScale;
	CachedValueBias = Frame.ValueBias;
	bHasCachedPreparation = true;
	++Revision;
	return true;
}

void FSkySimDensityPresentation::Reset()
{
	Bytes.Reset();
	Working.Reset();
	ScratchA.Reset();
	ScratchB.Reset();
	GridSize = FIntVector::ZeroValue;
	CachedSettings = FSkySimDensityPresentationSettings();
	CachedValueScale = 1.0f;
	CachedValueBias = 0.0f;
	CachedFrameRevision = 0;
	Revision = 0;
	bHasCachedPreparation = false;
}
