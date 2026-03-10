#include "sm64.h"
#include "types.h"
#include "smlua_level_utils.h"
#include "pc/lua/smlua.h"
#include "game/area.h"

#ifdef TARGET_WII_U
#define LEVEL_WIIU_LOG(...)
#else
#define LEVEL_WIIU_LOG(...)
#endif

#define MIN_AREA_INDEX 0
#define MAX_CUSTOM_LEVEL_INFOS 128
#define MAX_CUSTOM_LEVEL_SCRIPT_NAME 128
#define MAX_CUSTOM_LEVEL_FULL_NAME 128
#define MAX_CUSTOM_LEVEL_SHORT_NAME 64

struct CustomLevelInfo* sCustomLevelHead = NULL;
static s16 sCustomLevelNumNext = CUSTOM_LEVEL_NUM_START;
static struct CustomLevelInfo sCustomLevelPool[MAX_CUSTOM_LEVEL_INFOS];
static char sCustomLevelScriptNamePool[MAX_CUSTOM_LEVEL_INFOS][MAX_CUSTOM_LEVEL_SCRIPT_NAME];
static char sCustomLevelFullNamePool[MAX_CUSTOM_LEVEL_INFOS][MAX_CUSTOM_LEVEL_FULL_NAME];
static char sCustomLevelShortNamePool[MAX_CUSTOM_LEVEL_INFOS][MAX_CUSTOM_LEVEL_SHORT_NAME];
static s16 sCustomLevelPoolCount = 0;

void smlua_level_util_reset(void) {
    memset(sCustomLevelPool, 0, sizeof(sCustomLevelPool));
    memset(sCustomLevelScriptNamePool, 0, sizeof(sCustomLevelScriptNamePool));
    memset(sCustomLevelFullNamePool, 0, sizeof(sCustomLevelFullNamePool));
    memset(sCustomLevelShortNamePool, 0, sizeof(sCustomLevelShortNamePool));
    sCustomLevelPoolCount = 0;

    sCustomLevelHead = NULL;
    sCustomLevelNumNext = CUSTOM_LEVEL_NUM_START;
}

void smlua_level_util_change_area(s32 areaIndex) {
    if (areaIndex >= MIN_AREA_INDEX && areaIndex < MAX_AREAS && gAreas[areaIndex].root != NULL) {
        change_area(areaIndex);
    }
}

