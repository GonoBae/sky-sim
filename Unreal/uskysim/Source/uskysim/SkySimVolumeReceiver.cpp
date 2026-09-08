#include "SkySimVolumeReceiver.h"

#include "Misc/Crc.h"

namespace SkySimVolumeReceiverPrivate
{
	constexpr uint32 MaxPayloadBytes = 1200;
	constexpr uint32 MaxFieldBytes = 64 * 1024 * 1024;
	constexpr int32 MaxIncompleteFrames = 8;
	constexpr double StaleFrameSeconds = 1.0;

	uint16 VolumeReadU16(const uint8* Data, int32 Offset)
	{
		return static_cast<uint16>(Data[Offset]) |
			(static_cast<uint16>(Data[Offset + 1]) << 8);
	}

	uint32 VolumeReadU32(const uint8* Data, int32 Offset)
	{
		return static_cast<uint32>(Data[Offset]) |
			(static_cast<uint32>(Data[Offset + 1]) << 8) |
			(static_cast<uint32>(Data[Offset + 2]) << 16) |
			(static_cast<uint32>(Data[Offset + 3]) << 24);
	}

	float VolumeReadF32(const uint8* Data, int32 Offset)
	{
		const uint32 Bits = VolumeReadU32(Data, Offset);
		float Value = 0.0f;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}

	double VolumeReadF64(const uint8* Data, int32 Offset)
	{
		uint64 Bits = 0;
		for (int32 ByteIndex = 0; ByteIndex < 8; ++ByteIndex)
		{
			Bits |= static_cast<uint64>(Data[Offset + ByteIndex]) << (ByteIndex * 8);
		}
		double Value = 0.0;
		FMemory::Memcpy(&Value, &Bits, sizeof(Value));
		return Value;
	}
}

