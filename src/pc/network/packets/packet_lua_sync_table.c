#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include "../network.h"
#include "pc/lua/smlua.h"
#include "pc/lua/smlua_utils.h"
#include "pc/mods/mod.h"
#include "pc/mods/mods.h"
#include "pc/network/coopnet/coopnet_id.h"
#include "pc/debuglog.h"
#ifdef TARGET_WII_U
#include <coreinit/debug.h>
#include <stdarg.h>
static void lua_sync_wiiu_logf(const char* fmt, ...) {
    (void)fmt;
}
#define LUA_SYNC_TRACE(...)
#else
static void lua_sync_wiiu_logf(UNUSED const char* fmt, ...) { }
#define LUA_SYNC_TRACE(...)
#endif

/////////////////////////////////////////////////////////////

static bool network_lua_sync_trace_should_log(const char* path) {
    return path != NULL
        && (strcmp(path, "gGlobalSyncTable.round_state") == 0
            || strcmp(path, "gGlobalSyncTable.timer") == 0
            || strcmp(path, "gGlobalSyncTable.padding") == 0
            || strcmp(path, "gGlobalSyncTable.level") == 0);
}

static bool network_lua_sync_trace_should_log_mod0(u16 modIndex) {
    return false;
}

static bool network_lua_sync_trace_is_flood_mod(u16 modIndex, const char** outModName) {
    *outModName = "<invalid>";
    if (modIndex >= gActiveMods.entryCount) {
        return false;
    }
    return false;
}

static void network_lua_sync_trace_build_path(char* buffer, size_t bufferSize, u16 lntKeyCount, struct LSTNetworkType* lntKeys) {
    size_t used = 0;
    buffer[0] = '\0';
    for (s32 i = lntKeyCount - 1; i >= 0; i--) {
        const char* part = smlua_lnt_to_str(&lntKeys[i]);
        int written = snprintf(buffer + used, bufferSize - used, "%s%s", used == 0 ? "" : ".", part);
        if (written < 0 || (size_t)written >= bufferSize - used) {
            buffer[bufferSize - 1] = '\0';
            return;
        }
        used += (size_t)written;
    }
}

void network_send_lua_sync_table_request(void) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);
    struct Packet p = { 0 };
    packet_init(&p, PACKET_LUA_SYNC_TABLE_REQUEST, true, PLMT_NONE);
    u8 serverLocalIndex = (gNetworkPlayerServer != NULL) ? gNetworkPlayerServer->localIndex : 0;
    LUA_SYNC_TRACE("join-trace: lua_sync_request serverLocal=%u hostSlot0=%llu hostSlot1=%llu\n",
                   (unsigned)serverLocalIndex,
                   (unsigned long long)coopnet_raw_get_id(0),
                   (unsigned long long)coopnet_raw_get_id(1));
    network_send_to(serverLocalIndex, &p);
    LOG_INFO("sending lua sync table request");
}

void network_receive_lua_sync_table_request(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_SERVER);
    SOFT_ASSERT(p->localIndex < MAX_PLAYERS);
    smlua_sync_table_send_all(p->localIndex);
    LOG_INFO("received lua sync table request");
}

void network_send_lua_sync_table(u8 toLocalIndex, u64 seq, u16 modRemoteIndex, u16 lntKeyCount, struct LSTNetworkType* lntKeys, struct LSTNetworkType* lntValue) {
    if (gLuaState == NULL) { return; }
    if (lntKeyCount >= MAX_UNWOUND_LNT) { LOG_ERROR("Tried to send too many lnt keys"); return; }

    struct Packet p = { 0 };
    packet_init(&p, PACKET_LUA_SYNC_TABLE, true, PLMT_NONE);
    packet_write(&p, &seq, sizeof(u64));
    packet_write(&p, &modRemoteIndex, sizeof(u16));
    packet_write(&p, &lntKeyCount, sizeof(u16));

    //LOG_INFO("TX SYNC (%llu):", seq);
    for (s32 i = 0; i < lntKeyCount; i++) {
        if (!packet_write_lnt(&p, &lntKeys[i])) { return; }
        //LOG_INFO("  %s", smlua_lnt_to_str(&lntKeys[i]));
    }
    //LOG_INFO("    -> %s", smlua_lnt_to_str(lntValue));
    //LOG_INFO("  count %u", lntKeyCount);

    if (!packet_write_lnt(&p, lntValue)) { return; }

    const char* modName = NULL;
    if (network_lua_sync_trace_is_flood_mod(modRemoteIndex, &modName)) {
        char pathBuf[192] = { 0 };
        network_lua_sync_trace_build_path(pathBuf, sizeof(pathBuf), lntKeyCount, lntKeys);
        if (network_lua_sync_trace_should_log(pathBuf)) {
            LOG_INFO("flood-trace: sync tx toLocal=%u mod=%u(%s) seq=%llu path=%s value=%s",
                     (unsigned)toLocalIndex,
                     modRemoteIndex,
                     modName,
                     (unsigned long long)seq,
                     pathBuf,
                     smlua_lnt_to_str(lntValue));
        }
    }

    if (toLocalIndex == 0 || toLocalIndex >= MAX_PLAYERS) {
        network_send(&p);
    } else {
        network_send_to(toLocalIndex, &p);
    }
}

