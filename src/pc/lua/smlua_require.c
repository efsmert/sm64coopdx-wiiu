#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include "smlua.h"
#include "pc/mods/mods.h"
#include "pc/mods/mods_utils.h"
#include "pc/fs/fmem.h"

#define LOADING_SENTINEL ((void*)-1)

#ifdef TARGET_WII_U
struct WiiUModuleCacheEntry {
    struct Mod* mod;
    char* relativePath;
    int valueRef;
    bool loading;
};

static struct WiiUModuleCacheEntry* sWiiUModuleCache = NULL;
static u32 sWiiUModuleCacheCount = 0;
static u32 sWiiUModuleCacheCapacity = 0;
static lua_State* sWiiUModuleCacheState = NULL;

static void smlua_wiiu_module_cache_clear(lua_State* L, bool unrefValues) {
    if (sWiiUModuleCache) {
        for (u32 i = 0; i < sWiiUModuleCacheCount; i++) {
            struct WiiUModuleCacheEntry* e = &sWiiUModuleCache[i];
            if (unrefValues && L != NULL && e->valueRef > 0) {
                luaL_unref(L, LUA_REGISTRYINDEX, e->valueRef);
            }
            if (e->relativePath) {
                free(e->relativePath);
                e->relativePath = NULL;
            }
            e->valueRef = LUA_NOREF;
            e->loading = false;
            e->mod = NULL;
        }
        free(sWiiUModuleCache);
        sWiiUModuleCache = NULL;
    }
    sWiiUModuleCacheCount = 0;
    sWiiUModuleCacheCapacity = 0;
}

static void smlua_wiiu_module_cache_ensure_state(lua_State* L) {
    if (sWiiUModuleCacheState != L) {
        // The old lua_State has been replaced; drop stale cache metadata.
        smlua_wiiu_module_cache_clear(NULL, false);
        sWiiUModuleCacheState = L;
    }
}

static struct WiiUModuleCacheEntry* smlua_wiiu_module_cache_find(struct Mod* mod, const char* relativePath) {
    for (u32 i = 0; i < sWiiUModuleCacheCount; i++) {
        struct WiiUModuleCacheEntry* e = &sWiiUModuleCache[i];
        if (e->mod == mod && e->relativePath != NULL && strcmp(e->relativePath, relativePath) == 0) {
            return e;
        }
    }
    return NULL;
}

static struct WiiUModuleCacheEntry* smlua_wiiu_module_cache_get_or_add(struct Mod* mod, const char* relativePath) {
    struct WiiUModuleCacheEntry* existing = smlua_wiiu_module_cache_find(mod, relativePath);
    if (existing != NULL) {
        return existing;
    }

    if (sWiiUModuleCacheCount >= sWiiUModuleCacheCapacity) {
        u32 newCapacity = (sWiiUModuleCacheCapacity == 0) ? 64 : (sWiiUModuleCacheCapacity * 2);
        struct WiiUModuleCacheEntry* grown = realloc(sWiiUModuleCache, sizeof(struct WiiUModuleCacheEntry) * newCapacity);
        if (grown == NULL) {
            return NULL;
        }
        memset(grown + sWiiUModuleCacheCapacity, 0, sizeof(struct WiiUModuleCacheEntry) * (newCapacity - sWiiUModuleCacheCapacity));
        sWiiUModuleCache = grown;
        sWiiUModuleCacheCapacity = newCapacity;
    }

    struct WiiUModuleCacheEntry* e = &sWiiUModuleCache[sWiiUModuleCacheCount++];
    size_t len = strlen(relativePath);
    e->relativePath = calloc(len + 1, 1);
    if (e->relativePath == NULL) {
        sWiiUModuleCacheCount--;
        return NULL;
    }
    memcpy(e->relativePath, relativePath, len);
    e->mod = mod;
    e->valueRef = LUA_NOREF;
    e->loading = false;
    return e;
}
#endif

// table to track loaded modules per mod
void smlua_get_or_create_mod_loaded_table(lua_State* L, struct Mod* mod) {
    char registryKey[SYS_MAX_PATH + 16] = "";
    snprintf(registryKey, sizeof(registryKey), "mod_loaded_%s", mod->relativePath);

    lua_getfield(L, LUA_REGISTRYINDEX, registryKey);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, LUA_REGISTRYINDEX, registryKey);
    }
}

bool smlua_get_cached_module_result(lua_State* L, struct Mod* mod, struct ModFile* file) {
#ifdef TARGET_WII_U
    smlua_wiiu_module_cache_ensure_state(L);
    struct WiiUModuleCacheEntry* e = smlua_wiiu_module_cache_find(mod, file->relativePath);
    if (e == NULL) {
        return false;
    }

    if (e->loading) {
        LOG_LUA_LINE("loop or previous error loading module '%s'", file->relativePath);
        lua_pushnil(L);
        return true;
    }

    if (e->valueRef > 0) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, e->valueRef);
        return true;
    }
    return false;
#else
    smlua_get_or_create_mod_loaded_table(L, mod);
    lua_getfield(L, -1, file->relativePath);

    if (lua_touserdata(L, -1) == LOADING_SENTINEL) {
        LOG_LUA_LINE("loop or previous error loading module '%s'", file->relativePath);
        lua_pop(L, 1); // pop sentinel
        lua_pushnil(L);

        return true;
    }

    if (lua_isnil(L, -1)) {
        // not cached
        lua_pop(L, 2); // pop nil and loaded table
        return false;
    }

    // cached, remove loaded table and leave value on top
    lua_remove(L, -2);
    return true;
#endif
}

