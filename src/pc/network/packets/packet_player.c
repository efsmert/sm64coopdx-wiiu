#include <stdio.h>
#include <stddef.h>
#include "../network.h"
#include "object_fields.h"
#include "object_constants.h"
#include "sm64.h"
#include "game/interaction.h"
#include "game/mario.h"
#include "game/area.h"
#include "audio/external.h"
#include "engine/surface_collision.h"
#include "engine/math_util.h"
#include "game/object_list_processor.h"
#include "game/mario_misc.h"
#include "pc/configfile.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_language.h"
#include "pc/debuglog.h"
#include "src/game/hardcoded.h"

#pragma pack(1)
struct PacketPlayerData {
    u32 rawData[OBJECT_NUM_REGULAR_FIELDS];

    s16 cRawStickX;
    s16 cRawStickY;
    f32 cStickX;
    f32 cStickY;
    f32 cStickMag;
    u16 cButtonDown;
    u16 cButtonPressed;
    u16 cButtonReleased;
    s16 cExtStickX;
    s16 cExtStickY;

    s16 nodeFlags;

    u16 input;
    u32 flags;
    u32 particleFlags;
    u32 action;
    u32 prevAction;
    u16 actionState;
    u16 actionTimer;
    u32 actionArg;
    f32 intendedMag;
    s16 intendedYaw;
    s16 invincTimer;
    u8  framesSinceA;
    u8  framesSinceB;
    u8  wallKickTimer;
    u8  doubleJumpTimer;
    Vec3s faceAngle;
    Vec3s angleVel;
    s16 slideYaw;
    s16 twirlYaw;
    Vec3f pos;
    Vec3f vel;
    f32 forwardVel;
    f32 slideVelX;
    f32 slideVelZ;
    s16 health;
    u8  squishTimer;
    f32 peakHeight;
    s16 currentRoom;
    Vec3s headRotation;

    u8  customFlags;
    u32 heldSyncID;
    u32 heldBySyncID;
    u32 riddenSyncID;
    u32 interactSyncID;
    u32 usedSyncID;
    u32 platformSyncID;

    u8 levelSyncValid;
    u8 areaSyncValid;
    u8 knockbackTimer;

    s32 dialogId;
};
#pragma pack()

#ifdef TARGET_WII_U
static inline u16 packet_player_bswap_u16(u16 value) {
    return __builtin_bswap16(value);
}

static inline u32 packet_player_bswap_u32(u32 value) {
    return __builtin_bswap32(value);
}

static inline f32 packet_player_bswap_f32(f32 value) {
    u32 raw = 0;
    memcpy(&raw, &value, sizeof(raw));
    raw = packet_player_bswap_u32(raw);
    memcpy(&value, &raw, sizeof(value));
    return value;
}

