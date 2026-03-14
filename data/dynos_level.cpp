#include "dynos.cpp.h"
extern "C" {
#include "game/segment2.h"
#include "game/save_file.h"
#include "levels/scripts.h"
#include "pc/lua/utils/smlua_level_utils.h"
#include "game/area.h"
#include "game/level_update.h"
#ifdef TARGET_WII_U
#include <whb/log.h>
#endif
}

//
// Const
//

extern "C" {
extern const BehaviorScript *sWarpBhvSpawnTable[];
#include "engine/level_script.h"
}

#define DYNOS_LEVEL_MOD_INDEX_VANILLA (-1)

#define DYNOS_LEVEL_MARIO_POS_WARP_ID (-1)

extern void *gDynosLevelScriptsOriginal[LEVEL_COUNT];

void DynOS_Level_ParseScript(const void *aScript, s32 (*aPreprocessFunction)(u8, void *));

//
// Data
//

struct DynosWarp {
/* 0 */ s16 mArea = 0;
/* 1 */ s16 mId = 0;
/* 2 */ s16 mType = -1;
/* 3 */ s16 mPosX = 0;
/* 4 */ s16 mPosY = 0;
/* 5 */ s16 mPosZ = 0;
/* 6 */ s16 mAngle = 0;
/* 7 */ s16 mDestLevel = 0;
/* 8 */ s16 mDestArea = 0;
/* 9 */ s16 mDestId = 0;
};

struct DynosLevelScript {
    void *mLevelScript;
    s32 mModIndex;
};

static DynosLevelScript sDynosLevelScripts[LEVEL_COUNT] = { { NULL, DYNOS_LEVEL_MOD_INDEX_VANILLA } };
static Array<DynosWarp> sDynosLevelWarps[LEVEL_COUNT] = { Array<DynosWarp>() };
static Collision *sDynosLevelCollision[LEVEL_COUNT][MAX_AREAS] = { NULL };
static s32 sDynosCustomLevelSlot[LEVEL_UNKNOWN_2 + 1] = { 0 };
static bool sDynosSwapCmdScalarFields = false; // endian-fix for custom (little-endian) scripts on big-endian targets

u64 DynOS_Level_CmdGet(void *aCmd, u64 aOffset) {
    u64 _Offset = (((aOffset) & 3llu) | (((aOffset) & ~3llu) << (sizeof(void *) >> 3llu)));
    uintptr_t value = 0;
    memcpy(&value, (void *) ((uintptr_t) aCmd + _Offset), sizeof(value));
    return (u64) value;
}

template <typename T>
static T DynOS_Level_CmdRead(void *aCmd, u64 aOffset, bool swapScalar = true, bool forceSwap = false) {
    T value = (T) 0;
    u64 _Offset = (((aOffset) & 3llu) | (((aOffset) & ~3llu) << (sizeof(void *) >> 3llu)));
    memcpy(&value, (void *) ((uintptr_t) aCmd + _Offset), sizeof(value));
    if (swapScalar && (forceSwap || sDynosSwapCmdScalarFields)) {
        // DynOS custom level scripts are produced on little-endian hosts. When
        // parsing them on a big-endian target (Wii U), swap multi-byte scalar
        // fields (s16/u16/s32/u32/f32/etc).
        value = DynOS_EndianFix<T>::Read(value);
    }
    return value;
}

extern "C" u8 DynOS_Level_CmdReadU8(void *aCmd, u64 aOffset) {
    return DynOS_Level_CmdRead<u8>(aCmd, aOffset);
}

extern "C" s16 DynOS_Level_CmdReadS16(void *aCmd, u64 aOffset) {
    return DynOS_Level_CmdRead<s16>(aCmd, aOffset, true, true);
}

extern "C" u16 DynOS_Level_CmdReadU16(void *aCmd, u64 aOffset) {
    return DynOS_Level_CmdRead<u16>(aCmd, aOffset, true, true);
}

extern "C" s32 DynOS_Level_CmdReadS32(void *aCmd, u64 aOffset) {
    return DynOS_Level_CmdRead<s32>(aCmd, aOffset, true, true);
}

