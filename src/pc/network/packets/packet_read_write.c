#include "../network.h"
#include "game/area.h"
#include "pc/debuglog.h"

#define PACKET_FLAG_BUFFER_OFFSET        3
#define PACKET_DESTINATION_BUFFER_OFFSET 4
#define PACKET_ORDERED_SEQ_ID_OFFSET     8

static u16 sNextSeqNum = 1;

static bool sOrderedPackets = false;
static u16 sCurrentOrderedGroupId = 0;
static u16 sCurrentOrderedSeqId = 0;

#ifdef TARGET_WII_U
static bool packet_needs_scalar_swap(u16 length) {
    return length == sizeof(u16) || length == sizeof(u32) || length == sizeof(u64);
}

static void packet_copy_bytes_reversed(void* dst, const void* src, u16 length) {
    const u8* s = (const u8*)src;
    u8* d = (u8*)dst;
    for (u16 i = 0; i < length; i++) {
        d[i] = s[length - 1 - i];
    }
}

static void packet_store_u16_le(u8* dst, u16 value) {
    dst[0] = (u8)(value & 0xFF);
    dst[1] = (u8)((value >> 8) & 0xFF);
}

static u32 packet_load_u32_le(const u8* src) {
    return (u32)src[0]
        | ((u32)src[1] << 8)
        | ((u32)src[2] << 16)
        | ((u32)src[3] << 24);
}
#else
static void packet_store_u16_le(u8* dst, u16 value) {
    memcpy(dst, &value, sizeof(u16));
}

static u32 packet_load_u32_le(const u8* src) {
    u32 value = 0;
    memcpy(&value, src, sizeof(u32));
    return value;
}
#endif

void packet_init(struct Packet* packet, enum PacketType packetType, bool reliable, enum PacketLevelMatchType levelAreaMustMatch) {
    memset(packet->buffer, 0, PACKET_LENGTH);
    packet->packetType = packetType;
    packet->cursor = 0;
    packet->dataLength = 0;
    packet->error = false;
    packet->writeError = false;
    packet->reliable = reliable;
    packet->levelAreaMustMatch = (levelAreaMustMatch == PLMT_AREA);
    packet->levelMustMatch     = (levelAreaMustMatch == PLMT_LEVEL);
    packet->requestBroadcast = false;
    packet->sent = false;
    packet->orderedFromGlobalId = sOrderedPackets ? gNetworkPlayerLocal->globalIndex : 0;
    packet->orderedGroupId      = sOrderedPackets ? sCurrentOrderedGroupId : 0;
    packet->orderedSeqId        = 0;
    packet->keepSendingAfterDisconnect = false;

    // Enum storage is host-endian sized as int; always serialize packet type as a true u8.
    u8 packetTypeByte = (u8)packetType;
    packet_write(packet, &packetTypeByte, sizeof(u8));

    // write seq number
    if (reliable) {
        packet_write(packet, &sNextSeqNum, sizeof(u16));
        packet->seqId = sNextSeqNum;
        sNextSeqNum++;
        if (sNextSeqNum == 0) { sNextSeqNum++;  }
    } else {
        u16 nullSeqNum = 0;
        packet_write(packet, &nullSeqNum, sizeof(u16));
    }

    // write packet flags
    u8 flags = 0;
    packet_write(packet, &flags, sizeof(u8)); // fill in the byte
    packet_set_flags(packet);

    // write destination
    u8 destination = PACKET_DESTINATION_BROADCAST;
    packet_write(packet, &destination, sizeof(u8));

    // write ordered packet information
    if (sOrderedPackets) {
        packet_write(packet, &packet->orderedFromGlobalId, sizeof(u8));
        packet_write(packet, &packet->orderedGroupId,      sizeof(u16));
        packet_write(packet, &packet->orderedSeqId,        sizeof(u16));
    }

    // write location
    if (packet->levelAreaMustMatch) {
        // These globals are s16s. Serialize them through true u8 temporaries so
        // big-endian targets don't copy the high byte into the wire header.
        u8 courseNum = (u8)gCurrCourseNum;
        u8 actNum = (u8)gCurrActStarNum;
        u8 areaIndex = (u8)gCurrAreaIndex;
        packet_write(packet, &courseNum,      sizeof(u8));
        packet_write(packet, &actNum,         sizeof(u8));
        packet_write(packet, &gCurrLevelNum,   sizeof(s16));
        packet_write(packet, &areaIndex,       sizeof(u8));
        packet->courseNum = gCurrCourseNum;
        packet->actNum    = gCurrActStarNum;
        packet->levelNum  = gCurrLevelNum;
        packet->areaIndex = gCurrAreaIndex;
    } else if (packet->levelMustMatch) {
        u8 courseNum = (u8)gCurrCourseNum;
        u8 actNum = (u8)gCurrActStarNum;
        packet_write(packet, &courseNum,       sizeof(u8));
        packet_write(packet, &actNum,          sizeof(u8));
        packet_write(packet, &gCurrLevelNum,   sizeof(s16));
        packet->courseNum = gCurrCourseNum;
        packet->actNum    = gCurrActStarNum;
        packet->levelNum  = gCurrLevelNum;
    }
}