static void packet_player_data_swap_endian(struct PacketPlayerData* data) {
    if (data == NULL) { return; }

    for (u32 i = 0; i < OBJECT_NUM_REGULAR_FIELDS; i++) {
        data->rawData[i] = packet_player_bswap_u32(data->rawData[i]);
    }

    data->cRawStickX = (s16)packet_player_bswap_u16((u16)data->cRawStickX);
    data->cRawStickY = (s16)packet_player_bswap_u16((u16)data->cRawStickY);
    data->cStickX = packet_player_bswap_f32(data->cStickX);
    data->cStickY = packet_player_bswap_f32(data->cStickY);
    data->cStickMag = packet_player_bswap_f32(data->cStickMag);
    data->cButtonDown = packet_player_bswap_u16(data->cButtonDown);
    data->cButtonPressed = packet_player_bswap_u16(data->cButtonPressed);
    data->cButtonReleased = packet_player_bswap_u16(data->cButtonReleased);
    data->cExtStickX = (s16)packet_player_bswap_u16((u16)data->cExtStickX);
    data->cExtStickY = (s16)packet_player_bswap_u16((u16)data->cExtStickY);
    data->nodeFlags = (s16)packet_player_bswap_u16((u16)data->nodeFlags);
    data->input = packet_player_bswap_u16(data->input);
    data->flags = packet_player_bswap_u32(data->flags);
    data->particleFlags = packet_player_bswap_u32(data->particleFlags);
    data->action = packet_player_bswap_u32(data->action);
    data->prevAction = packet_player_bswap_u32(data->prevAction);
    data->actionState = packet_player_bswap_u16(data->actionState);
    data->actionTimer = packet_player_bswap_u16(data->actionTimer);
    data->actionArg = packet_player_bswap_u32(data->actionArg);
    data->intendedMag = packet_player_bswap_f32(data->intendedMag);
    data->intendedYaw = (s16)packet_player_bswap_u16((u16)data->intendedYaw);
    data->invincTimer = (s16)packet_player_bswap_u16((u16)data->invincTimer);

    for (u32 i = 0; i < 3; i++) {
        data->faceAngle[i] = (s16)packet_player_bswap_u16((u16)data->faceAngle[i]);
        data->angleVel[i] = (s16)packet_player_bswap_u16((u16)data->angleVel[i]);
        data->pos[i] = packet_player_bswap_f32(data->pos[i]);
        data->vel[i] = packet_player_bswap_f32(data->vel[i]);
        data->headRotation[i] = (s16)packet_player_bswap_u16((u16)data->headRotation[i]);
    }

    data->slideYaw = (s16)packet_player_bswap_u16((u16)data->slideYaw);
    data->twirlYaw = (s16)packet_player_bswap_u16((u16)data->twirlYaw);
    data->forwardVel = packet_player_bswap_f32(data->forwardVel);
    data->slideVelX = packet_player_bswap_f32(data->slideVelX);
    data->slideVelZ = packet_player_bswap_f32(data->slideVelZ);
    data->health = (s16)packet_player_bswap_u16((u16)data->health);
    data->peakHeight = packet_player_bswap_f32(data->peakHeight);
    data->currentRoom = (s16)packet_player_bswap_u16((u16)data->currentRoom);
    data->heldSyncID = packet_player_bswap_u32(data->heldSyncID);
    data->heldBySyncID = packet_player_bswap_u32(data->heldBySyncID);
    data->riddenSyncID = packet_player_bswap_u32(data->riddenSyncID);
    data->interactSyncID = packet_player_bswap_u32(data->interactSyncID);
    data->usedSyncID = packet_player_bswap_u32(data->usedSyncID);
    data->platformSyncID = packet_player_bswap_u32(data->platformSyncID);
    data->dialogId = (s32)packet_player_bswap_u32((u32)data->dialogId);
}
#endif

