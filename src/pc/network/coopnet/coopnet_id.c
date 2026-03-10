#include "libcoopnet.h"
#include "coopnet.h"
#include "pc/network/network.h"
#include "pc/debuglog.h"

static uint64_t sLocalUserId = 0;
static uint64_t sNetworkUserIds[MAX_PLAYERS] = { 0 };

#define MAX_DEST_IDS (MAX_PLAYERS * 2)
struct DestinationId {
    uint64_t userId;
    uint64_t destId;
};
struct DestinationId sDestinationIds[MAX_DEST_IDS] = { 0 };

void coopnet_save_dest_id(uint64_t userId, uint64_t destId) {
    struct DestinationId* dest = NULL;
    for (int i = 0; i < MAX_DEST_IDS; i++) {
        if (sDestinationIds[i].userId == userId) {
            sDestinationIds[i].destId = destId;
            return;
        } else if (dest == NULL && sDestinationIds[i].userId == 0) {
            dest = &sDestinationIds[i];
        }
    }
    if (dest) {
        dest->userId = userId;
        dest->destId = destId;
    }
}

void coopnet_clear_dest_id(uint64_t userId) {
    for (int i = 0; i < MAX_DEST_IDS; i++) {
        if (sDestinationIds[i].userId == userId) {
            sDestinationIds[i].userId = 0;
            sDestinationIds[i].destId = 0;
        }
    }
}

void coopnet_clear_dest_ids(void) {
    for (int i = 0; i < MAX_DEST_IDS; i++) {
        sDestinationIds[i].userId = 0;
        sDestinationIds[i].destId = 0;
    }
}

uint64_t coopnet_get_dest_id(uint64_t userId) {
    for (int i = 0; i < MAX_DEST_IDS; i++) {
        if (sDestinationIds[i].userId == userId) {
            return sDestinationIds[i].destId;
        }
    }
    return 0;
}

u8 coopnet_user_id_to_local_index(uint64_t userId) {
    if (userId == 0) { return UNKNOWN_LOCAL_INDEX; }

    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (sNetworkUserIds[i] == userId) {
            return i;
        }
    }
    return UNKNOWN_LOCAL_INDEX;
}

u8 coopnet_reserve_user_id(uint64_t userId) {
    if (userId == 0) { return UNKNOWN_LOCAL_INDEX; }

    u8 localIndex = coopnet_user_id_to_local_index(userId);
    if (localIndex != UNKNOWN_LOCAL_INDEX) {
        return localIndex;
    }

    // First prefer an unused disconnected slot.
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (sNetworkUserIds[i] == 0 && !gNetworkPlayers[i].connected) {
            sNetworkUserIds[i] = userId;
            return (u8)i;
        }
    }

    // If no empty slots exist, reclaim any disconnected provisional slot.
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (!gNetworkPlayers[i].connected) {
            sNetworkUserIds[i] = userId;
            return (u8)i;
        }
    }

    return UNKNOWN_LOCAL_INDEX;
}

void coopnet_clear_user_id(uint64_t userId) {
    if (userId == 0) { return; }

    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (sNetworkUserIds[i] == userId) {
            sNetworkUserIds[i] = 0;
        }
    }
}

void coopnet_set_user_id(uint8_t localIndex, uint64_t userId) {
    sNetworkUserIds[localIndex] = userId;
}

uint64_t coopnet_get_local_user_id(void) {
    return sLocalUserId;
}

void coopnet_set_local_user_id(uint64_t userId) {
    sLocalUserId = userId;
}

s64 coopnet_raw_get_id(u8 localIndex) {
    return (s64)sNetworkUserIds[localIndex];
}

s64 ns_coopnet_get_id(u8 localIndex) {
    if (localIndex == 0) { return (s64)sLocalUserId; }
    return (s64)sNetworkUserIds[localIndex];
}

void ns_coopnet_save_id(u8 localIndex, s64 networkId) {
    SOFT_ASSERT(localIndex > 0);
    SOFT_ASSERT(localIndex < MAX_PLAYERS);
    if (networkId != 0) {
        sNetworkUserIds[localIndex] = (u64)networkId;
        return;
    }

    if (sNetworkUserIds[localIndex] != 0) {
        // Keep any provisional mapping established before full join.
        return;
    }

    // Fallback for older flows that only know the lobby owner at this point.
    sNetworkUserIds[localIndex] = sNetworkUserIds[0];
}

void ns_coopnet_clear_id(u8 localIndex) {
    if (localIndex == 0) { return; }
    SOFT_ASSERT(localIndex < MAX_PLAYERS);
    sNetworkUserIds[localIndex] = 0;
}

void* ns_coopnet_dup_addr(u8 localIndex) {
    void* address = malloc(sizeof(u64));
    memcpy(address, &sNetworkUserIds[localIndex], sizeof(u64));
    return address;
}