void packet_duplicate(struct Packet* srcPacket, struct Packet* dstPacket) {
    memset(dstPacket->buffer, 0, PACKET_LENGTH);
    dstPacket->packetType = srcPacket->packetType;
    dstPacket->cursor = 0;
    dstPacket->dataLength = 0;
    dstPacket->error = srcPacket->error;
    dstPacket->writeError = srcPacket->writeError;
    dstPacket->reliable = srcPacket->reliable;
    dstPacket->levelAreaMustMatch = srcPacket->levelAreaMustMatch;
    dstPacket->levelMustMatch = srcPacket->levelMustMatch;
    dstPacket->requestBroadcast = srcPacket->requestBroadcast;
    dstPacket->destGlobalId = srcPacket->destGlobalId;
    dstPacket->sent = false;
    dstPacket->orderedGroupId = srcPacket->orderedGroupId;
    dstPacket->orderedSeqId = srcPacket->orderedSeqId;
    dstPacket->courseNum = srcPacket->courseNum;
    dstPacket->actNum    = srcPacket->actNum;
    dstPacket->levelNum  = srcPacket->levelNum;
    dstPacket->areaIndex = srcPacket->areaIndex;

#ifdef DEBUG
    assert(srcPacket->dataLength <= PACKET_LENGTH);
#endif
    memcpy(&dstPacket->buffer[0], &srcPacket->buffer[0], srcPacket->dataLength);

    if (dstPacket->reliable) {
        dstPacket->seqId = sNextSeqNum;
        sNextSeqNum++;
        if (sNextSeqNum == 0) { sNextSeqNum++; }
    } else {
        dstPacket->seqId = 0;
    }
    packet_store_u16_le(&dstPacket->buffer[1], dstPacket->seqId);

    dstPacket->dataLength = srcPacket->dataLength;
    dstPacket->cursor = dstPacket->dataLength;
}

void packet_set_flags(struct Packet* packet) {
    u8 flags = 0;
    flags |= SET_BIT(packet->levelAreaMustMatch,  0);
    flags |= SET_BIT(packet->requestBroadcast,    1);
    flags |= SET_BIT(packet->orderedGroupId != 0, 2);
    flags |= SET_BIT(packet->levelMustMatch,      3);
    packet->buffer[PACKET_FLAG_BUFFER_OFFSET] = flags;
}

void packet_set_destination(struct Packet* packet, u8 destGlobalId) {
    packet->buffer[PACKET_DESTINATION_BUFFER_OFFSET] = destGlobalId;
}

void packet_write(struct Packet* packet, void* data, u16 length) {
    if (data == NULL) { packet->error = true; return; }
#ifdef DEBUG
    assert(packet->dataLength + length <= PACKET_LENGTH);
#endif

    if (packet->cursor + length >= PACKET_LENGTH) {
        SOFT_ASSERT(packet->cursor + length < PACKET_LENGTH);
        packet->writeError = true;
    }
    u8* dst = &packet->buffer[packet->cursor];
#ifdef TARGET_WII_U
    // CoopDX network wire format is little-endian; swap primitive scalars on big-endian Wii U.
    if (packet_needs_scalar_swap(length)) {
        packet_copy_bytes_reversed(dst, data, length);
    } else {
        memcpy(dst, data, length);
    }
#else
    memcpy(dst, data, length);
#endif
    packet->dataLength += length;
    packet->cursor     += length;
}

