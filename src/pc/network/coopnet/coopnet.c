#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include "libcoopnet.h"
#include "coopnet.h"
#include "coopnet_id.h"
#include "pc/network/network.h"
#include "pc/network/version.h"
#include "pc/configfile.h"
#include "pc/djui/djui_language.h"
#include "pc/djui/djui_popup.h"
#include "pc/mods/mods.h"
#include "pc/utils/misc.h"
#include "pc/debuglog.h"
#ifdef TARGET_WII_U
#include <coreinit/debug.h>
#include "pc/wiiu_network.h"
#include <stdarg.h>
#include <stdio.h>
#define COOPNET_WIIU_LOG_BUFSZ 512
static void wiiu_coopnet_logf(const char* fmt, ...) {
    char buffer[COOPNET_WIIU_LOG_BUFSZ];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    OSReport("%s", buffer);
}
#endif
#ifdef DISCORD_SDK
#include "pc/discord/discord.h"
#endif

#ifdef COOPNET

#define MAX_COOPNET_DESCRIPTION_LENGTH 1024

uint64_t gCoopNetDesiredLobby = 0;
char gCoopNetPassword[64] = "";
char sCoopNetDescription[MAX_COOPNET_DESCRIPTION_LENGTH] = "";

static uint64_t sLocalLobbyId = 0;
static uint64_t sLocalLobbyOwnerId = 0;
static enum NetworkType sNetworkType;
static bool sReconnecting = false;
static bool sJoinPending = false;
static uint64_t sJoinPendingLobbyId = 0;
static float sJoinPendingStart = 0.0f;
static float sJoinPendingLastSend = 0.0f;
static uint32_t sJoinPendingAttempts = 0;
static float sJoinPendingLastProgressLog = 0.0f;
static bool sClientModListRequested = false;
static uint64_t sClientModListLobbyId = 0;
static float sClientModListLastSend = 0.0f;
static uint32_t sClientModListAttempts = 0;

// Be conservative on Wii U/Cemu: public lobbies can take a while to answer and
// overly aggressive retries/timeouts can cancel legitimate joins.
#define COOPNET_JOIN_TIMEOUT_SECONDS 30.0f
#define COOPNET_JOIN_RETRY_INTERVAL_SECONDS 8.0f
#define COOPNET_JOIN_MAX_ATTEMPTS 3
#define COOPNET_MODLIST_RETRY_INTERVAL_SECONDS 3.0f
#define COOPNET_MODLIST_MAX_ATTEMPTS 10

static CoopNetRc coopnet_initialize(void);

#define COOPNET_RX_QUEUE_CAPACITY 4096
struct CoopnetRxQueueItem {
    uint64_t userId;
    u8 localIndex;
    u16 dataLength;
    u8 data[PACKET_LENGTH + 1];
};

static struct CoopnetRxQueueItem* sCoopnetRxQueue = NULL;
static volatile u32 sCoopnetRxHead = 0;
static volatile u32 sCoopnetRxTail = 0;
static volatile s32 sCoopnetRxLock = 0;

static void coopnet_rx_lock(void) {
    while (__sync_lock_test_and_set(&sCoopnetRxLock, 1)) { }
}

static void coopnet_rx_unlock(void) {
    __sync_lock_release(&sCoopnetRxLock);
}

static bool coopnet_rx_queue_ensure_allocated(void) {
    if (sCoopnetRxQueue != NULL) {
        return true;
    }

    sCoopnetRxQueue = calloc(COOPNET_RX_QUEUE_CAPACITY, sizeof(struct CoopnetRxQueueItem));
    if (sCoopnetRxQueue == NULL) {
        LOG_ERROR("Failed to allocate CoopNet RX queue");
        return false;
    }

    return true;
}

static void coopnet_rx_queue_clear(void) {
    if (!coopnet_rx_queue_ensure_allocated()) {
        return;
    }
    coopnet_rx_lock();
    sCoopnetRxHead = 0;
    sCoopnetRxTail = 0;
    coopnet_rx_unlock();
}

