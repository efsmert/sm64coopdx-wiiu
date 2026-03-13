#include <stdio.h>
#include <string.h>
#include "../network.h"
#include "pc/mods/mods.h"
#include "pc/mods/mods_utils.h"
#include "pc/djui/djui.h"
#include "pc/djui/djui_panel_join_message.h"
#include "pc/debuglog.h"
#include "pc/mods/mod_cache.h"
#include "pc/network/coopnet/coopnet.h"
#include "pc/network/coopnet/coopnet_id.h"
#ifdef TARGET_WII_U
#include <coreinit/debug.h>
#include <stdarg.h>
#include <stdio.h>
#define MODLIST_WIIU_LOG_BUFSZ 256
static void modlist_wiiu_logf(const char* fmt, ...) {
    char buffer[MODLIST_WIIU_LOG_BUFSZ];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    OSReport("%s", buffer);
}
#else
static void modlist_wiiu_logf(UNUSED const char* fmt, ...) { }
#endif

static u8 sModListReplyLocalIndex = 0;

static bool modlist_is_arena_mod(const struct Mod* mod) {
    return mod != NULL
        && mod->relativePath[0] != '\0'
        && strcmp(mod->relativePath, "arena") == 0;
}

static bool modlist_has_file(const struct Mod* mod, const char* relativePath) {
    if (mod == NULL || relativePath == NULL) {
        return false;
    }
    for (u16 i = 0; i < mod->fileCount; i++) {
        const struct ModFile* file = &mod->files[i];
        if (strcmp(file->relativePath, relativePath) == 0) {
            return true;
        }
    }
    return false;
}

static bool modlist_sender_valid(u8 localIndex) {
    if (localIndex == UNKNOWN_LOCAL_INDEX) {
        return true;
    }

    if (gNetworkPlayerServer != NULL) {
        return gNetworkPlayerServer->localIndex == localIndex;
    }

#ifdef COOPNET
    uint64_t hostUserId = (uint64_t)coopnet_raw_get_id(0);
    if (hostUserId != 0) {
        u8 hostLocalIndex = coopnet_user_id_to_local_index(hostUserId);
        if (hostLocalIndex != UNKNOWN_LOCAL_INDEX) {
            return hostLocalIndex == localIndex;
        }
    }
#endif

    return false;
}

void network_send_mod_list_request(void) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);
    mods_clear(&gActiveMods);
    mods_clear(&gRemoteMods);

    if (!mods_generate_remote_base_path()) {
        LOG_ERROR("Failed to generate remote base path!");
        return;
    }

    struct Packet p = { 0 };
    packet_init(&p, PACKET_MOD_LIST_REQUEST, true, PLMT_NONE);
    char version[MAX_VERSION_LENGTH] = { 0 };
    snprintf(version, MAX_VERSION_LENGTH, "%s", get_version());
    packet_write_bytes(&p, &version, sizeof(u8) * MAX_VERSION_LENGTH);

    u8 serverLocal = network_get_server_local_index();
    network_send_to(PACKET_DESTINATION_SERVER, &p);
    modlist_wiiu_logf("coopnet-modlist: send request packetType=%u version=%s serverLocal=%d hostUserId=%llu\n",
                      (unsigned)p.buffer[0],
                      version,
                      (int)serverLocal,
                      (unsigned long long)coopnet_raw_get_id(0));
    LOG_INFO("sending mod list request");
    gAllowOrderedPacketClear = 0;
}

void network_receive_mod_list_request(struct Packet* p) {
    if (gNetworkType != NT_SERVER) {
        LOG_ERROR("Network type should be server!");
        return;
    }
    LOG_INFO("received mod list request");

#ifdef COOPNET
    if (p->addr != NULL) {
        uint64_t userId = 0;
        memcpy(&userId, p->addr, sizeof(userId));
        u8 reserved = coopnet_reserve_user_id(userId);
        if (reserved != UNKNOWN_LOCAL_INDEX) {
            p->localIndex = reserved;
        }
    }
#endif

    sModListReplyLocalIndex = (p->localIndex == UNKNOWN_LOCAL_INDEX) ? 0 : p->localIndex;
    network_send_mod_list();
}