extern "C" u32 DynOS_Level_CmdReadU32(void *aCmd, u64 aOffset) {
    return DynOS_Level_CmdRead<u32>(aCmd, aOffset, true, true);
}

extern "C" uintptr_t DynOS_Level_CmdReadPointer(void *aCmd, u64 aOffset) {
    return DynOS_Level_CmdRead<uintptr_t>(aCmd, aOffset, false);
}

LvlCmd *DynOS_Level_CmdNext(LvlCmd *aCmd) {
    u64 aCmdSize = aCmd->mSize;
    u64 _Offset = (((aCmdSize) & 3llu) | (((aCmdSize) & ~3llu) << (sizeof(void *) >> 3llu)));
    return (LvlCmd*) (u64(aCmd) + _Offset);
}

//
// Init
//

static s32 sDynosCurrentLevelNum = 0;
static u8 sDynosAreaIndex = 0;

inline static DynosWarp *DynOS_Level_GetWarpStruct(s8 aId) {
    for (s32 i = 0; i != sDynosLevelWarps[sDynosCurrentLevelNum].Count(); ++i) {
        if (sDynosLevelWarps[sDynosCurrentLevelNum][i].mArea == sDynosAreaIndex &&
            sDynosLevelWarps[sDynosCurrentLevelNum][i].mId == aId) {
            return &sDynosLevelWarps[sDynosCurrentLevelNum][i];
        }
    }
    DynosWarp _Warp;
    _Warp.mArea = sDynosAreaIndex;
    _Warp.mId = aId;
    sDynosLevelWarps[sDynosCurrentLevelNum].Add(_Warp);
    return &sDynosLevelWarps[sDynosCurrentLevelNum][sDynosLevelWarps[sDynosCurrentLevelNum].Count() - 1];
};

static s32 DynOS_Level_PreprocessScript(u8 aType, void *aCmd) {
    switch (aType) {

        // AREA
        case 0x1F: {
            sDynosAreaIndex = DynOS_Level_CmdRead<u8>(aCmd, 2);
        } break;

        // OBJECT / OBJECT_WITH_ACTS_EXT / OBJECT_WITH_ACTS_EXT2
        case 0x24:
        case 0x3F:
        case 0x40: {
            // 0x24 and 0x3F store behavior pointer at +20; 0x40 stores it at +24.
            const u64 bhvOffset = (aType == 0x40) ? 24 : 20;
            // Behavior pointers are native addresses loaded from DynOS token decoding;
            // do not endian-fix pointer fields while parsing custom scripts.
            const BehaviorScript *bhv = (const BehaviorScript *) DynOS_Level_CmdRead<uintptr_t>(aCmd, bhvOffset, false);
            for (s32 i = 0; i < 20; ++i) {
                if (sWarpBhvSpawnTable[i] == bhv) {
                    DynosWarp *_Warp = DynOS_Level_GetWarpStruct((DynOS_Level_CmdRead<u32>(aCmd, 16) >> 16) & 0xFF);
                    if (_Warp->mType == -1) {
                        _Warp->mType = i;
                        _Warp->mPosX = DynOS_Level_CmdRead<s16>(aCmd, 4);
                        _Warp->mPosY = DynOS_Level_CmdRead<s16>(aCmd, 6);
                        _Warp->mPosZ = DynOS_Level_CmdRead<s16>(aCmd, 8);
                        _Warp->mAngle = (DynOS_Level_CmdRead<s16>(aCmd, 12) * 0x8000) / 180;
                    }
                    break;
                }
            }
        } break;

        // WARP_NODE or PAINTING_WARP_NODE
        case 0x26:
        case 0x27: {
            DynosWarp *_Warp = DynOS_Level_GetWarpStruct(DynOS_Level_CmdRead<u8>(aCmd, 2));
            if (_Warp->mDestLevel == 0) {
                _Warp->mDestLevel = DynOS_Level_CmdRead<u8>(aCmd, 3);
                _Warp->mDestArea = DynOS_Level_CmdRead<u8>(aCmd, 4);
                _Warp->mDestId = DynOS_Level_CmdRead<u8>(aCmd, 5);
            }
        } break;

        // MARIO_POS
        case 0x2B: {
            DynosWarp *_Warp = DynOS_Level_GetWarpStruct(DYNOS_LEVEL_MARIO_POS_WARP_ID);
            // The area index is a u8 field at +2; reading it as s16 breaks on big-endian.
            _Warp->mArea = (s16) DynOS_Level_CmdRead<u8>(aCmd, 2);
            _Warp->mAngle = (DynOS_Level_CmdRead<s16>(aCmd, 4) * 0x8000) / 180 - 0x8000;
            _Warp->mPosX = DynOS_Level_CmdRead<s16>(aCmd, 6);
            _Warp->mPosY = DynOS_Level_CmdRead<s16>(aCmd, 8);
            _Warp->mPosZ = DynOS_Level_CmdRead<s16>(aCmd, 10);
            _Warp->mType = MARIO_SPAWN_IDLE - 1;
            _Warp->mDestLevel = sDynosCurrentLevelNum;
            _Warp->mDestArea = _Warp->mArea;
            _Warp->mDestId = DYNOS_LEVEL_MARIO_POS_WARP_ID;
        } break;

        // SLEEP or SLEEP_BEFORE_EXIT
        case 0x03:
        case 0x04:
            return 3;

        // TERRAIN
        case 0x2E: {
            sDynosLevelCollision[sDynosCurrentLevelNum][sDynosAreaIndex] =
                (Collision*) DynOS_Level_CmdRead<uintptr_t>(aCmd, 4, false);
        } break;
    }

    return 0;
}