FSkySimVolumeReceiver::EPacketResult FSkySimVolumeReceiver::Consume(
	const uint8* Data, int32 NumBytes, double ReceivePlatformSeconds, FSkySimDensityFrame& OutFrame)
{
	using namespace SkySimVolumeReceiverPrivate;
	if (Data == nullptr || NumBytes < 64 || FMemory::Memcmp(Data, "CLD2", 4) != 0 ||
		VolumeReadU16(Data, 4) != 2 || VolumeReadU16(Data, 6) != 64 || !FMath::IsFinite(ReceivePlatformSeconds))
	{
		return EPacketResult::Rejected;
	}

	const uint32 FrameId = VolumeReadU32(Data, 8);
	const uint32 FieldCrc32 = VolumeReadU32(Data, 12);
	const double SimulationTime = VolumeReadF64(Data, 16);
	const uint16 GridX = VolumeReadU16(Data, 24);
	const uint16 GridY = VolumeReadU16(Data, 26);
	const uint16 GridZ = VolumeReadU16(Data, 28);
	const uint8 VoxelFormat = Data[30];
	const uint8 FieldId = Data[31];
	const uint8 ChannelCount = Data[32];
	const uint8 Compression = Data[33];
	const uint16 ChunkIndex = VolumeReadU16(Data, 34);
	const uint16 ChunkCount = VolumeReadU16(Data, 36);
	const uint16 PayloadBytes = VolumeReadU16(Data, 38);
	const uint16 FieldMask = VolumeReadU16(Data, 40);
	const uint16 Flags = VolumeReadU16(Data, 42);
	const uint32 PayloadOffset = VolumeReadU32(Data, 44);
	const uint32 EncodedBytes = VolumeReadU32(Data, 48);
	const uint32 DecodedBytes = VolumeReadU32(Data, 52);
	const float ValueScale = VolumeReadF32(Data, 56);
	const float ValueBias = VolumeReadF32(Data, 60);

	constexpr uint16 SupportedFieldMask = 0x001f;
	constexpr uint16 MacroFieldMask = 0x000f;
	const uint16 FieldBit = FieldId >= 1 && FieldId <= 5 ? static_cast<uint16>(1u << (FieldId - 1u)) : 0;
	const uint32 BytesPerChannel = (VoxelFormat == 1 || VoxelFormat == 3) ? 1u :
		(VoxelFormat == 2 || VoxelFormat == 4) ? 2u : 0u;
	const uint8 ExpectedFormat[5] = {2, 4, 1, 1, 1};
	const uint8 ExpectedChannels[5] = {1, 3, 1, 1, 1};
	const uint64 ExpectedDecodedBytes = static_cast<uint64>(GridX) * GridY * GridZ * ChannelCount * BytesPerChannel;
	const uint32 ExpectedChunkCount = EncodedBytes == 0 ? 0 : (EncodedBytes + MaxPayloadBytes - 1u) / MaxPayloadBytes;
	const uint32 ExpectedPayloadBytes = PayloadOffset < EncodedBytes ? FMath::Min(MaxPayloadBytes, EncodedBytes - PayloadOffset) : 0;
	const uint8 OccupancyBrick = static_cast<uint8>(Flags >> 8);
	const bool bFlagsValid = (Flags & 0x00feu) == 0u &&
		((FieldId == 5 && (OccupancyBrick == 2 || OccupancyBrick == 4 || OccupancyBrick == 8)) ||
		 (FieldId != 5 && OccupancyBrick == 0));

	if (FieldBit == 0 || VoxelFormat != ExpectedFormat[FieldId - 1] || ChannelCount != ExpectedChannels[FieldId - 1] ||
		Compression > 1 || !FMath::IsFinite(SimulationTime) || !FMath::IsFinite(ValueScale) || !FMath::IsFinite(ValueBias) ||
		GridX == 0 || GridY == 0 || GridZ == 0 || BytesPerChannel == 0 || ExpectedDecodedBytes != DecodedBytes ||
		EncodedBytes == 0 || EncodedBytes > MaxFieldBytes || DecodedBytes > MaxFieldBytes ||
		FieldMask == 0 || (FieldMask & ~SupportedFieldMask) != 0 || (FieldMask & FieldBit) == 0 ||
		(FieldId == 5 && (FieldMask & MacroFieldMask) == 0) || !bFlagsValid ||
		PayloadBytes > MaxPayloadBytes || NumBytes != 64 + PayloadBytes || ChunkCount != ExpectedChunkCount ||
		ChunkIndex >= ChunkCount || PayloadOffset != static_cast<uint32>(ChunkIndex) * MaxPayloadBytes ||
		PayloadBytes != ExpectedPayloadBytes || PayloadOffset + PayloadBytes > EncodedBytes)
	{
		return EPacketResult::Rejected;
	}

	FFrameAssembly* Frame = Frames.Find(FrameId);
	if (Frame == nullptr)
	{
		if (Frames.Num() >= MaxIncompleteFrames)
		{
			Prune(ReceivePlatformSeconds);
			if (Frames.Num() >= MaxIncompleteFrames)
			{
				double OldestTime = TNumericLimits<double>::Max();
				uint32 OldestId = 0;
				for (const TPair<uint32, FFrameAssembly>& Pair : Frames)
				{
					if (Pair.Value.LastReceivedPlatformSeconds < OldestTime)
					{
						OldestTime = Pair.Value.LastReceivedPlatformSeconds;
						OldestId = Pair.Key;
					}
				}
				Frames.Remove(OldestId);
			}
		}

		FFrameAssembly NewFrame;
		NewFrame.SimulationTime = SimulationTime;
		NewFrame.FieldMask = FieldMask;
		NewFrame.LastReceivedPlatformSeconds = ReceivePlatformSeconds;
		Frame = &Frames.Add(FrameId, MoveTemp(NewFrame));
	}
	else if (Frame->FieldMask != FieldMask || Frame->SimulationTime != SimulationTime)
	{
		Frames.Remove(FrameId);
		return EPacketResult::Rejected;
	}

	for (const TPair<uint8, FFieldAssembly>& Pair : Frame->Fields)
	{
		const FFieldAssembly& Other = Pair.Value;
		if (FieldId <= 4 && Pair.Key <= 4 && Other.GridSize != FIntVector(GridX, GridY, GridZ))
		{
			Frames.Remove(FrameId);
			return EPacketResult::Rejected;
		}
		if (FieldId == 5 && Pair.Key <= 4)
		{
			const FIntVector ExpectedSize(
				(Other.GridSize.X + OccupancyBrick - 1) / OccupancyBrick,
				(Other.GridSize.Y + OccupancyBrick - 1) / OccupancyBrick,
				(Other.GridSize.Z + OccupancyBrick - 1) / OccupancyBrick);
			if (ExpectedSize != FIntVector(GridX, GridY, GridZ))
			{
				Frames.Remove(FrameId);
				return EPacketResult::Rejected;
			}
		}
		if (FieldId <= 4 && Pair.Key == 5)
		{
			const uint8 OtherBrick = static_cast<uint8>(Other.Flags >> 8);
			const FIntVector ExpectedSize(
				(GridX + OtherBrick - 1) / OtherBrick,
				(GridY + OtherBrick - 1) / OtherBrick,
				(GridZ + OtherBrick - 1) / OtherBrick);
			if (ExpectedSize != Other.GridSize)
			{
				Frames.Remove(FrameId);
				return EPacketResult::Rejected;
			}
		}
	}

	FFieldAssembly* Field = Frame->Fields.Find(FieldId);
	if (Field == nullptr)
	{
		FFieldAssembly NewField;
		NewField.GridSize = FIntVector(GridX, GridY, GridZ);
		NewField.FieldCrc32 = FieldCrc32;
		NewField.VoxelFormat = VoxelFormat;
		NewField.ChannelCount = ChannelCount;
		NewField.Compression = Compression;
		NewField.ChunkCount = ChunkCount;
		NewField.Flags = Flags;
		NewField.EncodedBytes = EncodedBytes;
		NewField.DecodedBytes = DecodedBytes;
		NewField.ValueScale = ValueScale;
		NewField.ValueBias = ValueBias;
		NewField.Encoded.SetNumZeroed(static_cast<int32>(EncodedBytes));
		NewField.ReceivedChunks.Init(false, ChunkCount);
		Field = &Frame->Fields.Add(FieldId, MoveTemp(NewField));
	}
	else if (Field->GridSize != FIntVector(GridX, GridY, GridZ) || Field->FieldCrc32 != FieldCrc32 ||
		Field->VoxelFormat != VoxelFormat || Field->ChannelCount != ChannelCount || Field->Compression != Compression ||
		Field->ChunkCount != ChunkCount || Field->Flags != Flags || Field->EncodedBytes != EncodedBytes ||
		Field->DecodedBytes != DecodedBytes || Field->ValueScale != ValueScale || Field->ValueBias != ValueBias)
	{
		Frames.Remove(FrameId);
		return EPacketResult::Rejected;
	}

	const uint8* Payload = Data + 64;
	if (Field->ReceivedChunks[ChunkIndex])
	{
		// Raw fields move their payload into Decoded. Compressed fields retain the
		// encoded payload until the whole frame completes so duplicates remain verifiable.
		const TArray<uint8>& ReceivedBytes = Field->IsComplete() && Field->Compression == 0 ? Field->Decoded : Field->Encoded;
		if (FMemory::Memcmp(ReceivedBytes.GetData() + PayloadOffset, Payload, PayloadBytes) != 0)
		{
			Frames.Remove(FrameId);
			return EPacketResult::Rejected;
		}
	}
	else
	{
		FMemory::Memcpy(Field->Encoded.GetData() + PayloadOffset, Payload, PayloadBytes);
		Field->ReceivedChunks[ChunkIndex] = true;
		++Field->ReceivedChunkCount;
	}

	if (Field->ReceivedChunkCount == Field->ChunkCount && !Field->IsComplete() && !DecodeCompletedField(*Field))
	{
		Frames.Remove(FrameId);
		return EPacketResult::Rejected;
	}

	Frame->LastReceivedPlatformSeconds = ReceivePlatformSeconds;
	uint16 CompleteMask = 0;
	for (const TPair<uint8, FFieldAssembly>& Pair : Frame->Fields)
	{
		if (Pair.Value.IsComplete())
		{
			CompleteMask |= static_cast<uint16>(1u << (Pair.Key - 1u));
		}
	}
	if (CompleteMask != Frame->FieldMask)
	{
		return EPacketResult::Accepted;
	}

	FFieldAssembly* Density = Frame->Fields.Find(1);
	if (Density == nullptr)
	{
		Frames.Remove(FrameId);
		return EPacketResult::Accepted;
	}

	FSkySimDensityFrame CompletedFrame;
	CompletedFrame.FrameId = FrameId;
	CompletedFrame.SimulationTime = Frame->SimulationTime;
	CompletedFrame.ReceivePlatformSeconds = ReceivePlatformSeconds;
	CompletedFrame.GridSize = Density->GridSize;
	CompletedFrame.ValueScale = Density->ValueScale;
	CompletedFrame.ValueBias = Density->ValueBias;
	CompletedFrame.Bytes = MoveTemp(Density->Decoded);
	Frames.Remove(FrameId);
	OutFrame = MoveTemp(CompletedFrame);
	return EPacketResult::FrameComplete;
}

