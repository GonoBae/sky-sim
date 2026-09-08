#pragma once

#include "CoreMinimal.h"
#include "SkySimDensityFrame.h"

// Reassembles CLD2 datagrams without depending on an actor, socket, or renderer.
class FSkySimVolumeReceiver
{
public:
	enum class EPacketResult : uint8
	{
		Rejected,
		Accepted,
		FrameComplete
	};

	// OutFrame changes only when a complete, validated density frame is returned.
	EPacketResult Consume(const uint8* Data, int32 NumBytes, double ReceivePlatformSeconds, FSkySimDensityFrame& OutFrame);
	void Reset();
	void Prune(double Now);

private:
	struct FFieldAssembly
	{
		FIntVector GridSize = FIntVector::ZeroValue;
		uint32 FieldCrc32 = 0;
		uint8 VoxelFormat = 0;
		uint8 ChannelCount = 0;
		uint8 Compression = 0;
		uint16 ChunkCount = 0;
		uint16 Flags = 0;
		uint32 EncodedBytes = 0;
		uint32 DecodedBytes = 0;
		float ValueScale = 0.0f;
		float ValueBias = 0.0f;
		TArray<uint8> Encoded;
		TArray<uint8> Decoded;
		TBitArray<> ReceivedChunks;
		int32 ReceivedChunkCount = 0;

		bool IsComplete() const { return Decoded.Num() == static_cast<int32>(DecodedBytes); }
	};

	struct FFrameAssembly
	{
		double SimulationTime = 0.0;
		uint16 FieldMask = 0;
		double LastReceivedPlatformSeconds = 0.0;
		TMap<uint8, FFieldAssembly> Fields;
	};

	static bool DecodeCompletedField(FFieldAssembly& Field);
	TMap<uint32, FFrameAssembly> Frames;
};
