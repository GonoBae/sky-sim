#include "../SkySimVolumeReceiver.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Crc.h"

namespace SkySimVolumeReceiverTests
{
	using EResult = FSkySimVolumeReceiver::EPacketResult;

	void WriteU16(TArray<uint8>& Data, int32 Offset, uint16 Value)
	{
		Data[Offset] = static_cast<uint8>(Value);
		Data[Offset + 1] = static_cast<uint8>(Value >> 8);
	}

	void WriteU32(TArray<uint8>& Data, int32 Offset, uint32 Value)
	{
		for (int32 Index = 0; Index < 4; ++Index)
		{
			Data[Offset + Index] = static_cast<uint8>(Value >> (Index * 8));
		}
	}

	void WriteF32(TArray<uint8>& Data, int32 Offset, float Value)
	{
		uint32 Bits = 0;
		FMemory::Memcpy(&Bits, &Value, sizeof(Value));
		WriteU32(Data, Offset, Bits);
	}

	TArray<uint8> MakePacket(uint32 FrameId, const TArray<uint8>& Decoded, const TArray<uint8>& Encoded,
		FIntVector Grid, uint16 Chunk = 0, uint8 Compression = 0, uint8 FieldId = 1, uint16 FieldMask = 1)
	{
		const int32 Offset = Chunk * 1200;
		const int32 PayloadBytes = FMath::Min(1200, Encoded.Num() - Offset);
		TArray<uint8> Packet;
		Packet.SetNumZeroed(64 + PayloadBytes);
		FMemory::Memcpy(Packet.GetData(), "CLD2", 4);
		WriteU16(Packet, 4, 2);
		WriteU16(Packet, 6, 64);
		WriteU32(Packet, 8, FrameId);
		WriteU32(Packet, 12, FCrc::MemCrc32(Decoded.GetData(), Decoded.Num()));
		const double SimulationTime = 123.25;
		uint64 TimeBits = 0;
		FMemory::Memcpy(&TimeBits, &SimulationTime, sizeof(SimulationTime));
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Packet[16 + Index] = static_cast<uint8>(TimeBits >> (Index * 8));
		}
		WriteU16(Packet, 24, static_cast<uint16>(Grid.X));
		WriteU16(Packet, 26, static_cast<uint16>(Grid.Y));
		WriteU16(Packet, 28, static_cast<uint16>(Grid.Z));
		Packet[30] = FieldId == 1 ? 2 : FieldId == 2 ? 4 : 1;
		Packet[31] = FieldId;
		Packet[32] = FieldId == 2 ? 3 : 1;
		Packet[33] = Compression;
		WriteU16(Packet, 34, Chunk);
		WriteU16(Packet, 36, static_cast<uint16>((Encoded.Num() + 1199) / 1200));
		WriteU16(Packet, 38, static_cast<uint16>(PayloadBytes));
		WriteU16(Packet, 40, FieldMask);
		WriteU32(Packet, 44, Offset);
		WriteU32(Packet, 48, Encoded.Num());
		WriteU32(Packet, 52, Decoded.Num());
		WriteF32(Packet, 56, 0.125f);
		WriteF32(Packet, 60, -0.5f);
		FMemory::Memcpy(Packet.GetData() + 64, Encoded.GetData() + Offset, PayloadBytes);
		return Packet;
	}

	EResult Consume(FSkySimVolumeReceiver& Receiver, const TArray<uint8>& Packet,
		FSkySimDensityFrame& OutFrame, double Time = 10.0)
	{
		return Receiver.Consume(Packet.GetData(), Packet.Num(), Time, OutFrame);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimVolumeRawTest, "SkySim.VolumeReceiver.RawAndMetadata",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimVolumeRawTest::RunTest(const FString& Parameters)
{
	using namespace SkySimVolumeReceiverTests;
	FSkySimVolumeReceiver Receiver;
	FSkySimDensityFrame Frame;
	const TArray<uint8> Bytes = {0, 0, 255, 255, 0, 128, 1, 0};
	const TArray<uint8> Packet = MakePacket(42, Bytes, Bytes, FIntVector(2, 2, 1));
	TestTrue(TEXT("Raw density completes"), Consume(Receiver, Packet, Frame, 10.5) == EResult::FrameComplete);
	TestTrue(TEXT("Completed density is valid"), Frame.IsValid());
	TestEqual(TEXT("Frame ID"), Frame.FrameId, 42u);
	TestEqual(TEXT("Simulation time"), Frame.SimulationTime, 123.25);
	TestEqual(TEXT("Receipt time"), Frame.ReceivePlatformSeconds, 10.5);
	TestEqual(TEXT("Grid"), Frame.GridSize, FIntVector(2, 2, 1));
	TestEqual(TEXT("Scale"), Frame.ValueScale, 0.125f);
	TestEqual(TEXT("Bias"), Frame.ValueBias, -0.5f);
	TestTrue(TEXT("Raw bytes preserved"), Frame.Bytes == Bytes);
	TestTrue(TEXT("Null datagram rejected"), Receiver.Consume(nullptr, 64, 11.0, Frame) == EResult::Rejected);
	TestEqual(TEXT("Rejection preserves output ID"), Frame.FrameId, 42u);
	TestTrue(TEXT("Rejection preserves output bytes"), Frame.Bytes == Bytes);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimVolumeRleTest, "SkySim.VolumeReceiver.RleAndCrc",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimVolumeRleTest::RunTest(const FString& Parameters)
{
	using namespace SkySimVolumeReceiverTests;
	FSkySimVolumeReceiver Receiver;
	FSkySimDensityFrame Frame;
	const TArray<uint8> Bytes = {7, 7, 7, 7, 1, 2, 3, 4};
	const TArray<uint8> Rle = {0x81, 7, 3, 1, 2, 3, 4};
	TArray<uint8> Packet = MakePacket(2, Bytes, Rle, FIntVector(4, 1, 1), 0, 1);
	TestTrue(TEXT("RLE run and literal complete"), Consume(Receiver, Packet, Frame) == EResult::FrameComplete);
	TestTrue(TEXT("RLE decoded bytes"), Frame.Bytes == Bytes);
	Packet[12] ^= 1;
	TestTrue(TEXT("Incorrect CRC rejected"), Consume(Receiver, Packet, Frame) == EResult::Rejected);
	const TArray<TArray<uint8>> InvalidRle = {{0x81}, {3, 1}, {0x87, 7}, {0x80, 7}};
	for (const TArray<uint8>& Encoded : InvalidRle)
	{
		Packet = MakePacket(3, Bytes, Encoded, FIntVector(4, 1, 1), 0, 1);
		TestTrue(TEXT("Truncated, oversized or undersized RLE rejected"), Consume(Receiver, Packet, Frame) == EResult::Rejected);
	}
	TestEqual(TEXT("Malformed RLE preserves last output"), Frame.FrameId, 2u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimVolumeChunksTest, "SkySim.VolumeReceiver.OutOfOrderAndDuplicates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimVolumeChunksTest::RunTest(const FString& Parameters)
{
	using namespace SkySimVolumeReceiverTests;
	FSkySimVolumeReceiver Receiver;
	FSkySimDensityFrame Frame;
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(2404);
	for (int32 Index = 0; Index < Bytes.Num(); ++Index)
	{
		Bytes[Index] = static_cast<uint8>(Index);
	}
	const FIntVector Grid(601, 2, 1);
	TestTrue(TEXT("Final chunk can arrive first"), Consume(Receiver, MakePacket(4, Bytes, Bytes, Grid, 2), Frame) == EResult::Accepted);
	const TArray<uint8> FirstChunk = MakePacket(4, Bytes, Bytes, Grid, 0);
	TestTrue(TEXT("First chunk accepted"), Consume(Receiver, FirstChunk, Frame) == EResult::Accepted);
	TestTrue(TEXT("Identical partial duplicate accepted"), Consume(Receiver, FirstChunk, Frame) == EResult::Accepted);
	TestTrue(TEXT("Middle chunk completes frame"), Consume(Receiver, MakePacket(4, Bytes, Bytes, Grid, 1), Frame) == EResult::FrameComplete);
	TestTrue(TEXT("Out-of-order bytes preserved"), Frame.Bytes == Bytes);

	const TArray<uint8> DensityBytes = {0x44, 0x44, 0x44, 0x44};
	const TArray<uint8> RleBytes = {0x81, 0x44};
	const TArray<uint8> TypeBytes = {1, 2};
	for (uint8 Compression = 0; Compression <= 1; ++Compression)
	{
		Receiver.Reset();
		const TArray<uint8> DensityPacket = MakePacket(5, DensityBytes, Compression == 0 ? DensityBytes : RleBytes,
			FIntVector(2, 1, 1), 0, Compression, 1, 5);
		TestTrue(TEXT("Completed density waits for other field"), Consume(Receiver, DensityPacket, Frame) == EResult::Accepted);
		TestEqual(TEXT("Partial frame preserves output"), Frame.FrameId, 4u);
		TestTrue(TEXT("Duplicate of completed field accepted safely"), Consume(Receiver, DensityPacket, Frame) == EResult::Accepted);
		TArray<uint8> Conflict = DensityPacket;
		Conflict.Last() ^= 1;
		TestTrue(TEXT("Conflicting completed-field duplicate rejected"), Consume(Receiver, Conflict, Frame) == EResult::Rejected);
		TestTrue(TEXT("Density can restart after conflict"), Consume(Receiver, DensityPacket, Frame) == EResult::Accepted);
		TestTrue(TEXT("All declared fields required"), Consume(Receiver,
			MakePacket(5, TypeBytes, TypeBytes, FIntVector(2, 1, 1), 0, 0, 3, 5), Frame) == EResult::FrameComplete);
		TestTrue(TEXT("Completed multi-field density preserved"), Frame.Bytes == DensityBytes);
		Frame.FrameId = 4;
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimVolumeValidationTest, "SkySim.VolumeReceiver.MetadataValidation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimVolumeValidationTest::RunTest(const FString& Parameters)
{
	using namespace SkySimVolumeReceiverTests;
	FSkySimVolumeReceiver Receiver;
	FSkySimDensityFrame Frame;
	Frame.FrameId = 99;
	const TArray<uint8> Bytes = {1, 2, 3, 4};
	const TArray<uint8> Base = MakePacket(6, Bytes, Bytes, FIntVector(2, 1, 1));
	const int32 InvalidByteOffsets[] = {0, 4, 6, 24, 30, 31, 32, 33, 34, 36, 38, 40, 42, 44, 48, 52};
	for (int32 Offset : InvalidByteOffsets)
	{
		TArray<uint8> Packet = Base;
		Packet[Offset] = 0xff;
		TestTrue(FString::Printf(TEXT("Invalid header byte at %d rejected"), Offset), Consume(Receiver, Packet, Frame) == EResult::Rejected);
	}
	TArray<uint8> NonFinite = Base;
	SkySimVolumeReceiverTests::WriteU32(NonFinite, 56, 0x7fc00000u);
	TestTrue(TEXT("NaN scale rejected"), Consume(Receiver, NonFinite, Frame) == EResult::Rejected);
	TestTrue(TEXT("Truncated packet rejected"), Receiver.Consume(Base.GetData(), Base.Num() - 1, 10.0, Frame) == EResult::Rejected);

	const TArray<uint8> Density = MakePacket(7, Bytes, Bytes, FIntVector(2, 1, 1), 0, 0, 1, 5);
	TestTrue(TEXT("Density held for types"), Consume(Receiver, Density, Frame) == EResult::Accepted);
	const TArray<uint8> Types = {1, 2};
	TestTrue(TEXT("Inconsistent macro grid rejected"), Consume(Receiver,
		MakePacket(7, Types, Types, FIntVector(1, 2, 1), 0, 0, 3, 5), Frame) == EResult::Rejected);
	TestEqual(TEXT("Invalid packets preserve output"), Frame.FrameId, 99u);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FSkySimVolumeLifetimeTest, "SkySim.VolumeReceiver.PruneResetAndCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FSkySimVolumeLifetimeTest::RunTest(const FString& Parameters)
{
	using namespace SkySimVolumeReceiverTests;
	FSkySimVolumeReceiver Receiver;
	FSkySimDensityFrame Frame;
	TArray<uint8> Bytes;
	Bytes.Init(17, 2400);
	const FIntVector Grid(1200, 1, 1);
	const TArray<uint8> First = MakePacket(8, Bytes, Bytes, Grid, 0);
	const TArray<uint8> Last = MakePacket(8, Bytes, Bytes, Grid, 1);
	Consume(Receiver, First, Frame, 0.0);
	Receiver.Prune(1.01);
	TestTrue(TEXT("Stale first chunk discarded"), Consume(Receiver, Last, Frame, 1.02) == EResult::Accepted);
	TestTrue(TEXT("Re-received first chunk completes"), Consume(Receiver, First, Frame, 1.03) == EResult::FrameComplete);
	Consume(Receiver, First, Frame, 2.0);
	Receiver.Reset();
	TestTrue(TEXT("Reset removes partial frame"), Consume(Receiver, Last, Frame, 2.01) == EResult::Accepted);
	Receiver.Reset();
	for (uint32 Index = 0; Index < 9; ++Index)
	{
		TestTrue(TEXT("Partial frame accepted within bounded assembly"), Consume(Receiver,
			MakePacket(20 + Index, Bytes, Bytes, Grid, 0), Frame, 3.0 + Index * 0.01) == EResult::Accepted);
	}
	TestTrue(TEXT("Newest frame retained"), Consume(Receiver,
		MakePacket(28, Bytes, Bytes, Grid, 1), Frame, 3.10) == EResult::FrameComplete);
	TestTrue(TEXT("Ninth frame evicts oldest"), Consume(Receiver,
		MakePacket(20, Bytes, Bytes, Grid, 1), Frame, 3.11) == EResult::Accepted);
	TestTrue(TEXT("Second oldest still retained"), Consume(Receiver,
		MakePacket(21, Bytes, Bytes, Grid, 1), Frame, 3.12) == EResult::FrameComplete);
	return true;
}

#endif