static bool coopnet_rx_queue_push(uint64_t userId, u8 localIndex, const uint8_t* data, u16 dataLength) {
    if (data == NULL || dataLength == 0 || dataLength > (PACKET_LENGTH + 1)) {
        return false;
    }
    if (!coopnet_rx_queue_ensure_allocated()) {
        return false;
    }

    bool pushed = false;
    coopnet_rx_lock();
    u32 head = sCoopnetRxHead;
    u32 next = (head + 1) % COOPNET_RX_QUEUE_CAPACITY;
    if (next != sCoopnetRxTail) {
        struct CoopnetRxQueueItem* item = &sCoopnetRxQueue[head];
        item->userId = userId;
        item->localIndex = localIndex;
        item->dataLength = dataLength;
        memcpy(item->data, data, dataLength);
        sCoopnetRxHead = next;
        pushed = true;
    }
    coopnet_rx_unlock();

    return pushed;
}

static bool coopnet_rx_queue_pop(struct CoopnetRxQueueItem* outItem) {
    if (outItem == NULL) {
        return false;
    }
    if (sCoopnetRxQueue == NULL) {
        return false;
    }

    bool popped = false;
    coopnet_rx_lock();
    u32 tail = sCoopnetRxTail;
    if (tail != sCoopnetRxHead) {
        memcpy(outItem, &sCoopnetRxQueue[tail], sizeof(struct CoopnetRxQueueItem));
        sCoopnetRxTail = (tail + 1) % COOPNET_RX_QUEUE_CAPACITY;
        popped = true;
    }
    coopnet_rx_unlock();

    return popped;
}

static void coopnet_process_rx_queue(void) {
    struct CoopnetRxQueueItem item;
    while (coopnet_rx_queue_pop(&item)) {
        network_receive(item.localIndex, &item.userId, item.data, item.dataLength);
    }
}

static uint64_t coopnet_generate_dest_id(void) {
    uint64_t seed = ((uint64_t)time(NULL) << 32) ^ (uint64_t)clock_elapsed_ticks();
    seed ^= (uint64_t)(uintptr_t)&seed;
    seed ^= ((uint64_t)(uint32_t)rand() << 16) ^ (uint64_t)(uint32_t)rand();
    if (seed == 0) {
        seed = 1;
    }
    return seed;
}

static void coopnet_clear_client_join_state(void) {
    sJoinPending = false;
    sJoinPendingLobbyId = 0;
    sJoinPendingStart = 0.0f;
    sJoinPendingLastSend = 0.0f;
    sJoinPendingAttempts = 0;
    sJoinPendingLastProgressLog = 0.0f;
    sClientModListRequested = false;
    sClientModListLobbyId = 0;
    sClientModListLastSend = 0.0f;
    sClientModListAttempts = 0;
}

static void coopnet_try_send_mod_list_request(uint64_t lobbyId, const char* reason) {
    if (gNetworkType != NT_CLIENT) { return; }
    if (lobbyId == 0) { return; }

    uint64_t hostUserId = (uint64_t)coopnet_raw_get_id(0);
    if (hostUserId == 0) {
#ifdef TARGET_WII_U
        wiiu_coopnet_logf("coopnet: modlist deferred lobbyId=%llu reason=%s hostUserId=0\n",
                          (unsigned long long)lobbyId, reason ? reason : "unknown");
#endif
        return;
    }

    u8 hostLocalIndex = coopnet_user_id_to_local_index(hostUserId);
    if (hostLocalIndex == UNKNOWN_LOCAL_INDEX) {
#ifdef TARGET_WII_U
        wiiu_coopnet_logf("coopnet: modlist deferred lobbyId=%llu reason=%s hostUserId=%llu localIndex=unknown\n",
                          (unsigned long long)lobbyId,
                          reason ? reason : "unknown",
                          (unsigned long long)hostUserId);
#endif
        return;
    }

    if (sClientModListRequested && sClientModListLobbyId == lobbyId) {
        if (sClientModListAttempts >= COOPNET_MODLIST_MAX_ATTEMPTS) {
            return;
        }
        float now = clock_elapsed();
        if ((now - sClientModListLastSend) < COOPNET_MODLIST_RETRY_INTERVAL_SECONDS) {
            return;
        }
    } else {
        sClientModListAttempts = 0;
    }

    network_send_mod_list_request();
    sClientModListRequested = true;
    sClientModListLobbyId = lobbyId;
    sClientModListLastSend = clock_elapsed();
    sClientModListAttempts++;

    if (sJoinPending && sJoinPendingLobbyId == lobbyId) {
        sJoinPendingStart = clock_elapsed();
        sJoinPendingLastProgressLog = sJoinPendingStart - 2.0f;
    }
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: modlist request lobbyId=%llu reason=%s hostUserId=%llu hostLocal=%u attempt=%u\n",
                      (unsigned long long)lobbyId,
                      reason ? reason : "unknown",
                      (unsigned long long)hostUserId,
                      (unsigned)hostLocalIndex,
                      (unsigned)sClientModListAttempts);
