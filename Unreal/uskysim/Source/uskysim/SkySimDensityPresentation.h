#pragma once

#include "CoreMinimal.h"
#include "SkySimDensityFrame.h"

struct FSkySimDensityPresentationSettings
{
	int32 SpreadIterations = 0;
	float SpreadStrength = 0.2f;
	float ShapePower = 0.92f;
	float Gain = 1.2f;
	bool bNormalizePhysicalDensity = false;
	float DensityReferenceScale = 8.0f;
};

// CPU-only conversion from a complete CLD2 frame to the displayed G16 density.
// Callers must advance FrameRevision whenever the source frame changes.
class FSkySimDensityPresentation
{
public:
	// Returns true only when new output was prepared. Invalid input clears output.
	bool Prepare(const FSkySimDensityFrame& Frame, uint64 FrameRevision,
		const FSkySimDensityPresentationSettings& Settings);
	const TArray<uint8>& GetBytes() const { return Bytes; }
	FIntVector GetGridSize() const { return GridSize; }
	uint64 GetRevision() const { return Revision; }
	void Reset();

private:
	TArray<uint8> Bytes;
	TArray<float> Working;
	TArray<float> ScratchA;
	TArray<float> ScratchB;
	FIntVector GridSize = FIntVector::ZeroValue;
	FSkySimDensityPresentationSettings CachedSettings;
	float CachedValueScale = 1.0f;
	float CachedValueBias = 0.0f;
	uint64 CachedFrameRevision = 0;
	uint64 Revision = 0;
	bool bHasCachedPreparation = false;
};
