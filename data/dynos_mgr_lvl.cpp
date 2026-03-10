#include "dynos.cpp.h"

extern "C" {
#include "engine/level_script.h"
#include "game/skybox.h"
}

struct OverrideLevelScript {
    const void *originalScript;
    const void *newScript;
    GfxData *gfxData;
};

static std::vector<OverrideLevelScript> &DynosOverrideLevelScripts() {
    static std::vector<OverrideLevelScript> sDynosOverrideLevelScripts;
    return sDynosOverrideLevelScripts;
}

static std::string DynOS_Lvl_NormalizeScriptName(const char *name) {
    if (name == NULL) {
        return std::string();
    }
    std::string normalized = name;
    size_t slashPos = normalized.find_last_of("/\\");
    if (slashPos != std::string::npos) {
        normalized.erase(0, slashPos + 1);
    }
    size_t dotPos = normalized.rfind('.');
    if (dotPos != std::string::npos) {
        normalized.erase(dotPos);
    }
    return normalized;
}

static bool DynOS_Lvl_ScriptNamesMatch(const char *lhs, const char *rhs) {
    if (lhs == NULL || rhs == NULL) {
        return false;
    }
    if (!strcmp(lhs, rhs)) {
        return true;
    }
    return DynOS_Lvl_NormalizeScriptName(lhs) == DynOS_Lvl_NormalizeScriptName(rhs);
}

struct DynOSLvlScriptScoreCtx {
    s32 areaCount;
    s32 warpNodeCount;
    s32 marioPosCount;
};

static DynOSLvlScriptScoreCtx *sDynOSLvlScriptScoreCtx = NULL;

static s32 DynOS_Lvl_ScorePreprocess(u8 aType, void *aCmd) {
    (void) aCmd;
    if (sDynOSLvlScriptScoreCtx == NULL) {
        return 0;
    }

    switch (aType) {
        case 0x1F: sDynOSLvlScriptScoreCtx->areaCount++; break;      // AREA
        case 0x26:                                                  // WARP_NODE
        case 0x27: sDynOSLvlScriptScoreCtx->warpNodeCount++; break; // PAINTING_WARP_NODE
        case 0x2B: sDynOSLvlScriptScoreCtx->marioPosCount++; break; // MARIO_POS
        case 0x03:                                                  // SLEEP
        case 0x04: return 3;                                        // SLEEP_BEFORE_EXIT
    }
    return 0;
}

static s32 DynOS_Lvl_ScoreScript(const void *script) {
    if (script == NULL) {
        return -1;
    }

    DynOSLvlScriptScoreCtx ctx = { 0, 0, 0 };
    sDynOSLvlScriptScoreCtx = &ctx;
    DynOS_Level_ParseScript(script, DynOS_Lvl_ScorePreprocess);
    sDynOSLvlScriptScoreCtx = NULL;

    // Favor scripts that can actually spawn Mario and define warp graph.
    return (ctx.marioPosCount * 100) + (ctx.warpNodeCount * 20) + (ctx.areaCount * 4);
}

template <typename TNodes>
static DataNode<LevelScript> *DynOS_Lvl_SelectEntryScriptNode(TNodes &scripts, const char *requestedName) {
    if (scripts.Count() <= 0) {
        return NULL;
    }

    // First try direct/normalized name match.
    for (auto &scriptNode : scripts) {
        if (requestedName != NULL && scriptNode->mName.begin() != NULL && !strcmp(scriptNode->mName.begin(), requestedName)) {
            return scriptNode;
        }
    }
    for (auto &scriptNode : scripts) {
        if (DynOS_Lvl_ScriptNamesMatch(scriptNode->mName.begin(), requestedName)) {
            return scriptNode;
        }
    }

    // If names don't match, pick the script that looks like a playable entry script.
    DataNode<LevelScript> *bestNode = scripts[scripts.Count() - 1];
    s32 bestScore = DynOS_Lvl_ScoreScript(bestNode->mData);
    for (auto &scriptNode : scripts) {
        s32 score = DynOS_Lvl_ScoreScript(scriptNode->mData);
        if (score > bestScore) {
            bestScore = score;
            bestNode = scriptNode;
        }
    }
    return bestNode;
}