void network_send_mod_list(void) {
    SOFT_ASSERT(gNetworkType == NT_SERVER);

    packet_ordered_begin();

    struct Packet p = { 0 };
    packet_init(&p, PACKET_MOD_LIST, true, PLMT_NONE);

    char version[MAX_VERSION_LENGTH] = { 0 };
    snprintf(version, MAX_VERSION_LENGTH, "%s", get_version());
    LOG_INFO("sending version: %s", version);
    packet_write_bytes(&p, &version, sizeof(u8) * MAX_VERSION_LENGTH);
    packet_write(&p, &gActiveMods.entryCount, sizeof(u16));
    network_send_to(sModListReplyLocalIndex, &p);

    LOG_INFO("sent mod list (%u):", gActiveMods.entryCount);
    for (u16 i = 0; i < gActiveMods.entryCount; i++) {
        struct Mod* mod = gActiveMods.entries[i];

        u16 nameLength = strlen(mod->name);
        if (nameLength > MOD_NAME_MAX_LENGTH) { nameLength = MOD_NAME_MAX_LENGTH; }

        u16 incompatibleLength = 0;
        if (mod->incompatible) {
            incompatibleLength = strlen(mod->incompatible);
            if (incompatibleLength > MOD_INCOMPATIBLE_MAX_LENGTH) { incompatibleLength = MOD_INCOMPATIBLE_MAX_LENGTH; }
        }

        u16 relativePathLength = strlen(mod->relativePath);
        u64 modSize = mod->size;

        struct Packet p = { 0 };
        packet_init(&p, PACKET_MOD_LIST_ENTRY, true, PLMT_NONE);
        packet_write(&p, &i, sizeof(u16));
        packet_write(&p, &nameLength, sizeof(u16));
        packet_write_bytes(&p, mod->name, sizeof(u8) * nameLength);
        packet_write(&p, &incompatibleLength, sizeof(u16));
        if (mod->incompatible) {
            packet_write_bytes(&p, mod->incompatible, sizeof(u8) * incompatibleLength);
        } else {
            packet_write(&p, "", 0);
        }
        packet_write(&p, &relativePathLength, sizeof(u16));
        packet_write_bytes(&p, mod->relativePath, sizeof(u8) * relativePathLength);
        packet_write(&p, &modSize, sizeof(u64));
        packet_write(&p, &mod->isDirectory, sizeof(u8));
        packet_write(&p, &mod->pausable, sizeof(u8));
        packet_write(&p, &mod->ignoreScriptWarnings, sizeof(u8));
        packet_write(&p, &mod->fileCount, sizeof(u16));
        network_send_to(sModListReplyLocalIndex, &p);
        LOG_INFO("    '%s': %llu", mod->name, (u64)mod->size);

        for (u16 j = 0; j < mod->fileCount; j++) {
            struct Packet p = { 0 };
            packet_init(&p, PACKET_MOD_LIST_FILE, true, PLMT_NONE);
            struct ModFile* file = &mod->files[j];
            u16 relativePathLength = strlen(file->relativePath);
            u64 fileSize = file->size;
            packet_write(&p, &i, sizeof(u16));
            packet_write(&p, &j, sizeof(u16));
            packet_write(&p, &relativePathLength, sizeof(u16));
            packet_write_bytes(&p, file->relativePath, sizeof(u8) * relativePathLength);
            packet_write(&p, &fileSize, sizeof(u64));
            packet_write_bytes(&p, &file->dataHash[0], sizeof(u8) * 16);
            network_send_to(sModListReplyLocalIndex, &p);
            LOG_INFO("      '%s': %llu", file->relativePath, (u64)file->size);
        }
    }

    struct Packet p2 = { 0 };
    packet_init(&p2, PACKET_MOD_LIST_DONE, true, PLMT_NONE);
    network_send_to(sModListReplyLocalIndex, &p2);

    packet_ordered_end();

}

void network_receive_mod_list(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);
#ifdef COOPNET
    coopnet_mark_client_join_progress("modlist_header");
#endif

    if (!modlist_sender_valid(p->localIndex)) {
        LOG_ERROR("Received mod list from invalid local index '%d'", p->localIndex);
        return;
    }

    if (gRemoteMods.entries != NULL) {
        LOG_INFO("received mod list after allocating");
        return;
    }

    if (gNetworkServerAddr == NULL) {
        gNetworkServerAddr = network_duplicate_address(0);
    }

    char version[MAX_VERSION_LENGTH] = { 0 };
    snprintf(version, MAX_VERSION_LENGTH, "%s", get_version());
    LOG_INFO("client has version: %s", version);

    // verify version
    char remoteVersion[MAX_VERSION_LENGTH] = { 0 };
    packet_read_bytes(p, &remoteVersion, sizeof(u8) * MAX_VERSION_LENGTH);
    LOG_INFO("server has version: %s", version);
    if (memcmp(version, remoteVersion, MAX_VERSION_LENGTH) != 0) {
        network_shutdown(true, false, false, false);
        LOG_ERROR("version mismatch");
        char mismatchMessage[256] = { 0 };
        snprintf(mismatchMessage, 256, "\\#ffa0a0\\Error:\\#dcdcdc\\ Version mismatch.\n\nYour version: \\#a0a0ff\\%s\\#dcdcdc\\\nTheir version: \\#a0a0ff\\%s\\#dcdcdc\\\n\nSomeone is out of date!\n", version, remoteVersion);
        djui_panel_join_message_error(mismatchMessage);
        return;
    }

    packet_read(p, &gRemoteMods.entryCount, sizeof(u16));
    gRemoteMods.entries = calloc(gRemoteMods.entryCount, sizeof(struct Mod*));
    if (gRemoteMods.entries == NULL) {
        LOG_ERROR("Failed to allocate remote mod entries");
        return;
    }

    modlist_wiiu_logf("coopnet-modlist: recv header entries=%u\n", gRemoteMods.entryCount);
    LOG_INFO("received mod list (%u):", gRemoteMods.entryCount);
}