#endif
}

bool ns_coopnet_query(QueryCallbackPtr callback, QueryFinishCallbackPtr finishCallback, const char* password) {
    gCoopNetCallbacks.OnLobbyListGot = callback;
    gCoopNetCallbacks.OnLobbyListFinish = finishCallback;
    if (coopnet_initialize() != COOPNET_OK) { return false; }
    if (coopnet_lobby_list_get(GAME_NAME, password) != COOPNET_OK) { return false; }
    return true;
}

static void coopnet_on_connected(uint64_t userId) {
    coopnet_set_local_user_id(userId);
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: connected userId=%llu\n", (unsigned long long)userId);
#endif
    LOG_INFO("coopnet connected userId=%" PRIu64, userId);
}

static void coopnet_on_disconnected(bool intentional) {
    LOG_INFO("Coopnet shutdown!");
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: disconnected intentional=%d\n", intentional ? 1 : 0);
#endif
    coopnet_clear_client_join_state();
    coopnet_rx_queue_clear();
    if (!intentional) {
        djui_popup_create(DLANG(NOTIF, COOPNET_DISCONNECTED), 2);
    }
    coopnet_shutdown();
    gCoopNetCallbacks.OnLobbyListGot = NULL;
    gCoopNetCallbacks.OnLobbyListFinish = NULL;
}

static void coopnet_on_peer_disconnected(uint64_t peerId) {
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: peer disconnected peerId=%llu\n", (unsigned long long)peerId);
#endif
    LOG_INFO("coopnet peer disconnected peerId=%" PRIu64, peerId);
    u8 localIndex = coopnet_user_id_to_local_index(peerId);
    if (localIndex != UNKNOWN_LOCAL_INDEX && gNetworkPlayers[localIndex].connected) {
        network_player_disconnected(gNetworkPlayers[localIndex].globalIndex);
    }
}

static void coopnet_on_peer_connected(uint64_t peerId) {
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: peer connected peerId=%llu\n", (unsigned long long)peerId);
#endif
    LOG_INFO("coopnet peer connected peerId=%" PRIu64, peerId);
}

static void coopnet_on_load_balance(const char* host, uint32_t port) {
    if (host && strlen(host) > 0) {
        snprintf(configCoopNetIp, MAX_CONFIG_STRING, "%s", host);
    }
    configCoopNetPort = port;
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: load-balance host='%s' port=%u\n", host ? host : "", port);
#endif
    LOG_INFO("coopnet load-balance host='%s' port=%u", host ? host : "", port);
    configfile_save(configfile_name());
}

static void coopnet_on_receive(uint64_t userId, const uint8_t* data, uint64_t dataLength) {
    // Keep slot 0 aligned with the latest active sender, matching upstream
    // CoopDX semantics for "server" routing during pre-join handshakes.
    coopnet_set_user_id(0, userId);
    u8 localIndex = coopnet_user_id_to_local_index(userId);
    if (localIndex == UNKNOWN_LOCAL_INDEX && gNetworkType == NT_SERVER) {
        localIndex = coopnet_reserve_user_id(userId);
#ifdef TARGET_WII_U
        wiiu_coopnet_logf("coopnet: reserved prejoin sender userId=%llu localIndex=%u\n",
                          (unsigned long long)userId,
                          (unsigned)localIndex);
#endif
    }
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: recv fromUser=%llu localIndex=%u hostUser=%llu bytes=%llu\n",
                      (unsigned long long)userId,
                      (unsigned)localIndex,
                      (unsigned long long)coopnet_raw_get_id(0),
                      (unsigned long long)dataLength);
#endif
    if (dataLength > (uint64_t)(PACKET_LENGTH + 1)) {
#ifdef TARGET_WII_U
        wiiu_coopnet_logf("coopnet: drop oversized recv userId=%llu bytes=%llu\n",
                          (unsigned long long)userId, (unsigned long long)dataLength);
#endif
        return;
    }
    if (!coopnet_rx_queue_push(userId, localIndex, data, (u16)dataLength)) {
#ifdef TARGET_WII_U
        wiiu_coopnet_logf("coopnet: rx queue full/drop userId=%llu bytes=%llu\n",
                          (unsigned long long)userId, (unsigned long long)dataLength);
#endif
    }
}