// Runs only once
void DynOS_Level_Init() {
    static bool sInited = false;
    if (!sInited) {

        // Level warps
        sDynosSwapCmdScalarFields = false;
        for (sDynosCurrentLevelNum = 0; sDynosCurrentLevelNum < LEVEL_COUNT; ++sDynosCurrentLevelNum) {
            sDynosLevelScripts[sDynosCurrentLevelNum].mLevelScript = gDynosLevelScriptsOriginal[sDynosCurrentLevelNum];
            sDynosLevelScripts[sDynosCurrentLevelNum].mModIndex = DYNOS_LEVEL_MOD_INDEX_VANILLA;
            if (sDynosLevelScripts[sDynosCurrentLevelNum].mLevelScript) {
                DynOS_Level_ParseScript(sDynosLevelScripts[sDynosCurrentLevelNum].mLevelScript, DynOS_Level_PreprocessScript);
            }
        }

        // Done
        sInited = true;
    }
}

//
// Common
//

s8 DynOS_Level_GetCourse(s32 aLevel) {
    return get_level_course_num(aLevel);
}

void DynOS_Level_Override(void* originalScript, void* newScript, s32 modIndex) {
    for (s32 i = 0; i < LEVEL_COUNT; i++) {
        if (sDynosLevelScripts[i].mLevelScript == originalScript) {
            sDynosCurrentLevelNum = i;
            sDynosLevelWarps[i].Clear();
            const bool prevSwap = sDynosSwapCmdScalarFields;
            sDynosSwapCmdScalarFields = (modIndex != DYNOS_LEVEL_MOD_INDEX_VANILLA);
            DynOS_Level_ParseScript(newScript, DynOS_Level_PreprocessScript);
            sDynosSwapCmdScalarFields = prevSwap;
            sDynosLevelScripts[i].mLevelScript = newScript;
            sDynosLevelScripts[i].mModIndex = modIndex;
            return;
        }
    }
}