void network_receive_mod_list_entry(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);

    // make sure it was sent by the server
    if (!modlist_sender_valid(p->localIndex)) {
        LOG_ERROR("Received mod list entry from invalid local index '%d'", p->localIndex);
        return;
    }

    // get mod index
    u16 modIndex = 0;
    packet_read(p, &modIndex, sizeof(u16));
    if (modIndex >= gRemoteMods.entryCount) {
        LOG_ERROR("Received mod outside of known range");
        return;
    }

    // allocate mod entry
    gRemoteMods.entries[modIndex] = calloc(1, sizeof(struct Mod));
    struct Mod* mod = gRemoteMods.entries[modIndex];
    if (mod == NULL) {
        LOG_ERROR("Failed to allocate remote mod!");
        return;
    }

    // get name length
    u16 nameLength = 0;
    packet_read(p, &nameLength, sizeof(u16));
    if (nameLength > MOD_NAME_MAX_LENGTH) {
        LOG_ERROR("Received name with invalid length!");
        return;
    }

    // get name
    char name[MOD_NAME_MAX_LENGTH + 1] = { 0 };
    packet_read_bytes(p, name, nameLength * sizeof(u8));
    mod->name = strdup(name);

    // get incompatible length
    u16 incompatibleLength = 0;
    packet_read(p, &incompatibleLength, sizeof(u16));
    if (incompatibleLength > MOD_INCOMPATIBLE_MAX_LENGTH) {
        LOG_ERROR("Received name with invalid length!");
        return;
    }

    // get incompatible
    if (incompatibleLength > 0) {
        char incompatible[MOD_INCOMPATIBLE_MAX_LENGTH + 1] = { 0 };
        packet_read_bytes(p, incompatible, incompatibleLength * sizeof(u8));
        mod->incompatible = strdup(incompatible);
    } else {
        packet_read(p, 0, 0);
    }

    // get other fields
    u16 relativePathLength = 0;
    packet_read(p, &relativePathLength, sizeof(u16));
    if (relativePathLength >= SYS_MAX_PATH) {
        LOG_ERROR("Received mod relative path with invalid length: %u", relativePathLength);
        return;
    }
    packet_read_bytes(p, mod->relativePath, relativePathLength * sizeof(u8));
    if (p->error) {
        LOG_ERROR("Failed to read mod relative path");
        return;
    }
    mod->relativePath[relativePathLength] = '\0';
    u64 remoteModSize = 0;
    packet_read(p, &remoteModSize, sizeof(u64));
    if (remoteModSize > (u64)((size_t)-1)) {
        LOG_ERROR("Received mod size too large for platform: %llu", (unsigned long long)remoteModSize);
        return;
    }
    mod->size = (size_t)remoteModSize;
    packet_read(p, &mod->isDirectory, sizeof(u8));
    packet_read(p, &mod->pausable, sizeof(u8));
    packet_read(p, &mod->ignoreScriptWarnings, sizeof(u8));
    normalize_path(mod->relativePath);
    LOG_INFO("    '%s': %llu", mod->name, (u64)mod->size);

    // figure out base path
    if (mod->isDirectory) {
        if (snprintf(mod->basePath, SYS_MAX_PATH - 1, "%s/%s", gRemoteModsBasePath, mod->relativePath) < 0) {
            LOG_ERROR("Failed save remote base path!");
            return;
        }
        normalize_path(mod->basePath);
    } else {
        if (snprintf(mod->basePath, SYS_MAX_PATH - 1, "%s", gRemoteModsBasePath) < 0) {
            LOG_ERROR("Failed save remote base path!");
            return;
        }
    }

    // sanity check mod size
    if (mod->size >= MAX_MOD_SIZE) {
        djui_popup_create(DLANG(NOTIF, DISCONNECT_BIG_MOD), 4);
        network_shutdown(false, false, false, false);
        return;
    }

    // get file count and allocate them
    packet_read(p, &mod->fileCount, sizeof(u16));
    if (mod->fileCount > 8192) {
        LOG_ERROR("Received mod file count too large: %u", mod->fileCount);
        return;
    }
    mod->files = calloc(mod->fileCount, sizeof(struct ModFile));
    if (mod->files == NULL) {
        LOG_ERROR("Failed to allocate mod files!");
        return;
    }

    if (modlist_is_arena_mod(mod)) {
        modlist_wiiu_logf("coopnet-modlist: arena entry files=%u size=%llu base=%s\n",
                          mod->fileCount,
                          (unsigned long long)mod->size,
                          mod->basePath);
    }
}