static void coopnet_on_lobby_joined(uint64_t lobbyId, uint64_t userId, uint64_t ownerId, uint64_t destId) {
    LOG_INFO("coopnet_on_lobby_joined!");
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: lobby joined lobbyId=%llu userId=%llu ownerId=%llu destId=%llu\n",
                      (unsigned long long)lobbyId,
                      (unsigned long long)userId,
                      (unsigned long long)ownerId,
                      (unsigned long long)destId);
#endif
    uint64_t localUserId = coopnet_get_local_user_id();
    if (ownerId != localUserId) {
        coopnet_reserve_user_id(ownerId);
    }
    if (userId != localUserId) {
        coopnet_reserve_user_id(userId);
    }
    coopnet_set_user_id(0, ownerId);
    sLocalLobbyId = lobbyId;
    sLocalLobbyOwnerId = ownerId;

    if (userId == localUserId) {
        sJoinPending = false;
        sJoinPendingLobbyId = 0;
        sJoinPendingStart = 0.0f;
        sJoinPendingLastSend = 0.0f;
        sJoinPendingAttempts = 0;
        sJoinPendingLastProgressLog = 0.0f;
        coopnet_clear_dest_ids();
        snprintf(configDestId, MAX_CONFIG_STRING, "%" PRIu64 "", destId);
    } else if (sJoinPending && gNetworkType == NT_CLIENT && lobbyId == sJoinPendingLobbyId) {
        // Some servers send peer snapshots before (or instead of) a self-joined packet.
        // Receiving a lobby snapshot for our target lobby means join has progressed;
        // do not keep a hard timeout armed or we can self-cancel a valid join.
#ifdef TARGET_WII_U
        wiiu_coopnet_logf("coopnet: peer snapshot observed while join pending localUser=%llu eventUser=%llu lobbyId=%llu\n",
                          (unsigned long long)localUserId,
                          (unsigned long long)userId,
                          (unsigned long long)lobbyId);
#endif
        coopnet_try_send_mod_list_request(lobbyId, "peer_snapshot");
        sJoinPending = false;
        sJoinPendingLobbyId = 0;
        sJoinPendingStart = 0.0f;
        sJoinPendingLastSend = 0.0f;
        sJoinPendingAttempts = 0;
        sJoinPendingLastProgressLog = 0.0f;
#ifdef TARGET_WII_U
        wiiu_coopnet_logf("coopnet: join pending cleared via peer snapshot lobbyId=%llu\n",
                          (unsigned long long)lobbyId);
#endif
    }

    coopnet_save_dest_id(userId, destId);

    if (userId == localUserId && gNetworkType == NT_CLIENT) {
        coopnet_try_send_mod_list_request(lobbyId, "self_joined");
    }
#ifdef DISCORD_SDK
    if (gDiscordInitialized) {
        discord_activity_update();
    }
#endif
}

static void coopnet_on_lobby_left(uint64_t lobbyId, uint64_t userId) {
    LOG_INFO("coopnet_on_lobby_left!");
    coopnet_clear_dest_id(userId);
    coopnet_clear_user_id(userId);
    if (lobbyId == sLocalLobbyId && userId == coopnet_get_local_user_id()) {
        network_shutdown(false, false, true, false);
    }
}

void coopnet_mark_client_join_progress(const char* phase) {
    if (gNetworkType != NT_CLIENT || !sJoinPending) {
        return;
    }
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: join progress phase=%s lobbyId=%llu attempts=%u\n",
                      phase ? phase : "unknown",
                      (unsigned long long)sJoinPendingLobbyId,
                      (unsigned)sJoinPendingAttempts);
#endif
    coopnet_clear_client_join_state();
}