static void read_packet_data(struct PacketPlayerData* data, struct MarioState* m) {
    u32 heldSyncID     = (m->heldObj != NULL)            ? m->heldObj->oSyncID            : 0;
    u32 heldBySyncID   = (m->heldByObj != NULL)          ? m->heldByObj->oSyncID          : 0;
    u32 riddenSyncID   = (m->riddenObj != NULL)          ? m->riddenObj->oSyncID          : 0;
    u32 interactSyncID = (m->interactObj != NULL)        ? m->interactObj->oSyncID        : 0;
    u32 usedSyncID     = (m->usedObj != NULL)            ? m->usedObj->oSyncID            : 0;
    u32 platformSyncID = (m->marioObj->platform != NULL) ? m->marioObj->platform->oSyncID : 0;

    u8 customFlags     = SET_BIT((m->freeze > 0), 0);

    memcpy(data->rawData, m->marioObj->rawData.asU32, sizeof(u32) * OBJECT_NUM_REGULAR_FIELDS);
    data->nodeFlags    = m->marioObj->header.gfx.node.flags;

    data->cRawStickX      = m->controller->rawStickX;
    data->cRawStickY      = m->controller->rawStickY;
    data->cStickX         = m->controller->stickX;
    data->cStickY         = m->controller->stickY;
    data->cStickMag       = m->controller->stickMag;
    data->cButtonDown     = m->controller->buttonDown;
    data->cButtonPressed  = m->controller->buttonPressed;
    data->cButtonReleased = m->controller->buttonReleased;
    data->cExtStickX      = m->controller->extStickX;
    data->cExtStickY      = m->controller->extStickY;

    data->input           = m->input;
    data->flags           = m->flags;
    data->particleFlags   = m->particleFlags;
    data->action          = m->action;
    data->prevAction      = m->prevAction;
    data->actionState     = m->actionState;
    data->actionTimer     = m->actionTimer;
    data->actionArg       = m->actionArg;
    data->intendedMag     = m->intendedMag;
    data->intendedYaw     = m->intendedYaw;
    data->invincTimer     = m->invincTimer;
    data->framesSinceA    = m->framesSinceA;
    data->framesSinceB    = m->framesSinceB;
    data->wallKickTimer   = m->wallKickTimer;
    data->doubleJumpTimer = m->doubleJumpTimer;
    memcpy(data->faceAngle, m->faceAngle, sizeof(s16) * 3);
    memcpy(data->angleVel,  m->angleVel,  sizeof(s16) * 3);
    data->slideYaw        = m->slideYaw;
    data->twirlYaw        = m->twirlYaw;
    memcpy(data->pos, m->pos, sizeof(f32) * 3);
    memcpy(data->vel, m->vel, sizeof(f32) * 3);
    data->forwardVel      = m->forwardVel;
    data->slideVelX       = m->slideVelX;
    data->slideVelZ       = m->slideVelZ;
    data->health          = m->health;
    data->squishTimer     = m->squishTimer;
    data->peakHeight      = m->peakHeight;
    data->currentRoom     = m->currentRoom;
    memcpy(data->headRotation, gPlayerCameraState[m->playerIndex].headRotation, sizeof(s16) * 3);

    data->customFlags    = customFlags;
    data->heldSyncID     = heldSyncID;
    data->heldBySyncID   = heldBySyncID;
    data->riddenSyncID   = riddenSyncID;
    data->interactSyncID = interactSyncID;
    data->usedSyncID     = usedSyncID;
    data->platformSyncID = platformSyncID;

    struct NetworkPlayer* np = &gNetworkPlayers[m->playerIndex];
    data->areaSyncValid  = np->currAreaSyncValid;
    data->levelSyncValid = np->currLevelSyncValid;

    data->knockbackTimer = m->knockbackTimer;

    data->dialogId = get_dialog_id();
}

static void write_packet_data(struct PacketPlayerData* data, struct MarioState* m,
                              u8* customFlags, u32* heldSyncID, u32* heldBySyncID,
                              u32* riddenSyncID, u32* interactSyncID, u32* usedSyncID,
                              u32* platformSyncID) {
    memcpy(m->marioObj->rawData.asU32, data->rawData, sizeof(u32) * OBJECT_NUM_REGULAR_FIELDS);
    m->marioObj->header.gfx.node.flags = data->nodeFlags;

    m->controller->rawStickX      = data->cRawStickX;
    m->controller->rawStickY      = data->cRawStickY;
    m->controller->stickX         = data->cStickX;
    m->controller->stickY         = data->cStickY;
    m->controller->stickMag       = data->cStickMag;
    m->controller->buttonDown     = data->cButtonDown;
    m->controller->buttonPressed  = data->cButtonPressed;
    m->controller->buttonReleased = data->cButtonReleased;
    m->controller->extStickX      = data->cExtStickX;
    m->controller->extStickY      = data->cExtStickY;

    m->input           = data->input;
    m->flags           = data->flags;
    m->particleFlags   = data->particleFlags;
    m->action          = data->action;
    m->prevAction      = data->prevAction;
    m->actionState     = data->actionState;
    m->actionTimer     = data->actionTimer;
    m->actionArg       = data->actionArg;
    m->intendedMag     = data->intendedMag;
    m->intendedYaw     = data->intendedYaw;
    m->invincTimer     = data->invincTimer;
    m->framesSinceA    = data->framesSinceA;
    m->framesSinceB    = data->framesSinceB;
    m->wallKickTimer   = data->wallKickTimer;
    m->doubleJumpTimer = data->doubleJumpTimer;
    memcpy(m->faceAngle, data->faceAngle, sizeof(s16) * 3);
    memcpy(m->angleVel,  data->angleVel,  sizeof(s16) * 3);
    m->slideYaw        = data->slideYaw;
    m->twirlYaw        = data->twirlYaw;
    memcpy(m->pos, data->pos, sizeof(f32) * 3);
    memcpy(m->vel, data->vel, sizeof(f32) * 3);
    m->forwardVel      = data->forwardVel;
    m->slideVelX       = data->slideVelX;
    m->slideVelZ       = data->slideVelZ;
    m->health          = data->health;
    m->squishTimer     = data->squishTimer;
    m->peakHeight      = data->peakHeight;
    m->currentRoom     = data->currentRoom;
    memcpy(gPlayerCameraState[m->playerIndex].headRotation, data->headRotation, sizeof(s16) * 3);

    *customFlags    = data->customFlags;
    *heldSyncID     = data->heldSyncID;
    *heldBySyncID   = data->heldBySyncID;
    *riddenSyncID   = data->riddenSyncID;
    *interactSyncID = data->interactSyncID;
    *usedSyncID     = data->usedSyncID;
    *platformSyncID = data->platformSyncID;

    if (gNetworkType != NT_SERVER) {
        struct NetworkPlayer* np = &gNetworkPlayers[m->playerIndex];
        np->currAreaSyncValid  = data->areaSyncValid;
        np->currLevelSyncValid = data->levelSyncValid;
    }

    m->knockbackTimer = data->knockbackTimer;

    m->dialogId = data->dialogId;
}