void DynOS_Level_Unoverride() {
    // clear custom level slots
    sDynosCustomLevelSlot[LEVEL_UNKNOWN_1] = 0;
    sDynosCustomLevelSlot[LEVEL_UNKNOWN_2] = 0;

    for (s32 i = 0; i < LEVEL_COUNT; i++) {
        sDynosCurrentLevelNum = i;
        sDynosLevelWarps[i].Clear();
        sDynosLevelScripts[i].mLevelScript = gDynosLevelScriptsOriginal[i];
        sDynosLevelScripts[i].mModIndex = DYNOS_LEVEL_MOD_INDEX_VANILLA;
        sDynosSwapCmdScalarFields = false;
        DynOS_Level_ParseScript(sDynosLevelScripts[i].mLevelScript, DynOS_Level_PreprocessScript);
    }
}

const void *DynOS_Level_GetScript(s32 aLevel) {
    if (aLevel >= CUSTOM_LEVEL_NUM_START) {
        struct CustomLevelInfo* info = smlua_level_util_get_info(aLevel);
        if (!info || !info->script) { return NULL; }
        return info->script;
    }

    DynOS_Level_Init();
    return sDynosLevelScripts[aLevel].mLevelScript;
}

s32 DynOS_Level_GetModIndex(s32 aLevel) {
    if (aLevel >= CUSTOM_LEVEL_NUM_START) {
        struct CustomLevelInfo* info = smlua_level_util_get_info(aLevel);
        if (!info || !info->script) { return DYNOS_LEVEL_MOD_INDEX_VANILLA; }
        return info->modIndex;
    }

    DynOS_Level_Init();
    return sDynosLevelScripts[aLevel].mModIndex;
}

bool DynOS_Level_IsVanillaLevel(s32 aLevel) {
    DynOS_Level_Init();

    if (aLevel >= LEVEL_MIN && aLevel < LEVEL_COUNT) {
        return sDynosLevelScripts[aLevel].mLevelScript == gDynosLevelScriptsOriginal[aLevel];
    }
    return false;
}

Collision *DynOS_Level_GetCollision(u32 aLevel, u16 aArea) {
    if (aLevel >= LEVEL_COUNT) return NULL;
    if (aArea >= MAX_AREAS) return NULL;
    DynOS_Level_Init();
    return sDynosLevelCollision[aLevel][aArea];
}

//
// Level Script Preprocessing
// - Ifs are always true
// - Skips are always false
// - Loops break after the first loop
//

struct Stack {
    u64 mData[32];
    s32 mBaseIndex;
    s32 mTopIndex;
};

template <typename T>
static void StackPush(Stack& aStack, const T &aValue) {
    if (aStack.mTopIndex >= 0) {
        aStack.mData[aStack.mTopIndex] = u64(aValue);
        aStack.mTopIndex++;
    }
}

template <typename T>
static T StackPop(Stack& aStack) {
    if (aStack.mTopIndex <= 0) {
        return (T) 0;
    }
    aStack.mTopIndex--;
    return (T) aStack.mData[aStack.mTopIndex];
}

static LvlCmd *DynOS_Level_CmdExecute(Stack &aStack, LvlCmd *aCmd) {
    StackPush(aStack, DynOS_Level_CmdNext(aCmd));
    StackPush(aStack, aStack.mBaseIndex);
    aStack.mBaseIndex = aStack.mTopIndex;
    return (LvlCmd *) DynOS_Level_CmdRead<uintptr_t>(aCmd, 12, false);
}

static LvlCmd *DynOS_Level_CmdExitAndExecute(Stack &aStack, LvlCmd *aCmd) {
    aStack.mTopIndex = aStack.mBaseIndex;
    return (LvlCmd *) DynOS_Level_CmdRead<uintptr_t>(aCmd, 12, false);
}

static LvlCmd *DynOS_Level_CmdExit(Stack &aStack, LvlCmd *aCmd) {
    aStack.mTopIndex = aStack.mBaseIndex;
    aStack.mBaseIndex = StackPop<s32>(aStack);
    return StackPop<LvlCmd *>(aStack);
}

static LvlCmd *DynOS_Level_CmdJump(Stack &aStack, LvlCmd *aCmd) {
    return (LvlCmd *) DynOS_Level_CmdRead<uintptr_t>(aCmd, 4, false);
}