static void coopnet_on_error(enum MPacketErrorNumber error, uint64_t tag) {
    const char* errorName = "MERR_UNKNOWN";
    switch (error) {
        case MERR_NONE: errorName = "MERR_NONE"; break;
        case MERR_LOBBY_NOT_FOUND: errorName = "MERR_LOBBY_NOT_FOUND"; break;
        case MERR_LOBBY_JOIN_FULL: errorName = "MERR_LOBBY_JOIN_FULL"; break;
        case MERR_LOBBY_JOIN_FAILED: errorName = "MERR_LOBBY_JOIN_FAILED"; break;
        case MERR_LOBBY_PASSWORD_INCORRECT: errorName = "MERR_LOBBY_PASSWORD_INCORRECT"; break;
        case MERR_COOPNET_VERSION: errorName = "MERR_COOPNET_VERSION"; break;
        case MERR_PEER_FAILED: errorName = "MERR_PEER_FAILED"; break;
        case MERR_MAX: errorName = "MERR_MAX"; break;
    }
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: error=%d(%s) tag=%llu\n", (int)error, errorName, (unsigned long long)tag);
#endif
    coopnet_clear_client_join_state();
    LOG_INFO("coopnet error=%d(%s) tag=%" PRIu64, (int)error, errorName, tag);
    switch (error) {
        case MERR_COOPNET_VERSION:
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: shutdown reason=error_coopnet_version tag=%llu\n",
                              (unsigned long long)tag);
#endif
            djui_popup_create(DLANG(NOTIF, COOPNET_VERSION), 2);
            network_shutdown(false, false, false, false);
            break;
        case MERR_PEER_FAILED:
            {
                char built[256] = { 0 };
                u8 localIndex = coopnet_user_id_to_local_index(tag);
                char* name = DLANG(NOTIF, UNKNOWN);
                if (localIndex == 0) {
                    name = DLANG(NOTIF, LOBBY_HOST);
                } else if (localIndex != UNKNOWN_LOCAL_INDEX && gNetworkPlayers[localIndex].connected) {
                    name = gNetworkPlayers[localIndex].name;
                }
                djui_language_replace(DLANG(NOTIF, PEER_FAILED), built, 256, '@', name);
                djui_popup_create(built, 2);
            }
            break;
        case MERR_LOBBY_NOT_FOUND:
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: shutdown reason=error_lobby_not_found lobbyId=%llu\n",
                              (unsigned long long)tag);
#endif
            djui_popup_create(DLANG(NOTIF, LOBBY_NOT_FOUND), 2);
            network_shutdown(false, false, false, false);
            break;
        case MERR_LOBBY_JOIN_FULL:
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: shutdown reason=error_lobby_full lobbyId=%llu\n",
                              (unsigned long long)tag);
#endif
            djui_popup_create(DLANG(NOTIF, DISCONNECT_FULL), 2);
            network_shutdown(false, false, false, false);
            break;
        case MERR_LOBBY_JOIN_FAILED:
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: shutdown reason=error_lobby_join_failed lobbyId=%llu\n",
                              (unsigned long long)tag);
#endif
            djui_popup_create(DLANG(NOTIF, LOBBY_JOIN_FAILED), 2);
            network_shutdown(false, false, false, false);
            break;
        case MERR_LOBBY_PASSWORD_INCORRECT:
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: shutdown reason=error_lobby_password lobbyId=%llu\n",
                              (unsigned long long)tag);
#endif
            djui_popup_create(DLANG(NOTIF, LOBBY_PASSWORD_INCORRECT), 2);
            network_shutdown(false, false, false, false);
            break;
        case MERR_NONE:
        case MERR_MAX:
            break;
    }
}

static bool ns_coopnet_initialize(enum NetworkType networkType, bool reconnecting) {
    sNetworkType = networkType;
    sReconnecting = reconnecting;
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: initialize networkType=%d reconnecting=%d connected=%d\n",
                      (int)networkType, reconnecting ? 1 : 0, coopnet_is_connected() ? 1 : 0);
#endif
    if (reconnecting) { return true; }
    return coopnet_is_connected()
        ? true
        : (coopnet_initialize() == COOPNET_OK);
}

static char* ns_coopnet_get_id_str(u8 localIndex) {
    static char id_str[32] = { 0 };
    if (localIndex == UNKNOWN_LOCAL_INDEX) {
        snprintf(id_str, 32, "???");
    } else {
        uint64_t userId = ns_coopnet_get_id(localIndex);
        uint64_t destId = coopnet_get_dest_id(userId);
        snprintf(id_str, 32, "%" PRIu64 "", destId);
    }
    return id_str;
}

static bool ns_coopnet_match_addr(void* addr1, void* addr2) {
    return !memcmp(addr1, addr2, sizeof(u64));
}

bool ns_coopnet_is_connected(void) {
    return coopnet_is_connected();
}

