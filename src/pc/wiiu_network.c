#include "wiiu_network.h"

#ifdef TARGET_WII_U

#include <coreinit/debug.h>
#include <nn/ac.h>
#include <nn/result.h>
#include <nsysnet/_socket.h>
#include <whb/log.h>

static bool sSocketLibReady = false;
static bool sAcReady = false;
static bool sNetworkBootstrapped = false;

static void wiiu_network_log_result(const char *label, NNResult result) {
    WHBLogPrintf("network: %s rc=%d", label, result.value);
    OSReport("network: %s rc=%d\n", label, result.value);
}

void wiiu_network_init(void) {
    if (sNetworkBootstrapped) {
        return;
    }

    socket_lib_init();
    sSocketLibReady = true;
    WHBLogPrint("network: socket_lib_init done");
    OSReport("network: socket_lib_init done\n");

    NNResult rc = ACInitialize();
    if (NNResult_IsFailure(rc)) {
        wiiu_network_log_result("ACInitialize failed", rc);
        sNetworkBootstrapped = true;
        return;
    }
    sAcReady = true;
    wiiu_network_log_result("ACInitialize ok", rc);

    BOOL connected = FALSE;
    rc = ACIsApplicationConnected(&connected);
    if (NNResult_IsFailure(rc)) {
        wiiu_network_log_result("ACIsApplicationConnected failed", rc);
        sNetworkBootstrapped = true;
        return;
    }

    if (!connected) {
        rc = ACConnect();
        if (NNResult_IsFailure(rc)) {
            wiiu_network_log_result("ACConnect failed", rc);
        } else {
            wiiu_network_log_result("ACConnect ok", rc);
            rc = ACIsApplicationConnected(&connected);
            if (NNResult_IsFailure(rc)) {
                wiiu_network_log_result("ACIsApplicationConnected post-connect failed", rc);
            }
        }
    }

    WHBLogPrintf("network: bootstrap complete connected=%d", connected ? 1 : 0);
    OSReport("network: bootstrap complete connected=%d\n", connected ? 1 : 0);
    sNetworkBootstrapped = true;
}

void wiiu_network_shutdown(void) {
    if (!sNetworkBootstrapped) {
        return;
    }

    if (sAcReady) {
        (void)ACClose();
        ACFinalize();
        sAcReady = false;
        WHBLogPrint("network: ACFinalize done");
        OSReport("network: ACFinalize done\n");
    }

    if (sSocketLibReady) {
        socket_lib_finish();
        sSocketLibReady = false;
        WHBLogPrint("network: socket_lib_finish done");
        OSReport("network: socket_lib_finish done\n");
    }

    sNetworkBootstrapped = false;
}

bool wiiu_network_is_online(void) {
    if (!sAcReady) {
        return false;
    }

    BOOL connected = FALSE;
    NNResult rc = ACIsApplicationConnected(&connected);
    if (NNResult_IsFailure(rc)) {
        return false;
    }
    return connected;
}

#else

void wiiu_network_init(void) {
}

void wiiu_network_shutdown(void) {
}

bool wiiu_network_is_online(void) {
    return true;
}

#endif