static LvlCmd *DynOS_Level_CmdJumpLink(Stack &aStack, LvlCmd *aCmd) {
    StackPush(aStack, DynOS_Level_CmdNext(aCmd));
    return (LvlCmd *) DynOS_Level_CmdRead<uintptr_t>(aCmd, 4, false);
}

static LvlCmd *DynOS_Level_CmdReturn(Stack &aStack, UNUSED LvlCmd *aCmd) {
    return StackPop<LvlCmd *>(aStack);
}

static LvlCmd *DynOS_Level_CmdJumpLinkPushArg(Stack &aStack, LvlCmd *aCmd) {
    StackPush(aStack, DynOS_Level_CmdNext(aCmd));
    StackPush(aStack, DynOS_Level_CmdRead<s16>(aCmd, 2));
    return DynOS_Level_CmdNext(aCmd);
}

static LvlCmd *DynOS_Level_CmdJumpRepeat(Stack &aStack, LvlCmd *aCmd) {
    aStack.mTopIndex -= 2;
    return DynOS_Level_CmdNext(aCmd);
}

static LvlCmd *DynOS_Level_CmdLoopBegin(Stack &aStack, LvlCmd *aCmd) {
    StackPush(aStack, DynOS_Level_CmdNext(aCmd));
    StackPush(aStack, 0);
    return DynOS_Level_CmdNext(aCmd);
}

static LvlCmd *DynOS_Level_CmdLoopUntil(Stack &aStack, LvlCmd *aCmd) {
    aStack.mTopIndex -= 2;
    return DynOS_Level_CmdNext(aCmd);
}

static LvlCmd *DynOS_Level_CmdJumpIf(Stack &aStack, LvlCmd *aCmd) {
    StackPush(aStack, DynOS_Level_CmdNext(aCmd)); /* Not an error, that's intentional */
    return (LvlCmd *) DynOS_Level_CmdRead<uintptr_t>(aCmd, 8, false);
}

static LvlCmd *DynOS_Level_CmdJumpLinkIf(Stack &aStack, LvlCmd *aCmd) {
    StackPush(aStack, DynOS_Level_CmdNext(aCmd));
    return (LvlCmd *) DynOS_Level_CmdRead<uintptr_t>(aCmd, 8, false);
}

static LvlCmd *DynOS_Level_CmdJumpArea(Stack &aStack, LvlCmd *aCmd, s32 (*aPreprocessFunction)(u8, void *)) {
    DynOS_Level_ParseScript((const void *) DynOS_Level_CmdRead<uintptr_t>(aCmd, 8, false), aPreprocessFunction);
    return DynOS_Level_CmdNext(aCmd);
}