static void coopnet_populate_description(void) {
    char* buffer = sCoopNetDescription;
    int bufferLength = MAX_COOPNET_DESCRIPTION_LENGTH;
    // get version
    const char* version = get_version();
    int versionLength = strlen(version);
    snprintf(buffer, bufferLength, "%s", version);
    buffer += versionLength;
    bufferLength -= versionLength;

    // get mod strings
    if (gActiveMods.entryCount <= 0) { return; }
    char* strings[gActiveMods.entryCount];
    for (int i = 0; i < gActiveMods.entryCount; i++) {
        struct Mod* mod = gActiveMods.entries[i];
        strings[i] = mod->name;
    }

    // add seperator
    char* sep = "\n\nMods:\n";
    snprintf(buffer, bufferLength, "%s", sep);
    buffer += strlen(sep);
    bufferLength -= strlen(sep);

    // concat mod strings
    str_seperator_concat(buffer, bufferLength, strings, gActiveMods.entryCount, "\\#dcdcdc\\\n");
}

void ns_coopnet_update(void) {
    if (!coopnet_is_connected()) { return; }

    coopnet_update();
    coopnet_process_rx_queue();
    if (gNetworkType == NT_CLIENT && sJoinPending && sJoinPendingStart > 0.0f) {
        float now = clock_elapsed();
        float elapsed = now - sJoinPendingStart;
        if (now - sJoinPendingLastProgressLog >= 2.0f) {
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: join pending lobbyId=%llu elapsed=%.2f attempts=%u\n",
                              (unsigned long long)sJoinPendingLobbyId,
                              (double)elapsed,
                              (unsigned)sJoinPendingAttempts);
#endif
            sJoinPendingLastProgressLog = now;
        }
        if ((now - sJoinPendingLastSend) >= COOPNET_JOIN_RETRY_INTERVAL_SECONDS
            && sJoinPendingAttempts < COOPNET_JOIN_MAX_ATTEMPTS) {
            CoopNetRc rc = coopnet_lobby_join(sJoinPendingLobbyId, gCoopNetPassword);
            sJoinPendingLastSend = now;
            sJoinPendingAttempts++;
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: lobby_join retry attempt=%u rc=%d lobbyId=%llu\n",
                              (unsigned)sJoinPendingAttempts,
                              (int)rc,
                              (unsigned long long)sJoinPendingLobbyId);
#endif
            if (rc != COOPNET_OK) {
                uint64_t failedLobbyId = sJoinPendingLobbyId;
                coopnet_clear_client_join_state();
#ifdef TARGET_WII_U
                wiiu_coopnet_logf("coopnet: shutdown reason=join_retry_send_failed lobbyId=%llu rc=%d\n",
                                  (unsigned long long)failedLobbyId, (int)rc);
#endif
                djui_popup_create(DLANG(NOTIF, LOBBY_JOIN_FAILED), 2);
                network_shutdown(false, false, false, false);
                return;
            }
        }
        if (elapsed > COOPNET_JOIN_TIMEOUT_SECONDS) {
            uint64_t failedLobbyId = sJoinPendingLobbyId;
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: join timeout lobbyId=%llu elapsed=%.2f\n",
                              (unsigned long long)failedLobbyId, (double)elapsed);
#endif
            LOG_INFO("coopnet join timeout lobbyId=%" PRIu64 " elapsed=%.2f", failedLobbyId, elapsed);
            coopnet_clear_client_join_state();
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: shutdown reason=join_timeout lobbyId=%llu\n",
                              (unsigned long long)failedLobbyId);