struct CustomLevelInfo* smlua_level_util_get_info(s16 levelNum) {
    struct CustomLevelInfo* node = sCustomLevelHead;
    while (node != NULL) {
        if (node->levelNum == levelNum) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

struct CustomLevelInfo* smlua_level_util_get_info_from_short_name(const char* shortName) {
    struct CustomLevelInfo* node = sCustomLevelHead;
    while (node != NULL) {
        if (!strcmp(node->shortName, shortName)) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

static struct CustomLevelInfo* smlua_level_util_get_info_from_script(const char* scriptEntryName) {
    struct CustomLevelInfo* node = sCustomLevelHead;
    s32 guard = 0;
    while (node != NULL) {
        if (++guard > 1024) {
            LEVEL_WIIU_LOG("level_register: duplicate_scan_guard_trip");
            return NULL;
        }
        if (!strcmp(node->scriptEntryName, scriptEntryName)) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

static void level_copy_cstr(char* dst, s32 dstCap, const char* src) {
    if (dst == NULL || dstCap <= 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    s32 i = 0;
    while (i < (dstCap - 1) && src[i] != '\0') {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

struct CustomLevelInfo* smlua_level_util_get_info_from_course_num(u8 courseNum) {
    struct CustomLevelInfo* node = sCustomLevelHead;
    while (node != NULL) {
        if (node->courseNum == courseNum) {
            return node;
        }
        node = node->next;
    }
    return NULL;
}

s16 level_register(const char* scriptEntryName, s16 courseNum, const char* fullName, const char* shortName, u32 acousticReach, u32 echoLevel1, u32 echoLevel2, u32 echoLevel3) {
    static s32 sLevelRegisterCallCount = 0;
    s32 callId = ++sLevelRegisterCallCount;
    LEVEL_WIIU_LOG("level_register:%d begin", callId);

    // validate params
    if (scriptEntryName == NULL) {
        LOG_LUA("Provided nil scriptEntryName");
        LEVEL_WIIU_LOG("level_register:%d err nil scriptEntryName", callId);
        return 0;
    }

    if (fullName == NULL) {
        LOG_LUA("Provided nil fullName");
        LEVEL_WIIU_LOG("level_register:%d err nil fullName", callId);
        return 0;
    }

    if (shortName == NULL) {
        LOG_LUA("Provided nil shortName");
        LEVEL_WIIU_LOG("level_register:%d err nil shortName", callId);
        return 0;
    }

    LEVEL_WIIU_LOG("level_register:%d dup_scan", callId);
    // find duplicate
    struct CustomLevelInfo* info = smlua_level_util_get_info_from_script(scriptEntryName);
    if (info != NULL) {
        LEVEL_WIIU_LOG("level_register:%d duplicate", callId);
        return info->levelNum;
    }

    LEVEL_WIIU_LOG("level_register:%d get_script", callId);
    // find script
    LevelScript* script = dynos_get_level_script(scriptEntryName);
    if (script == NULL) {
        LOG_LUA("Failed to find script: %s", scriptEntryName);
        LEVEL_WIIU_LOG("level_register:%d err script null", callId);
        return 0;
    }

    LEVEL_WIIU_LOG("level_register:%d alloc", callId);
    // allocate and fill
    if (sCustomLevelPoolCount < 0 || sCustomLevelPoolCount >= MAX_CUSTOM_LEVEL_INFOS) {
        LEVEL_WIIU_LOG("level_register:%d err pool full", callId);
        LOG_LUA("Too many custom levels registered (%d max): %s", MAX_CUSTOM_LEVEL_INFOS, scriptEntryName);
        return 0;
    }

    info = &sCustomLevelPool[sCustomLevelPoolCount];
    memset(info, 0, sizeof(struct CustomLevelInfo));
    info->script = script;
    level_copy_cstr(sCustomLevelScriptNamePool[sCustomLevelPoolCount], MAX_CUSTOM_LEVEL_SCRIPT_NAME, scriptEntryName);
    info->scriptEntryName = sCustomLevelScriptNamePool[sCustomLevelPoolCount];
    info->courseNum = courseNum;
    info->levelNum = sCustomLevelNumNext++;
    level_copy_cstr(sCustomLevelFullNamePool[sCustomLevelPoolCount], MAX_CUSTOM_LEVEL_FULL_NAME, fullName);
    info->fullName = sCustomLevelFullNamePool[sCustomLevelPoolCount];
    level_copy_cstr(sCustomLevelShortNamePool[sCustomLevelPoolCount], MAX_CUSTOM_LEVEL_SHORT_NAME, shortName);
    info->shortName = sCustomLevelShortNamePool[sCustomLevelPoolCount];
    info->acousticReach = acousticReach;
    info->echoLevel1 = echoLevel1;
    info->echoLevel2 = echoLevel2;
    info->echoLevel3 = echoLevel3;
    LEVEL_WIIU_LOG("level_register:%d mod_index", callId);
    if (gLuaLoadingMod) {
        info->modIndex = gLuaLoadingMod->index;
    } else if (gLuaActiveMod) {
        info->modIndex = gLuaActiveMod->index;
    } else {
        memset(info, 0, sizeof(struct CustomLevelInfo));
        LOG_LUA("Failed to find mod index for level: %s", scriptEntryName);
        LEVEL_WIIU_LOG("level_register:%d err mod index", callId);
        return 0;
    }

    LEVEL_WIIU_LOG("level_register:%d link", callId);
    // add to list
    if (!sCustomLevelHead) {
        sCustomLevelHead = info;
        sCustomLevelPoolCount++;
        LEVEL_WIIU_LOG("level_register:%d done head", callId);
        return info->levelNum;
    }

    struct CustomLevelInfo* node = sCustomLevelHead;
    s32 linkGuard = 0;
    while (node) {
        if (++linkGuard > 1024) {
            LEVEL_WIIU_LOG("level_register:%d err link_guard", callId);
            return info->levelNum;
        }
        if (!node->next) {
            node->next = info;
            sCustomLevelPoolCount++;
            LEVEL_WIIU_LOG("level_register:%d done append", callId);
            return info->levelNum;
        }
        node = node->next;
    }

    // just in case, should never trigger
    return 0;
}

bool level_is_vanilla_level(s16 levelNum) {
    return dynos_level_is_vanilla_level(levelNum);
}

bool warp_to_warpnode(s32 aLevel, s32 aArea, s32 aAct, s32 aWarpId) {
    return dynos_warp_to_warpnode(aLevel, aArea, aAct, aWarpId);
}

bool warp_to_level(s32 aLevel, s32 aArea, s32 aAct) {
    return dynos_warp_to_level(aLevel, aArea, aAct);
}

bool warp_to_start_level(void) {
    return dynos_warp_to_start_level();
}

bool warp_restart_level(void) {
    return dynos_warp_restart_level();
}

bool warp_exit_level(s32 aDelay) {
    return dynos_warp_exit_level(aDelay);
}

bool warp_to_castle(s32 aLevel) {
    return dynos_warp_to_castle(aLevel);
}