void DynOS_Level_ParseScript(const void *aScript, s32 (*aPreprocessFunction)(u8, void *)) {
    Stack _Stack;
    _Stack.mBaseIndex = -1;
    _Stack.mTopIndex = 0;
    const u32 kMaxParseSteps = 200000;
    u32 parseSteps = 0;
    LvlCmd *prevCmd = NULL;
    u32 sameCmdCount = 0;
    for (LvlCmd *_Cmd = (LvlCmd *) aScript; _Cmd != NULL;) {
        if (++parseSteps > kMaxParseSteps) {
#ifdef TARGET_WII_U
            static u32 sParseStepLimitLogCount = 0;
            if (sParseStepLimitLogCount < 32) {
                WHBLogPrintf("dynos: parse abort step_limit script=%p cmd=%p steps=%u", aScript, _Cmd, parseSteps);
                sParseStepLimitLogCount++;
            }
#else
            PrintError("dynos: parse abort step_limit script=%p cmd=%p steps=%u", aScript, _Cmd, parseSteps);
#endif
            return;
        }
        if (_Cmd == prevCmd) {
            if (++sameCmdCount > 1024) {
#ifdef TARGET_WII_U
                static u32 sParseStallLogCount = 0;
                if (sParseStallLogCount < 32) {
                    WHBLogPrintf("dynos: parse abort stalled script=%p cmd=%p type=%u size=%u",
                                 aScript, _Cmd, (u32)(_Cmd->mType & 0xFF), (u32)_Cmd->mSize);
                    sParseStallLogCount++;
                }
#else
                PrintError("dynos: parse abort stalled script=%p cmd=%p type=%u size=%u",
                           aScript, _Cmd, (u32)(_Cmd->mType & 0xFF), (u32)_Cmd->mSize);
#endif
                return;
            }
        } else {
            prevCmd = _Cmd;
            sameCmdCount = 0;
        }
        if (_Cmd->mSize == 0) {
#ifdef TARGET_WII_U
            static u32 sParseZeroSizeLogCount = 0;
            if (sParseZeroSizeLogCount < 32) {
                WHBLogPrintf("dynos: parse abort zero_size script=%p cmd=%p type=%u",
                             aScript, _Cmd, (u32)(_Cmd->mType & 0xFF));
                sParseZeroSizeLogCount++;
            }
#else
            PrintError("dynos: parse abort zero_size script=%p cmd=%p type=%u",
                       aScript, _Cmd, (u32)(_Cmd->mType & 0xFF));
#endif
            return;
        }
        u8 _CmdType = (_Cmd->mType & 0xFF);
        s32 _Action = aPreprocessFunction(_CmdType, (void *) _Cmd);
        switch (_Action) {
            case 0:
                switch (_CmdType) {
                    case 0x00: _Cmd = DynOS_Level_CmdExecute(_Stack, _Cmd);         break;
                    case 0x01: _Cmd = DynOS_Level_CmdExitAndExecute(_Stack, _Cmd);  break;
                    case 0x02: _Cmd = DynOS_Level_CmdExit(_Stack, _Cmd);            break;
                    case 0x05: _Cmd = DynOS_Level_CmdJump(_Stack, _Cmd);            break;
                    case 0x06: _Cmd = DynOS_Level_CmdJumpLink(_Stack, _Cmd);        break;
                    case 0x07: _Cmd = DynOS_Level_CmdReturn(_Stack, _Cmd);          break;
                    case 0x08: _Cmd = DynOS_Level_CmdJumpLinkPushArg(_Stack, _Cmd); break;
                    case 0x09: _Cmd = DynOS_Level_CmdJumpRepeat(_Stack, _Cmd);      break;
                    case 0x0A: _Cmd = DynOS_Level_CmdLoopBegin(_Stack, _Cmd);       break;
                    case 0x0B: _Cmd = DynOS_Level_CmdLoopUntil(_Stack, _Cmd);       break;
                    case 0x0C: _Cmd = DynOS_Level_CmdJumpIf(_Stack, _Cmd);          break;
                    case 0x0D: _Cmd = DynOS_Level_CmdJumpLinkIf(_Stack, _Cmd);      break;

                    // coop
                    case 0x42: _Cmd = DynOS_Level_CmdJumpArea(_Stack, _Cmd, aPreprocessFunction); break;

                    default: _Cmd = DynOS_Level_CmdNext(_Cmd); break;
                } break;

            case 1:
                _Cmd = DynOS_Level_CmdNext(_Cmd);
                break;

            case 2:
                _Cmd = DynOS_Level_CmdReturn(_Stack, _Cmd);
                break;

            case 3:
                return;
        }
    }
}

//
// Level Script Utilities
//