#endif
            djui_popup_create(DLANG(NOTIF, LOBBY_JOIN_FAILED), 2);
            network_shutdown(false, false, false, false);
            return;
        }
    }
    if (gNetworkType == NT_CLIENT && sLocalLobbyId != 0 && gRemoteMods.entries == NULL) {
        if (!sClientModListRequested) {
            coopnet_try_send_mod_list_request(sLocalLobbyId, "post_join_poll");
        } else if (sClientModListLobbyId == sLocalLobbyId) {
            coopnet_try_send_mod_list_request(sLocalLobbyId, "retry_timer");
        }
    }

    if (gNetworkType != NT_NONE && sNetworkType != NT_NONE) {
        if (sNetworkType == NT_SERVER) {
            char mode[64] = "";
            mods_get_main_mod_name(mode, 64);
            if (sReconnecting) {
                LOG_INFO("Update lobby");
                coopnet_populate_description();
                CoopNetRc rc = coopnet_lobby_update(sLocalLobbyId, GAME_NAME, get_version(), configPlayerName, mode, sCoopNetDescription);
#ifdef TARGET_WII_U
                wiiu_coopnet_logf("coopnet: lobby_update rc=%d lobbyId=%llu mode='%s'\n", rc, (unsigned long long)sLocalLobbyId, mode);
#endif
            } else {
                LOG_INFO("Create lobby");
                snprintf(gCoopNetPassword, 64, "%s", configPassword);
                coopnet_populate_description();
                CoopNetRc rc = coopnet_lobby_create(GAME_NAME, get_version(), configPlayerName, mode, (uint16_t)configAmountOfPlayers, gCoopNetPassword, sCoopNetDescription);
#ifdef TARGET_WII_U
                wiiu_coopnet_logf("coopnet: lobby_create rc=%d mode='%s' max=%d pwlen=%u\n",
                                  rc, mode, (int)configAmountOfPlayers, (unsigned)strlen(gCoopNetPassword));
#endif
            }
        } else if (sNetworkType == NT_CLIENT) {
            LOG_INFO("Join lobby");
            coopnet_clear_client_join_state();
            CoopNetRc rc = coopnet_lobby_join(gCoopNetDesiredLobby, gCoopNetPassword);
#ifdef TARGET_WII_U
            wiiu_coopnet_logf("coopnet: lobby_join rc=%d lobbyId=%llu pwlen=%u\n",
                              rc, (unsigned long long)gCoopNetDesiredLobby, (unsigned)strlen(gCoopNetPassword));
#endif
            if (rc == COOPNET_OK) {
                sJoinPending = true;
                sJoinPendingLobbyId = gCoopNetDesiredLobby;
                sJoinPendingStart = clock_elapsed();
                sJoinPendingLastSend = sJoinPendingStart;
                sJoinPendingAttempts = 1;
                sJoinPendingLastProgressLog = sJoinPendingStart - 2.0f;
            } else {
                coopnet_clear_client_join_state();
#ifdef TARGET_WII_U
                wiiu_coopnet_logf("coopnet: join request rejected before pending rc=%d lobbyId=%llu\n",
                                  (int)rc, (unsigned long long)gCoopNetDesiredLobby);
#endif
            }
        }
        sNetworkType = NT_NONE;
    }
}

static int ns_coopnet_network_send(u8 localIndex, void* address, u8* data, u16 dataLength) {
    if (!coopnet_is_connected()) { return 1; }
    //if (gCurLobbyId == 0) { return 2; }
    u64 userId = coopnet_raw_get_id(localIndex);
    if (localIndex == 0 && address != NULL) { userId = *(u64*)address; }
    coopnet_send_to(userId, data, dataLength);

    return 0;
}

static bool coopnet_allow_invite(void) {
    if (sLocalLobbyId == 0) { return false; }
    return (sLocalLobbyOwnerId == coopnet_get_local_user_id()) || (strlen(gCoopNetPassword) == 0);
}

static void ns_coopnet_get_lobby_id(UNUSED char* destination, UNUSED u32 destLength) {
    if (sLocalLobbyId == 0) {
        snprintf(destination, destLength, "%s", "");
    } else {
        snprintf(destination, destLength, "coopnet:%" PRIu64 "", sLocalLobbyId);
    }
}

static void ns_coopnet_get_lobby_secret(UNUSED char* destination, UNUSED u32 destLength) {
    if (sLocalLobbyId == 0 || !coopnet_allow_invite()) {
        snprintf(destination, destLength, "%s", "");
    } else {
        snprintf(destination, destLength, "coopnet:%" PRIu64":%s", sLocalLobbyId, gCoopNetPassword);
    }
}

static void ns_coopnet_shutdown(bool reconnecting) {
    if (reconnecting) { return; }
    LOG_INFO("Coopnet shutdown!");
    coopnet_clear_client_join_state();
    coopnet_shutdown();
    gCoopNetCallbacks.OnLobbyListGot = NULL;
    gCoopNetCallbacks.OnLobbyListFinish = NULL;

    gCoopNetCallbacks.OnConnected = NULL;
    gCoopNetCallbacks.OnDisconnected = NULL;
    gCoopNetCallbacks.OnReceive = NULL;
    gCoopNetCallbacks.OnLobbyJoined = NULL;
    gCoopNetCallbacks.OnLobbyLeft = NULL;
    gCoopNetCallbacks.OnError = NULL;
    gCoopNetCallbacks.OnPeerConnected = NULL;
    gCoopNetCallbacks.OnPeerDisconnected = NULL;
    gCoopNetCallbacks.OnLoadBalance = NULL;

    sLocalLobbyId = 0;
    sLocalLobbyOwnerId = 0;

    coopnet_rx_lock();
    free(sCoopnetRxQueue);
    sCoopnetRxQueue = NULL;
    sCoopnetRxHead = 0;
    sCoopnetRxTail = 0;
    coopnet_rx_unlock();
}