std::vector<std::pair<std::string, GfxData *>> &DynOS_Lvl_GetArray() {
    static std::vector<std::pair<std::string, GfxData *>> sDynosCustomLevelScripts;
    return sDynosCustomLevelScripts;
}

LevelScript* DynOS_Lvl_GetScript(const char* aScriptEntryName) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    for (size_t i = 0; i < _CustomLevelScripts.size(); ++i) {
        auto& pair = _CustomLevelScripts[i];
        if (DynOS_Lvl_ScriptNamesMatch(pair.first.c_str(), aScriptEntryName)) {
            auto& newScripts = pair.second->mLevelScripts;
            DataNode<LevelScript> *newScriptNode = DynOS_Lvl_SelectEntryScriptNode(newScripts, aScriptEntryName);
            if (newScriptNode == NULL) {
                return NULL;
            }
            return newScriptNode->mData;
        }
    }
    return NULL;
}

void DynOS_Lvl_ModShutdown() {
    DynOS_Level_Unoverride();

    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    if (!_CustomLevelScripts.empty()) {
        for (auto& pair : _CustomLevelScripts) {
            DynOS_Tex_Invalid(pair.second);
            Delete(pair.second);
        }
        _CustomLevelScripts.clear();
    }

    auto& _OverrideLevelScripts = DynosOverrideLevelScripts();
    _OverrideLevelScripts.clear();
}

void DynOS_Lvl_Activate(s32 modIndex, const SysPath &aFilename, const char *aLevelName) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    auto& _OverrideLevelScripts = DynosOverrideLevelScripts();

    // make sure vanilla levels were parsed
    DynOS_Level_Init();

    // check for duplicates
    for (auto &customLevel : _CustomLevelScripts) {
        if (customLevel.first == aLevelName) {
            return;
        }
    }

    std::string levelName = aLevelName;

    GfxData* _Node = DynOS_Lvl_LoadFromBinary(aFilename, levelName.c_str());
    if (!_Node) {
        return;
    }

    // remember index
    _Node->mModIndex = modIndex;

    // Add to levels
    _CustomLevelScripts.emplace_back(levelName, _Node);
    DynOS_Tex_Valid(_Node);

    // Override vanilla script
    auto& newScripts = _Node->mLevelScripts;
    if (newScripts.Count() <= 0) {
        PrintError("Could not find level scripts: '%s'", aLevelName);
        return;
    }

    DataNode<LevelScript> *newScriptNode = DynOS_Lvl_SelectEntryScriptNode(newScripts, aLevelName);
    if (newScriptNode == NULL) {
        PrintError("Could not select level script: '%s'", aLevelName);
        return;
    }

    const void* originalScript = DynOS_Builtin_ScriptPtr_GetFromName(newScriptNode->mName.begin());
    if (originalScript == NULL) {
        return;
    }

    DynOS_Level_Override((void*)originalScript, newScriptNode->mData, modIndex);
    _OverrideLevelScripts.push_back({ originalScript, newScriptNode->mData, _Node});
}

GfxData* DynOS_Lvl_GetActiveGfx(void) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    for (auto &lvlEntry : _CustomLevelScripts) {
        auto& gfxData = lvlEntry.second;
        auto& scripts = gfxData->mLevelScripts;
        for (auto& s : scripts) {
            if (gLevelScriptActive == s->mData) {
                return gfxData;
            }
        }
    }
    return NULL;
}

bool DynOS_Lvl_IsCustomGeoLayoutPtr(const void *aPtr) {
    if (aPtr == NULL) {
        return false;
    }

    GfxData *gfxData = DynOS_Lvl_GetActiveGfx();
    if (gfxData == NULL) {
        return false;
    }

    const u8 *ptr = (const u8 *) aPtr;
    for (auto &geoNode : gfxData->mGeoLayouts) {
        if (geoNode == NULL || geoNode->mData == NULL || geoNode->mSize == 0) {
            continue;
        }
        const u8 *begin = (const u8 *) geoNode->mData;
        const u8 *end = begin + ((size_t) geoNode->mSize * sizeof(GeoLayout));
        if (ptr >= begin && ptr < end) {
            return true;
        }
    }
    return false;
}