void network_receive_lua_sync_table(struct Packet* p) {
    LUA_SYNC_TRACE("lua-sync-enter: local=%u gLuaState=%p dataLength=%u\n",
                   (unsigned)p->localIndex,
                   gLuaState,
                   (unsigned)p->dataLength);
    if (gLuaState == NULL) { return; }

    u64 seq = 0;
    u16 modRemoteIndex = 0;
    u16 lntKeyCount = 0;
    struct LSTNetworkType lntKeys[MAX_UNWOUND_LNT] = { 0 };
    struct LSTNetworkType lntValue = { 0 };

    packet_read(p, &seq, sizeof(u64));
    packet_read(p, &modRemoteIndex, sizeof(u16));
    packet_read(p, &lntKeyCount, sizeof(u16));
    LUA_SYNC_TRACE("lua-sync-header: local=%u mod=%u keyCount=%u seqLo=%u\n",
                   (unsigned)p->localIndex,
                   (unsigned)modRemoteIndex,
                   (unsigned)lntKeyCount,
                   (unsigned)(seq & 0xffffffffu));
    if (lntKeyCount >= MAX_UNWOUND_LNT) { LOG_ERROR("Tried to receive too many lnt keys"); return; }
    LOG_INFO("lua-sync: rx packet local=%u mod=%u keyCount=%u", (unsigned)p->localIndex, modRemoteIndex, lntKeyCount);

    //LOG_INFO("RX SYNC (%llu):", seq);
    for (s32 i = 0; i < lntKeyCount; i++) {
        if (!packet_read_lnt(p, &lntKeys[i])) { goto cleanup; }
        //LOG_INFO("  %s", smlua_lnt_to_str(&lntKeys[i]));
    }
    //LOG_INFO("    -> %s", smlua_lnt_to_str(&lntValue));
    //LOG_INFO("  count %u", lntKeyCount);

    if (!packet_read_lnt(p, &lntValue)) { goto cleanup; }

    if (p->error) { LOG_ERROR("Packet read error"); return; }
    if (network_lua_sync_trace_should_log_mod0(modRemoteIndex)) {
        char pathBuf[192] = { 0 };
        network_lua_sync_trace_build_path(pathBuf, sizeof(pathBuf), lntKeyCount, lntKeys);
        LUA_SYNC_TRACE("lua-sync-path: mod=%u local=%u seqLo=%u path=%s value=%s\n",
                       (unsigned)modRemoteIndex,
                       (unsigned)p->localIndex,
                       (unsigned)(seq & 0xffffffffu),
                       pathBuf,
                       smlua_lnt_to_str(&lntValue));
    }
    const char* modName = NULL;
    if (network_lua_sync_trace_is_flood_mod(modRemoteIndex, &modName)) {
        char pathBuf[192] = { 0 };
        network_lua_sync_trace_build_path(pathBuf, sizeof(pathBuf), lntKeyCount, lntKeys);
        if (network_lua_sync_trace_should_log(pathBuf)) {
            LOG_INFO("flood-trace: sync rx mod=%u(%s) local=%u seq=%llu path=%s value=%s",
                     modRemoteIndex, modName, p->localIndex, seq, pathBuf, smlua_lnt_to_str(&lntValue));
        }
    }
    smlua_set_sync_table_field_from_network(seq, modRemoteIndex, lntKeyCount, lntKeys, &lntValue);

cleanup:
    for (s32 i = 0; i < lntKeyCount; i++) {
        if (lntKeys[i].type != LST_NETWORK_TYPE_STRING) { continue; }
        if (lntKeys[i].value.string == NULL) { continue; }
        free(lntKeys[i].value.string);
        lntKeys[i].value.string = NULL;
    }
    if (lntValue.type == LST_NETWORK_TYPE_STRING && lntValue.value.string != NULL) {
        free(lntValue.value.string);
        lntValue.value.string = NULL;
    }
}