static CoopNetRc coopnet_initialize(void) {
    if (!coopnet_rx_queue_ensure_allocated()) {
        return COOPNET_FAILED;
    }
    gCoopNetCallbacks.OnConnected = coopnet_on_connected;
    gCoopNetCallbacks.OnDisconnected = coopnet_on_disconnected;
    gCoopNetCallbacks.OnReceive = coopnet_on_receive;
    gCoopNetCallbacks.OnLobbyJoined = coopnet_on_lobby_joined;
    gCoopNetCallbacks.OnLobbyLeft = coopnet_on_lobby_left;
    gCoopNetCallbacks.OnError = coopnet_on_error;
    gCoopNetCallbacks.OnPeerConnected = coopnet_on_peer_connected;
    gCoopNetCallbacks.OnPeerDisconnected = coopnet_on_peer_disconnected;
    gCoopNetCallbacks.OnLoadBalance = coopnet_on_load_balance;

    if (coopnet_is_connected()) { return COOPNET_OK; }

#ifdef TARGET_WII_U
    if (!wiiu_network_is_online()) {
        wiiu_coopnet_logf("coopnet: AC offline before begin, retrying wiiu_network_init\n");
        wiiu_network_init();
    }
#endif

    if (configCoopNetIp[0] == '\0') {
        snprintf(configCoopNetIp, MAX_CONFIG_STRING, "%s", DEFAULT_COOPNET_IP);
    }
    if (configCoopNetPort == 0 || configCoopNetPort > 65535) {
        configCoopNetPort = DEFAULT_COOPNET_PORT;
    }
    if (configDestId[0] == '\0') {
        snprintf(configDestId, MAX_CONFIG_STRING, "%s", "0");
    }

    char* endptr = NULL;
    uint64_t destId = strtoull(configDestId, &endptr, 10);
    if (configDestId[0] == '\0' || endptr == configDestId || (endptr && *endptr != '\0') || destId == 0) {
        destId = coopnet_generate_dest_id();
        snprintf(configDestId, MAX_CONFIG_STRING, "%" PRIu64, destId);
        configfile_save(configfile_name());
#ifdef TARGET_WII_U
        wiiu_coopnet_logf("coopnet: generated persistent destId=%s\n", configDestId);
#endif
    }

#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: begin host='%s' port=%u player='%s' dest='%s'\n",
                      configCoopNetIp, configCoopNetPort, configPlayerName, configDestId);
#endif
    printf("coopnet: begin host='%s' port=%u player='%s' dest='%s'\n",
           configCoopNetIp, configCoopNetPort, configPlayerName, configDestId);
    LOG_INFO("coopnet begin host='%s' port=%u player='%s' dest='%s'",
             configCoopNetIp, configCoopNetPort, configPlayerName, configDestId);
    CoopNetRc rc = coopnet_begin(configCoopNetIp, configCoopNetPort, configPlayerName, destId);
#ifdef TARGET_WII_U
    wiiu_coopnet_logf("coopnet: begin rc=%d\n", rc);
#endif
    printf("coopnet: begin rc=%d\n", rc);
    LOG_INFO("coopnet begin rc=%d", rc);
    if (rc == COOPNET_FAILED) {
        djui_popup_create(DLANG(NOTIF, COOPNET_CONNECTION_FAILED), 2);
    }
    return rc;
}

struct NetworkSystem gNetworkSystemCoopNet = {
    .initialize       = ns_coopnet_initialize,
    .get_id           = ns_coopnet_get_id,
    .get_id_str       = ns_coopnet_get_id_str,
    .save_id          = ns_coopnet_save_id,
    .clear_id         = ns_coopnet_clear_id,
    .dup_addr         = ns_coopnet_dup_addr,
    .match_addr       = ns_coopnet_match_addr,
    .update           = ns_coopnet_update,
    .send             = ns_coopnet_network_send,
    .get_lobby_id     = ns_coopnet_get_lobby_id,
    .get_lobby_secret = ns_coopnet_get_lobby_secret,
    .shutdown         = ns_coopnet_shutdown,
    .requireServerBroadcast = false,
    .name             = "CoopNet",
};

#endif
