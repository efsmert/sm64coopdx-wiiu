#include <stdio.h>
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
    char buffer[384];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    OSReport("%s", buffer);
}
#else
static void lua_sync_wiiu_logf(UNUSED const char* fmt, ...) { }
#endif

/////////////////////////////////////////////////////////////

static bool network_lua_sync_trace_is_flood_mod(u16 modIndex, const char** outModName) {
    *outModName = "<invalid>";
    if (modIndex >= gActiveMods.entryCount) { return false; }
    struct Mod* mod = gActiveMods.entries[modIndex];
    if (mod == NULL) { return false; }

    if (mod->name != NULL && mod->name[0] != '\0') {
        *outModName = mod->name;
    } else if (mod->relativePath[0] != '\0') {
        *outModName = mod->relativePath;
    }

    return (mod->name != NULL && strstr(mod->name, "Flood") != NULL) ||
           (strstr(mod->relativePath, "flood") != NULL);
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
    lua_sync_wiiu_logf("lua-sync: tx request serverLocal=%u serverGlobal=%d hostUserId=%llu\n",
                       (unsigned)network_get_server_local_index(),
                       (gNetworkPlayerServer != NULL) ? (int)gNetworkPlayerServer->globalIndex : -1,
                       (unsigned long long)coopnet_raw_get_id(network_get_server_local_index()));
    network_send_to(network_get_server_local_index(), &p);
    LOG_INFO("sending lua sync table request");
}

void network_receive_lua_sync_table_request(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_SERVER);
    SOFT_ASSERT(p->localIndex < MAX_PLAYERS);
    lua_sync_wiiu_logf("lua-sync: rx request local=%u global=%d connected=%d\n",
                       (unsigned)p->localIndex,
                       (p->localIndex < MAX_PLAYERS) ? (int)gNetworkPlayers[p->localIndex].globalIndex : -1,
                       (p->localIndex < MAX_PLAYERS && gNetworkPlayers[p->localIndex].connected) ? 1 : 0);
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
        lua_sync_wiiu_logf("lua-sync: tx field toLocal=%u mod=%u(%s) seq=%llu path=%s value=%s\n",
                           (unsigned)toLocalIndex,
                           modRemoteIndex,
                           modName,
                           (unsigned long long)seq,
                           pathBuf,
                           smlua_lnt_to_str(lntValue));
    }

    if (toLocalIndex == 0 || toLocalIndex >= MAX_PLAYERS) {
        network_send(&p);
    } else {
        network_send_to(toLocalIndex, &p);
    }
}

void network_receive_lua_sync_table(struct Packet* p) {
    if (gLuaState == NULL) { return; }

    u64 seq = 0;
    u16 modRemoteIndex = 0;
    u16 lntKeyCount = 0;
    struct LSTNetworkType lntKeys[MAX_UNWOUND_LNT] = { 0 };
    struct LSTNetworkType lntValue = { 0 };

    packet_read(p, &seq, sizeof(u64));
    packet_read(p, &modRemoteIndex, sizeof(u16));
    packet_read(p, &lntKeyCount, sizeof(u16));
    if (lntKeyCount >= MAX_UNWOUND_LNT) { LOG_ERROR("Tried to receive too many lnt keys"); return; }

    //LOG_INFO("RX SYNC (%llu):", seq);
    for (s32 i = 0; i < lntKeyCount; i++) {
        if (!packet_read_lnt(p, &lntKeys[i])) { goto cleanup; }
        //LOG_INFO("  %s", smlua_lnt_to_str(&lntKeys[i]));
    }
    //LOG_INFO("    -> %s", smlua_lnt_to_str(&lntValue));
    //LOG_INFO("  count %u", lntKeyCount);

    if (!packet_read_lnt(p, &lntValue)) { goto cleanup; }

    if (p->error) { LOG_ERROR("Packet read error"); return; }
    const char* modName = NULL;
    if (network_lua_sync_trace_is_flood_mod(modRemoteIndex, &modName)) {
        char pathBuf[192] = { 0 };
        network_lua_sync_trace_build_path(pathBuf, sizeof(pathBuf), lntKeyCount, lntKeys);
        LOG_INFO("flood-trace: sync rx mod=%u(%s) local=%u seq=%llu path=%s value=%s", modRemoteIndex, modName, p->localIndex, seq, pathBuf, smlua_lnt_to_str(&lntValue));
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