void network_send_player(u8 localIndex) {
    if (gMarioStates[localIndex].marioObj == NULL) { return; }
    if (gDjuiInMainMenu) { return; }
    if (gNetworkPlayerLocal == NULL || !gNetworkPlayerLocal->currAreaSyncValid) { return; }

    struct PacketPlayerData data = { 0 };
    read_packet_data(&data, &gMarioStates[localIndex]);

    struct Packet p = { 0 };
    packet_init(&p, PACKET_PLAYER, false, PLMT_AREA);
    packet_write(&p, &gNetworkPlayers[localIndex].globalIndex, sizeof(u8));
#ifdef TARGET_WII_U
    struct PacketPlayerData wireData = data;
    packet_player_data_swap_endian(&wireData);
    packet_write(&p, &wireData, sizeof(struct PacketPlayerData));
#else
    packet_write(&p, &data, sizeof(struct PacketPlayerData));
#endif
    network_send(&p);
}

void network_receive_player(struct Packet* p) {
    u8 globalIndex = 0;
    packet_read(p, &globalIndex, sizeof(u8));
    struct NetworkPlayer* np = network_player_from_global_index(globalIndex);
    if (np == NULL || np->localIndex == UNKNOWN_LOCAL_INDEX || !np->connected) { return; }

    // prevent receiving a packet about our player
    if (gNetworkPlayerLocal && globalIndex == gNetworkPlayerLocal->globalIndex) { return; }

    struct MarioState* m = &gMarioStates[np->localIndex];
    if (m == NULL || m->marioObj == NULL) { return; }

    if (gNetworkType == NT_SERVER && *((u32*)(p->buffer + p->cursor + offsetof(struct PacketPlayerData, action))) == ACT_DEBUG_FREE_MOVE) {
#ifdef DEVELOPMENT
        if (m->action != ACT_DEBUG_FREE_MOVE) {
            construct_player_popup(np, DLANG(NOTIF, DEBUG_FLY), NULL);
        }
#else
        network_send_kick(np->localIndex, EKT_KICKED);
        network_player_disconnected(np->localIndex);
        return;
#endif
    }

    // prevent receiving player from other area
    bool levelAreaMismatch = ((gNetworkPlayerLocal == NULL)
        || np->currCourseNum != gNetworkPlayerLocal->currCourseNum
        || np->currActNum    != gNetworkPlayerLocal->currActNum
        || np->currLevelNum  != gNetworkPlayerLocal->currLevelNum
        || np->currAreaIndex != gNetworkPlayerLocal->currAreaIndex);
    if (levelAreaMismatch) { np->currPositionValid = false; return; }

    // save previous state
    struct PacketPlayerData oldData = { 0 };
    read_packet_data(&oldData, m);
    u16 playerIndex  = np->localIndex;
    u32 oldBehParams = m->marioObj->oBehParams;

    // load mario information from packet
    struct PacketPlayerData data = { 0 };
    packet_read(p, &data, sizeof(struct PacketPlayerData));
#ifdef TARGET_WII_U
    packet_player_data_swap_endian(&data);
#endif

    // check to see if we should just drop this packet
    if (oldData.action == ACT_JUMBO_STAR_CUTSCENE && data.action == ACT_JUMBO_STAR_CUTSCENE) {
        return;
    }

    // apply data from packet to mario state
    u32 heldSyncID     = 0;
    u32 heldBySyncID   = 0;
    u32 riddenSyncID   = 0;
    u32 interactSyncID = 0;
    u32 usedSyncID     = 0;
    u32 platformSyncID = 0;
    u8  customFlags    = 0;
    write_packet_data(&data, m, &customFlags,
                      &heldSyncID, &heldBySyncID,
                      &riddenSyncID, &interactSyncID,
                      &usedSyncID, &platformSyncID);

    // read custom flags
    m->freeze = GET_BIT(customFlags, 0);

    // reset player index
    m->playerIndex = playerIndex;
    m->marioObj->oBehParams = oldBehParams;

    // reset mario sound play flag so that their jump sounds work
    if (m->action != oldData.action) {
        m->flags &= ~(MARIO_ACTION_SOUND_PLAYED | MARIO_MARIO_SOUND_PLAYED);
    }

    // find and set their held object
    struct SyncObject* heldSo = sync_object_get(heldSyncID);
    m->heldObj = heldSo ? heldSo->o : NULL;
    if (m->heldObj != NULL) {
        if (gMarioStates[0].heldObj == m->heldObj && np->globalIndex < gNetworkPlayerLocal->globalIndex) {
            // drop the object if a higher priority player is holding our object
            mario_drop_held_object(&gMarioStates[0]);
            force_idle_state(&gMarioStates[0]);
        }
        m->heldObj->oHeldState = HELD_HELD;
        m->heldObj->heldByPlayerIndex = np->localIndex;
    }

    // find and set their held-by object
    struct SyncObject* heldBySo = sync_object_get(heldBySyncID);
    if (heldBySo && heldBySo->o) {
        // TODO: do we have to move graphics nodes around to make this visible?
        m->heldByObj = heldBySo->o;
    } else {
        m->heldByObj = NULL;
    }

    // find and set their ridden object
    struct SyncObject* riddenSo = sync_object_get(riddenSyncID);
    if (riddenSo && riddenSo->o) {
        riddenSo->o->heldByPlayerIndex = np->localIndex;
        m->riddenObj = riddenSo->o;
    } else {
        m->riddenObj = NULL;
    }

    // find and set their interact object
    struct SyncObject* interactSo = sync_object_get(interactSyncID);
    if (interactSo && interactSo->o) {
        m->interactObj = interactSo->o;
    }

    // find and set their used object
    struct SyncObject* usedSo = sync_object_get(usedSyncID);
    if (usedSo && usedSo->o != NULL) {
        m->usedObj = usedSo->o;
    }

    // place on top of platform
    struct SyncObject* platformSo = sync_object_get(platformSyncID);
    if (platformSo && platformSo->o) {

        // search up to 500 units for the platform
        f32 maxDifference = 500;

        // look for a platform above and below, and a ceiling above the player
        gCheckingSurfaceCollisionsForObject = platformSo->o;
        f32 currFloorHeight = find_floor_height(m->pos[0], m->pos[1], m->pos[2]);
        f32 floorHeight = find_floor_height(m->pos[0], m->pos[1] + maxDifference, m->pos[2]);
        f32 ceilHeight = find_ceil_height(m->pos[0], m->pos[1], m->pos[2]);
        gCheckingSurfaceCollisionsForObject = NULL;

        // always prefer the closest floor
        // use the floor below if there's a ceiling between the player and the floor above
        // only accept floors 500 units away
        f32 diffAbove = ABS(m->pos[1] - floorHeight);
        f32 diffBelow = ABS(m->pos[1] - currFloorHeight);
        if (floorHeight != gLevelValues.floorLowerLimit &&
            (currFloorHeight == gLevelValues.floorLowerLimit || diffBelow > diffAbove) &&
            (ceilHeight == gLevelValues.cellHeightLimit || floorHeight < ceilHeight) &&
            diffAbove <= maxDifference) {
            // place on top of platform
            m->pos[1] = floorHeight;
        }
    }

    // jump kicking: restore action state, otherwise it won't play
    if (m->action == ACT_JUMP_KICK) {
        m->actionState = oldData.actionState;
    }

    // punching:
    if ((m->action == ACT_PUNCHING || m->action == ACT_MOVE_PUNCHING)) {
        // play first punching sound, otherwise it will be missed
        if (m->action != oldData.action) {
            play_character_sound(m, CHAR_SOUND_PUNCH_YAH);
        }
        // make the first punch large, otherwise it will be missed
        if (m->actionArg == 2 && oldData.actionArg == 1) {
            m->marioBodyState->punchState = (0 << 6) | 4;
        }
    }

    // inform of player death
    if (oldData.action != ACT_BUBBLED && data.action == ACT_BUBBLED) {
        construct_player_popup(np, DLANG(NOTIF, DIED), NULL);
    }

    // action changed, reset timer
    if (m->action != oldData.action) {
        m->actionTimer = 0;
    }

    // mark this player as visible
    if (gNetworkAreaLoaded && !m->wasNetworkVisible) {
        m->wasNetworkVisible = true;
        vec3f_copy(m->marioObj->header.gfx.pos, m->pos);
        vec3s_copy(m->marioObj->header.gfx.angle, m->faceAngle);
    }

    // Player's position is valid since it's updated and in the same area as the local player
    np->currPositionValid = true;

    if (np->currLevelNum == LEVEL_BOWSER_3 && m->action == ACT_JUMBO_STAR_CUTSCENE && gMarioStates[0].action != ACT_JUMBO_STAR_CUTSCENE) {
        set_mario_action(&gMarioStates[0], ACT_JUMBO_STAR_CUTSCENE, 0);
    }
    m->marioObj->oActiveParticleFlags = oldData.rawData[0x16];
}