void smlua_mark_module_as_loading(lua_State* L, struct Mod* mod, struct ModFile* file) {
#ifdef TARGET_WII_U
    smlua_wiiu_module_cache_ensure_state(L);
    struct WiiUModuleCacheEntry* e = smlua_wiiu_module_cache_get_or_add(mod, file->relativePath);
    if (e == NULL) {
        LOG_LUA_LINE("module cache allocation failed for '%s'", file->relativePath);
        return;
    }
    if (e->valueRef > 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, e->valueRef);
        e->valueRef = LUA_NOREF;
    }
    e->loading = true;
#else
    smlua_get_or_create_mod_loaded_table(L, mod);
    lua_pushlightuserdata(L, LOADING_SENTINEL);
    lua_setfield(L, -2, file->relativePath);
    lua_pop(L, 1); // pop loaded table
#endif
}
 
void smlua_cache_module_result(lua_State* L, struct Mod* mod, struct ModFile* file, s32 prevTop) {
    if (lua_gettop(L) == prevTop) {
        lua_pushboolean(L, 1);
    } else if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_pushboolean(L, 1);
    }

#ifdef TARGET_WII_U
    smlua_wiiu_module_cache_ensure_state(L);
    struct WiiUModuleCacheEntry* e = smlua_wiiu_module_cache_get_or_add(mod, file->relativePath);
    if (e == NULL) {
        LOG_LUA_LINE("module cache allocation failed for '%s'", file->relativePath);
        return;
    }
    if (e->valueRef > 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, e->valueRef);
    }
    lua_pushvalue(L, -1); // duplicate result
    e->valueRef = luaL_ref(L, LUA_REGISTRYINDEX);
    e->loading = false;
#else
    smlua_get_or_create_mod_loaded_table(L, mod);

    lua_pushvalue(L, -2); // duplicate result
    lua_setfield(L, -2, file->relativePath); // loaded[file->relativePath] = result
    lua_pop(L, 1); // pop loaded table
#endif
}

static struct ModFile* smlua_find_mod_file(const char* moduleName) {
    char basePath[SYS_MAX_PATH] = "";
    char absolutePath[SYS_MAX_PATH] = "";
    char normalizedRelative[SYS_MAX_PATH] = "";

    if (!gLuaActiveMod) {
        return NULL;
    }

    // get the directory of the current file
    if (gLuaActiveModFile) {
        path_get_folder(gLuaActiveModFile->relativePath, basePath);
    }

    // resolve moduleName to a path relative to mod root
    resolve_relative_path(basePath, moduleName, absolutePath);

    char luaName[SYS_MAX_PATH] = "";
    char luacName[SYS_MAX_PATH] = "";
    snprintf(luaName, SYS_MAX_PATH, "%s.lua", absolutePath);
    snprintf(luacName, SYS_MAX_PATH, "%s.luac", absolutePath);

    // since mods' relativePaths are relative to the mod's root, we can do a direct comparison
    for (int i = 0; i < gLuaActiveMod->fileCount; i++) {
        struct ModFile* file = &gLuaActiveMod->files[i];

        // don't consider the currently running file
        if (file == gLuaActiveModFile) {
            continue;
        }

        // only consider lua files
        if (!path_ends_with(file->relativePath, ".lua") && !path_ends_with(file->relativePath, ".luac")) {
            continue;
        }

        // check for match, normalizing to system separators
        strcpy(normalizedRelative, file->relativePath);
        normalize_path(normalizedRelative);
        if (!strcmp(normalizedRelative, luaName) || !strcmp(normalizedRelative, luacName)) {
            return file;
        }
    }

    return NULL;
}

static int smlua_custom_require(lua_State* L) {
    const char* moduleName = luaL_checkstring(L, 1);

    struct Mod* activeMod = gLuaActiveMod;
    if (!activeMod) {
        LOG_LUA_LINE("require() called outside of mod context");
        return 0;
    }

    if (path_ends_with(moduleName, "/") || path_ends_with(moduleName, "\\")) {
        LOG_LUA_LINE("cannot require a directory");
        return 0;
    }

    // find the file in mod files
    struct ModFile* file = smlua_find_mod_file(moduleName);
    if (!file) {
        LOG_LUA_LINE("module '%s' not found in mod files", moduleName);
        return 0;
    }

    // tag it as a loaded lua module
    file->isLoadedLuaModule = true;

    // check cache first
    if (smlua_get_cached_module_result(L, activeMod, file)) {
        return 1;
    }

    // mark module as "loading" to prevent recursion
    smlua_mark_module_as_loading(L, activeMod, file);

    // cache the previous mod file
    struct ModFile* prevModFile = gLuaActiveModFile;

    // load and execute
    gLuaActiveModFile = file;
    s32 prevTop = lua_gettop(L);

    int rc = smlua_load_script(activeMod, file, activeMod->index, false);

    if (rc == LUA_OK) {
        smlua_cache_module_result(L, activeMod, file, prevTop);
    }

    gLuaActiveModFile = prevModFile;

    return 1; // return the module value
}

void smlua_bind_custom_require(lua_State* L) {
    // replace the global require function
    lua_pushcfunction(L, smlua_custom_require);
    lua_setglobal(L, "require");
}

void smlua_init_require_system(void) {
    lua_State* L = gLuaState;
    if (!L) return;

#ifdef TARGET_WII_U
    // Reset the Wii U-side module cache whenever a fresh lua_State is created.
    smlua_wiiu_module_cache_clear(L, false);
    sWiiUModuleCacheState = L;
#endif

    // initialize the custom require function
    smlua_bind_custom_require(L);

    // initialize loaded tables for each mod
#ifndef TARGET_WII_U
    for (int i = 0; i < gActiveMods.entryCount; i++) {
        struct Mod* mod = gActiveMods.entries[i];
        smlua_get_or_create_mod_loaded_table(L, mod);
        lua_pop(L, 1); // pop loaded table
    }
#endif
}