bool FSkySimVolumeReceiver::DecodeCompletedField(FFieldAssembly& Field)
{
	if (Field.Compression == 0)
	{
		if (Field.EncodedBytes != Field.DecodedBytes)
		{
			return false;
		}
		Field.Decoded = MoveTemp(Field.Encoded);
	}
	else
	{
		Field.Decoded.Reset(static_cast<int32>(Field.DecodedBytes));
		int32 InputPosition = 0;
		while (InputPosition < Field.Encoded.Num())
		{
			const uint8 Control = Field.Encoded[InputPosition++];
			if ((Control & 0x80u) != 0)
			{
				if (InputPosition >= Field.Encoded.Num())
				{
					return false;
				}
				const int32 RunLength = (Control & 0x7fu) + 3;
				if (Field.Decoded.Num() > static_cast<int32>(Field.DecodedBytes) - RunLength)
				{
					return false;
				}
				Field.Decoded.AddUninitialized(RunLength);
				FMemory::Memset(Field.Decoded.GetData() + Field.Decoded.Num() - RunLength, Field.Encoded[InputPosition++], RunLength);
			}
			else
			{
				const int32 LiteralLength = Control + 1;
				if (InputPosition > Field.Encoded.Num() - LiteralLength || Field.Decoded.Num() > static_cast<int32>(Field.DecodedBytes) - LiteralLength)
				{
					return false;
				}
				Field.Decoded.Append(Field.Encoded.GetData() + InputPosition, LiteralLength);
				InputPosition += LiteralLength;
			}
		}
		if (Field.Decoded.Num() != static_cast<int32>(Field.DecodedBytes))
		{
			return false;
		}
	}

	return FCrc::MemCrc32(Field.Decoded.GetData(), Field.Decoded.Num()) == Field.FieldCrc32;
}

void FSkySimVolumeReceiver::Reset()
{
	Frames.Reset();
}

void FSkySimVolumeReceiver::Prune(double Now)
{
	for (auto Iterator = Frames.CreateIterator(); Iterator; ++Iterator)
	{
		if (Now - Iterator.Value().LastReceivedPlatformSeconds > SkySimVolumeReceiverPrivate::StaleFrameSeconds)
		{
			Iterator.RemoveCurrent();
		}
	}
}