void network_receive_mod_list_file(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);

    if (!modlist_sender_valid(p->localIndex)) {
        LOG_ERROR("Received mod list file from invalid local index '%d'", p->localIndex);
        return;
    }

    // get mod index
    u16 modIndex = 0;
    packet_read(p, &modIndex, sizeof(u16));
    if (modIndex >= gRemoteMods.entryCount) {
        LOG_ERROR("Received mod outside of known range");
        return;
    }
    struct Mod* mod = gRemoteMods.entries[modIndex];
    if (mod == NULL) {
        LOG_ERROR("Received mod file for null mod");
        return;
    }

    // get file index
    u16 fileIndex = 0;
    packet_read(p, &fileIndex, sizeof(u16));
    if (fileIndex >= mod->fileCount) {
        LOG_ERROR("Received mod file outside of known range");
        return;
    }
    struct ModFile* file = &mod->files[fileIndex];
    if (mod == NULL) {
        LOG_ERROR("Received null mod file");
        return;
    }

    u16 relativePathLength = 0;
    packet_read(p, &relativePathLength, sizeof(u16));
    if (relativePathLength >= SYS_MAX_PATH) {
        LOG_ERROR("Received mod file path with invalid length: %u", relativePathLength);
        return;
    }
    packet_read_bytes(p, file->relativePath, relativePathLength * sizeof(u8));
    if (p->error) {
        LOG_ERROR("Failed to read mod file path");
        return;
    }
    file->relativePath[relativePathLength] = '\0';
    u64 remoteFileSize = 0;
    packet_read(p, &remoteFileSize, sizeof(u64));
    if (remoteFileSize > (u64)((size_t)-1)) {
        LOG_ERROR("Received mod file size too large for platform: %llu", (unsigned long long)remoteFileSize);
        return;
    }
    file->size = (size_t)remoteFileSize;
    packet_read_bytes(p, &file->dataHash, sizeof(u8) * 16);
    file->fp = NULL;
    LOG_INFO("      '%s': %llu", file->relativePath, (u64)file->size);

    if (modlist_is_arena_mod(mod)) {
        modlist_wiiu_logf("coopnet-modlist: arena file[%u]='%s' size=%llu hash=%02x%02x%02x%02x\n",
                          fileIndex,
                          file->relativePath,
                          (unsigned long long)file->size,
                          file->dataHash[0],
                          file->dataHash[1],
                          file->dataHash[2],
                          file->dataHash[3]);
    }

    struct ModCacheEntry* cache = mod_cache_get_from_hash(file->dataHash);
    if (cache != NULL) {
        LOG_INFO("Found file in cache: %s -> %s", file->relativePath, cache->path);
        if (file->cachedPath != NULL) {
            free((char*)file->cachedPath);
        }
        file->cachedPath = strdup(cache->path);
        normalize_path(file->cachedPath);
    }
}

void network_receive_mod_list_done(struct Packet* p) {
    SOFT_ASSERT(gNetworkType == NT_CLIENT);

    if (!modlist_sender_valid(p->localIndex)) {
        LOG_ERROR("Received mod list done from invalid local index '%d'", p->localIndex);
        return;
    }

    size_t totalSize = 0;
    for (u16 i = 0; i < gRemoteMods.entryCount; i++) {
        struct Mod* mod = gRemoteMods.entries[i];
        totalSize += mod->size;
        if (modlist_is_arena_mod(mod)) {
            modlist_wiiu_logf("coopnet-modlist: arena done hasMain=%d hasFlag=%d hasPlayer=%d hasSpawn=%d fileCount=%u\n",
                              modlist_has_file(mod, "main.lua") ? 1 : 0,
                              modlist_has_file(mod, "arena-flag.lua") ? 1 : 0,
                              modlist_has_file(mod, "arena-player.lua") ? 1 : 0,
                              modlist_has_file(mod, "arena-spawn.lua") ? 1 : 0,
                              mod->fileCount);
        }
    }
    gRemoteMods.size = totalSize;

    modlist_wiiu_logf("coopnet-modlist: recv done totalBytes=%llu\n", (unsigned long long)gRemoteMods.size);
    network_start_download_requests();
}