s16 *DynOS_Level_GetWarp(s32 aLevel, s32 aArea, s8 aWarpId) {
    if (aLevel >= CUSTOM_LEVEL_NUM_START) {
        struct CustomLevelInfo* info = smlua_level_util_get_info(aLevel);
        if (!info || !info->script) { return NULL; }
        LevelScript *cachedScript = info->script;
        LevelScript *resolvedScript = NULL;
        if (info->scriptEntryName != NULL) {
            resolvedScript = DynOS_Lvl_GetScript(info->scriptEntryName);
            if (resolvedScript != NULL) {
                info->script = resolvedScript;
            }
        }
        LevelScript *scriptToParse = (resolvedScript != NULL) ? resolvedScript : info->script;

        // This requires some explaination...
        // It's a bit of a hack but it works.

        // DynOS's arrays for level information are LEVEL_MAX in size.
        // LEVEL_MAX is based on the max number of vanilla levels.

        // So when we want to warp to a custom level we load the
        // data into a slot that vanilla left unused.

        // We need two unused slots because we may warp from one
        // custom level to another.

        // Check if we're warping to a level we've already loaded into a slot
        if (sDynosCustomLevelSlot[LEVEL_UNKNOWN_1] == aLevel) {
            sDynosCurrentLevelNum = LEVEL_UNKNOWN_1;
        } else if (sDynosCustomLevelSlot[LEVEL_UNKNOWN_2] == aLevel) {
            sDynosCurrentLevelNum = LEVEL_UNKNOWN_2;
        } else {
            // Pick the unused slot
            s32 unusedSlot = (sDynosCustomLevelSlot[LEVEL_UNKNOWN_1] == gCurrLevelNum) ? LEVEL_UNKNOWN_2 : LEVEL_UNKNOWN_1;

            // Assign it to dynos
            sDynosCurrentLevelNum = unusedSlot;

            // Remember that the custom level is loaded into the slot
            sDynosCustomLevelSlot[sDynosCurrentLevelNum] = aLevel;

            // Clear cached level warps from the slot to be loaded
            sDynosLevelWarps[sDynosCurrentLevelNum].Clear();

            // Parse the custom level to fill in the level warps.
            // Some mods can hand out stale script pointers after reloads, so prefer a
            // fresh lookup by entry name and fall back to the cached pointer.
            const bool prevSwap = sDynosSwapCmdScalarFields;
            // Custom DynOS scripts are authored in little-endian layout.
            // On Wii U (big-endian), scalar fields must be endian-fixed while
            // parsing warps/MARIO_POS so spawn points stay valid.
            sDynosSwapCmdScalarFields = true;
            if (scriptToParse != NULL) {
                DynOS_Level_ParseScript(scriptToParse, DynOS_Level_PreprocessScript);
            }
            if (sDynosLevelWarps[sDynosCurrentLevelNum].Count() == 0 &&
                cachedScript != NULL && cachedScript != scriptToParse) {
                DynOS_Level_ParseScript(cachedScript, DynOS_Level_PreprocessScript);
            }
            sDynosSwapCmdScalarFields = prevSwap;
        }

        // find the custom level warp
        for (const auto &_Warp : sDynosLevelWarps[sDynosCurrentLevelNum]) {
            if (_Warp.mArea == aArea && _Warp.mId == aWarpId) {
                return (s16 *) &_Warp;
            }
        }
        return NULL;
    }

    DynOS_Level_Init();
    if (aLevel >= 0 && aLevel < LEVEL_COUNT) {
        for (const auto &_Warp : sDynosLevelWarps[aLevel]) {
            if (_Warp.mArea == aArea && _Warp.mId == aWarpId) {
                return (s16 *) &_Warp;
            }
        }
    }
    return NULL;
}