const char* DynOS_Lvl_GetToken(u32 index) {
    GfxData* gfxData = DynOS_Lvl_GetActiveGfx();
    if (gfxData == NULL) {
        return NULL;
    }

    // have to 1-index due to to pointer read code
    index = index - 1;

    if (index >= gfxData->mLuaTokenList.Count()) {
        return NULL;
    }

    return gfxData->mLuaTokenList[index].begin();
}

Trajectory* DynOS_Lvl_GetTrajectory(const char* aName) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();

    for (auto& script : _CustomLevelScripts) {
        for (auto& trajectoryNode : script.second->mTrajectories) {
            if (trajectoryNode->mName == aName) {
                return trajectoryNode->mData;
            }
        }
    }
    return NULL;
}

void DynOS_Lvl_LoadBackground(void *aPtr) {
    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();

    // ensure this texture list exists
    GfxData* foundGfxData = NULL;
    DataNode<TexData*>* foundList = NULL;
    for (auto& script : _CustomLevelScripts) {
        auto &textureLists = script.second->mTextureLists;
        for (auto& textureList : textureLists) {
            if (textureList == aPtr) {
                foundGfxData = script.second;
                foundList = textureList;
                goto double_break;
            }
        }
    }
double_break:

    if (foundList == NULL) {
        PrintError("Could not find custom background");
        return;
    }

    // Load up custom background
    for (s32 i = 0; i < 80; i++) {
        // find texture
        for (auto& tex : foundGfxData->mTextures) {
            if (tex->mData == foundList->mData[i]) {
                gCustomSkyboxPtrList[i] = (Texture*)tex;
                break;
            }
        }
    }
}

void *DynOS_Lvl_Override(void *aCmd) {
    // Track DynOS execution context per-command.
    // Custom DynOS level scripts are authored on little-endian hosts; the Wii U
    // interpreter swaps scalar reads when `gLevelScriptModIndex >= 0`.
    //
    // Flood custom stages call into vanilla/global scripts (e.g. `level_main_scripts_entry`)
    // via JUMP/JUMP_LINK. If we leave `gLevelScriptModIndex` set while executing those
    // vanilla scripts, the engine will swap their immediates and break segment loads/area init.
    //
    // Fix: set `gLevelScriptModIndex` based on whether `aCmd` is inside a DynOS-provided
    // script buffer, not based only on base script pointer equality.
    if (aCmd == NULL) {
        gLevelScriptModIndex = -1;
        gLevelScriptActive = NULL;
        return NULL;
    }

    // Cache the last matched DynOS script memory range to keep the per-command overhead low.
    static const u8 *sLastScriptBegin = NULL;
    static const u8 *sLastScriptEnd = NULL;
    static s32 sLastScriptModIndex = -1;
    static LevelScript *sLastScriptBase = NULL;

    auto& _OverrideLevelScripts = DynosOverrideLevelScripts();
    for (auto& overrideStruct : _OverrideLevelScripts) {
        if (aCmd == overrideStruct.originalScript || aCmd == overrideStruct.newScript) {
            aCmd = (void*)overrideStruct.newScript;
            break;
        }
    }

    const u8 *cmdPtr = (const u8 *) aCmd;
    if (sLastScriptBegin != NULL && cmdPtr >= sLastScriptBegin && cmdPtr < sLastScriptEnd) {
        gLevelScriptModIndex = sLastScriptModIndex;
        gLevelScriptActive = sLastScriptBase;
        return aCmd;
    }

    auto& _CustomLevelScripts = DynOS_Lvl_GetArray();
    for (auto& script : _CustomLevelScripts) {
        auto& scripts = script.second->mLevelScripts;
        for (auto& s : scripts) {
            if (s == NULL || s->mData == NULL || s->mSize == 0) {
                continue;
            }
            const u8 *begin = (const u8 *) s->mData;
            const u8 *end = begin + ((size_t) s->mSize * sizeof(LevelScript));
            if (cmdPtr >= begin && cmdPtr < end) {
                sLastScriptBegin = begin;
                sLastScriptEnd = end;
                sLastScriptModIndex = script.second->mModIndex;
                sLastScriptBase = s->mData;
                gLevelScriptModIndex = sLastScriptModIndex;
                gLevelScriptActive = sLastScriptBase;
                return aCmd;
            }
        }
    }

    // Not inside a DynOS-loaded script buffer; treat as vanilla.
    gLevelScriptModIndex = -1;
    gLevelScriptActive = NULL;
    return aCmd;
}