void packet_write_bytes(struct Packet* packet, const void* data, u16 length) {
    if (data == NULL) { packet->error = true; return; }
#ifdef DEBUG
    assert(packet->dataLength + length <= PACKET_LENGTH);
#endif
    if (packet->cursor + length >= PACKET_LENGTH) {
        SOFT_ASSERT(packet->cursor + length < PACKET_LENGTH);
        packet->writeError = true;
    }
    memcpy(&packet->buffer[packet->cursor], data, length);
    packet->dataLength += length;
    packet->cursor     += length;
}

u8 packet_initial_read(struct Packet* packet) {
    // read packet type
    packet->packetType = packet->buffer[0];

    // read packet flags
    u8 flags = 0;
    packet_read(packet, &flags, sizeof(u8));
    packet->levelAreaMustMatch = GET_BIT(flags, 0);
    packet->requestBroadcast   = GET_BIT(flags, 1);
    bool packetIsOrdered       = GET_BIT(flags, 2);
    packet->levelMustMatch     = GET_BIT(flags, 3);

    // read destination
    packet_read(packet, &packet->destGlobalId, sizeof(u8));

    // read ordered packet information
    if (packetIsOrdered) {
        packet_read(packet, &packet->orderedFromGlobalId, sizeof(u8));
        packet_read(packet, &packet->orderedGroupId,      sizeof(u16));
        packet_read(packet, &packet->orderedSeqId,        sizeof(u16));
    }

    // read location
    if (packet->levelAreaMustMatch) {
        packet_read(packet, &packet->courseNum, sizeof(u8));
        packet_read(packet, &packet->actNum,    sizeof(u8));
        packet_read(packet, &packet->levelNum,  sizeof(s16));
        packet_read(packet, &packet->areaIndex, sizeof(u8));
    } else if (packet->levelMustMatch) {
        packet_read(packet, &packet->courseNum, sizeof(u8));
        packet_read(packet, &packet->actNum,    sizeof(u8));
        packet_read(packet, &packet->levelNum,  sizeof(s16));
    }

    // don't drop packet
    return TRUE;
}

void packet_read(struct Packet* packet, void* data, u16 length) {
    u16 cursor = packet->cursor;
    if (length == 0) { return; }
    if (data == NULL) { packet->error = true; return; }
    if (cursor + length >= PACKET_LENGTH) { packet->error = true; return; }

    const u8* src = &packet->buffer[cursor];
#ifdef TARGET_WII_U
    if (packet_needs_scalar_swap(length)) {
        packet_copy_bytes_reversed(data, src, length);
    } else {
        memcpy(data, src, length);
    }
#else
    memcpy(data, src, length);
#endif
    packet->cursor = cursor + length;
}

void packet_read_bytes(struct Packet* packet, void* data, u16 length) {
    u16 cursor = packet->cursor;
    if (length == 0) { return; }
    if (data == NULL) { packet->error = true; return; }
    if (cursor + length >= PACKET_LENGTH) { packet->error = true; return; }
    memcpy(data, &packet->buffer[cursor], length);
    packet->cursor = cursor + length;
}

u32 packet_hash(struct Packet* packet) {
    u32 hash = 0;
    u16 byte = 0;
    for (u16 i = 0; i < packet->dataLength; i++) {
        hash ^= ((u32)packet->buffer[i]) << (8 * byte);
        byte = (byte + 1) % sizeof(u32);
    }
    return hash;
}

bool packet_check_hash(struct Packet* packet) {
    u32 localHash = packet_hash(packet);
    u32 packetHash = packet_load_u32_le(&packet->buffer[packet->dataLength]);
    return localHash == packetHash;
}

void packet_ordered_begin(void) {
    if (sOrderedPackets) { return; }
    sOrderedPackets = true;

    sCurrentOrderedGroupId++;
    if (sCurrentOrderedGroupId == 0) { sCurrentOrderedGroupId++; }
    sCurrentOrderedSeqId = 1;
}

void packet_ordered_end(void) {
    sOrderedPackets = false;
    sCurrentOrderedSeqId = 0;
}

void packet_set_ordered_data(struct Packet* packet) {
    if (packet->orderedGroupId == 0) { return; }
    if (packet->orderedSeqId != 0) { return; }
    packet->orderedSeqId = sCurrentOrderedSeqId++;
    packet_store_u16_le(&packet->buffer[PACKET_ORDERED_SEQ_ID_OFFSET], packet->orderedSeqId);
}