s16 *DynOS_Level_GetWarpEntry(s32 aLevel, s32 aArea) {
    DynOS_Level_Init();

    // Custom levels in Lua mods do not always provide a 0x0A entry warp.
    // Prefer the safest known entry between 0x0A and MARIO_POS, then any
    // area-matching warp as fallback.
    if (aLevel >= CUSTOM_LEVEL_NUM_START) {
        s16 *warpNode = DynOS_Level_GetWarp(aLevel, aArea, 0x0A);
        s16 *marioPos = DynOS_Level_GetWarp(aLevel, aArea, DYNOS_LEVEL_MARIO_POS_WARP_ID);

        // Some custom scripts define WARP_NODE(0x0A, ...) but map it to a deep
        // transition spawn (or leave it partially undefined). In those cases,
        // MARIO_POS is usually the intended safe player spawn.
        if (warpNode && warpNode[2] == -1) {
            warpNode = NULL;
        }
        if (warpNode && marioPos) {
            const bool warpNodeLooksUnsafe = (warpNode[4] <= -10000);
            if (warpNodeLooksUnsafe) {
                return marioPos;
            }
        }

        if (warpNode) {
            return warpNode;
        }
        if (marioPos) {
            return marioPos;
        }

        for (s32 i = 0; i != sDynosLevelWarps[sDynosCurrentLevelNum].Count(); ++i) {
            DynosWarp *candidate = &sDynosLevelWarps[sDynosCurrentLevelNum][i];
            if (candidate->mArea == aArea) {
                // Prefer warps with a known spawn type to avoid invalid spawns.
                if (candidate->mType != -1) return (s16 *) candidate;
            }
        }
        for (s32 i = 0; i != sDynosLevelWarps[sDynosCurrentLevelNum].Count(); ++i) {
            DynosWarp *candidate = &sDynosLevelWarps[sDynosCurrentLevelNum][i];
            if (candidate->mArea == aArea) {
                return (s16 *) candidate;
            }
        }
        if (sDynosLevelWarps[sDynosCurrentLevelNum].Count() > 0) {
            return (s16 *) &sDynosLevelWarps[sDynosCurrentLevelNum][0];
        }
        return NULL;
    }

    // override vanilla castle warps
    if (DynOS_Level_GetCourse(aLevel) == COURSE_NONE && aLevel >= 0 && aLevel < LEVEL_COUNT) {
        extern const LevelScript level_castle_grounds_entry[];
        extern const LevelScript level_castle_inside_entry[];
        extern const LevelScript level_castle_courtyard_entry[];
        if (sDynosLevelScripts[aLevel].mLevelScript == level_castle_inside_entry) {
            return DynOS_Level_GetWarp(aLevel, aArea, (aArea == 3) ? 0x00 : 0x01);
        } else if (sDynosLevelScripts[aLevel].mLevelScript == level_castle_grounds_entry) {
            return DynOS_Level_GetWarp(aLevel, aArea, DYNOS_LEVEL_MARIO_POS_WARP_ID);
        } else if (sDynosLevelScripts[aLevel].mLevelScript == level_castle_courtyard_entry) {
            return DynOS_Level_GetWarp(aLevel, aArea, 0x01);
        }
    }

    // Co-op DX expects a 0x0A entry warp for most levels, but some overridden scripts (Lua mods)
    // and some COURSE_NONE levels may only provide MARIO_POS. Fall back gracefully.
    s16 *_Warp = DynOS_Level_GetWarp(aLevel, aArea, 0x0A);
    if (_Warp && _Warp[2] == -1) {
        _Warp = NULL;
    }
    if (!_Warp) _Warp = DynOS_Level_GetWarp(aLevel, aArea, DYNOS_LEVEL_MARIO_POS_WARP_ID);
    if (_Warp) return _Warp;

    // As a last resort, pick any warp entry that matches the requested area, preferring ones with
    // a known spawn type (mType != -1) to avoid invalid spawn behavior.
    if (aLevel >= 0 && aLevel < LEVEL_COUNT) {
        DynosWarp *anyArea = NULL;
        for (s32 i = 0; i != sDynosLevelWarps[aLevel].Count(); ++i) {
            DynosWarp *candidate = &sDynosLevelWarps[aLevel][i];
            if (candidate->mArea != aArea) continue;
            if (candidate->mType != -1) return (s16 *) candidate;
            if (!anyArea) anyArea = candidate;
        }
        if (anyArea) return (s16 *) anyArea;
        if (sDynosLevelWarps[aLevel].Count() > 0) {
            return (s16 *) &sDynosLevelWarps[aLevel][0];
        }
    }
    return NULL;
}

s16 *DynOS_Level_GetWarpDeath(s32 aLevel, s32 aArea) {
    DynOS_Level_Init();
    s16 *_Warp = DynOS_Level_GetWarp(aLevel, aArea, 0xF1);
    if (!_Warp) _Warp = DynOS_Level_GetWarp(aLevel, aArea, 0xF3);
    return _Warp;
}