void network_update_player(void) {
    if (!network_player_any_connected()) { return; }
    struct MarioState* m = &gMarioStates[0];

    u8 localIsHeadless = (&gNetworkPlayers[0] == gNetworkPlayerServer && gServerSettings.headlessServer);
    if (localIsHeadless) { return; }

    // figure out if we should send it or not
    static u8 sTicksSinceSend = 0;
    static u32 sLastPlayerAction = 0;
    static u32 sLastPlayerParticles = 0;
    static f32 sLastStickX = 0;
    static f32 sLastStickY = 0;
    static u32 sLastButtonDown = 0;
    static u32 sLastButtonPressed = 0;
    static u32 sLastButtonReleased = 0;

    f32 stickDist = sqrtf(powf(sLastStickX - m->controller->stickX, 2) + powf(sLastStickY - m->controller->stickY, 2));
    bool shouldSend = (sTicksSinceSend > 2)
        || (sLastPlayerAction    != m->action)
        || (sLastButtonDown      != m->controller->buttonDown)
        || (sLastButtonPressed   != m->controller->buttonPressed)
        || (sLastButtonReleased  != m->controller->buttonReleased)
        || (sLastPlayerParticles != m->particleFlags)
        || (stickDist          > 5.0f);

    if (!shouldSend) { sTicksSinceSend++; return; }
    network_send_player(0);
    sTicksSinceSend = 0;

    sLastPlayerAction    = m->action;
    sLastStickX          = m->controller->stickX;
    sLastStickY          = m->controller->stickY;
    sLastButtonDown      = m->controller->buttonDown;
    sLastButtonPressed   = m->controller->buttonPressed;
    sLastButtonReleased  = m->controller->buttonReleased;
    sLastPlayerParticles = m->particleFlags;
}
