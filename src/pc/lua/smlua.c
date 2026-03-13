#include "smlua.h"
#include "pc/lua/smlua_require.h"
#include "pc/lua/smlua_live_reload.h"
#include "game/hardcoded.h"
#include "pc/mods/mods.h"
#include "pc/mods/mods_utils.h"
#include "pc/mods/mod_storage.h"
#include "pc/mods/mod_fs.h"
#include "pc/crash_handler.h"
#include "pc/lua/utils/smlua_text_utils.h"
#include "pc/lua/utils/smlua_audio_utils.h"
#include "pc/lua/utils/smlua_model_utils.h"
#include "pc/lua/utils/smlua_level_utils.h"
#include "pc/lua/utils/smlua_anim_utils.h"
#include "pc/djui/djui.h"
#include "pc/fs/fmem.h"
#include <string.h>
#include <stdint.h>
#ifdef TARGET_WII_U
#include <stdarg.h>
#include <coreinit/debug.h>
typedef void* (*SmluaMemAllocFromDefaultHeapExFn)(uint32_t size, int32_t alignment);
typedef void* (*SmluaMemAllocFromDefaultHeapFn)(uint32_t size);
typedef void (*SmluaMemFreeToDefaultHeapFn)(void* ptr);
extern SmluaMemAllocFromDefaultHeapExFn MEMAllocFromDefaultHeapEx;
extern SmluaMemAllocFromDefaultHeapFn MEMAllocFromDefaultHeap;
extern SmluaMemFreeToDefaultHeapFn MEMFreeToDefaultHeap;
// Keep Wii U logging bounded so we can capture root errors without flooding Cemu.
static u32 sSmluaWiiULogBudget = 128;
static bool sSmluaWiiULogLimitNotified = false;
static void smlua_wiiu_log_limited(const char* fmt, ...) {
    if (fmt == NULL) {
        return;
    }
    if (sSmluaWiiULogBudget == 0) {
        if (!sSmluaWiiULogLimitNotified) {
            sSmluaWiiULogLimitNotified = true;
            OSReport("smlua: lua log budget reached, suppressing further lines\n");
        }
        return;
    }
    sSmluaWiiULogBudget--;
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    OSReport("%s", buffer);
}
#define SMLUA_WIIU_LOG(...) smlua_wiiu_log_limited(__VA_ARGS__)
#else
#define SMLUA_WIIU_LOG(...)
#endif

lua_State* gLuaState = NULL;
u8 gLuaInitializingScript = 0;
u8 gSmLuaSuppressErrors = 0;
struct Mod* gLuaLoadingMod = NULL;
struct Mod* gLuaActiveMod = NULL;
struct ModFile* gLuaActiveModFile = NULL;
struct Mod* gLuaLastHookMod = NULL;

void smlua_platform_log_lua(const char* text) {
#ifdef TARGET_WII_U
    if (text != NULL && text[0] != '\0') {
        SMLUA_WIIU_LOG("[LUA] %s\n", text);
    }
#else
    (void)text;
#endif
}

#ifdef TARGET_WII_U
static void smlua_wiiu_step(const char* stage) {
    if (stage == NULL) {
        return;
    }
    OSReport("wiiu-lua-step:%s\n", stage);
}

static int smlua_lua_panic(lua_State* L) {
    const char* err = lua_tostring(L, -1);
    if (err != NULL) {
        LOG_LUA("PANIC: %s", err);
    } else {
        LOG_LUA("PANIC: unprotected Lua error");
    }
    return 0;
}

#define SMLUA_WIIU_ALIGN 16u
#define SMLUA_WIIU_ALIGN_UP(v) (((v) + (SMLUA_WIIU_ALIGN - 1)) & ~(SMLUA_WIIU_ALIGN - 1))
#define SMLUA_WIIU_LUA_ARENA_SIZE (32u * 1024u * 1024u)

struct SmluaWiiUAllocHeader {
    u32 size;
    u32 tag;
    u64 reserved;
};

static const u32 sSmluaWiiUAllocTag = 0x534C5541u; // "SLUA"
static u8* sSmluaWiiULuaArena = NULL;
static u32 sSmluaWiiULuaArenaOffset = 0;

static bool smlua_wiiu_lua_arena_init(void) {
    if (sSmluaWiiULuaArena != NULL) {
        return true;
    }
    if (MEMAllocFromDefaultHeapEx == NULL) {
        SMLUA_WIIU_LOG("smlua: MEMAllocFromDefaultHeapEx is NULL\n");
        return false;
    }
    sSmluaWiiULuaArena = (u8*)MEMAllocFromDefaultHeapEx(SMLUA_WIIU_LUA_ARENA_SIZE, SMLUA_WIIU_ALIGN);
    sSmluaWiiULuaArenaOffset = 0;
    if (sSmluaWiiULuaArena == NULL) {
        SMLUA_WIIU_LOG("smlua: failed to allocate lua arena size=%u\n", SMLUA_WIIU_LUA_ARENA_SIZE);
        return false;
    }
    SMLUA_WIIU_LOG("smlua: lua arena base=%p size=%u\n", sSmluaWiiULuaArena, SMLUA_WIIU_LUA_ARENA_SIZE);
    return true;
}

static void smlua_wiiu_lua_arena_reset(void) {
    sSmluaWiiULuaArenaOffset = 0;
}

static void* smlua_wiiu_alloc_new(u32 wanted) {
    if (!smlua_wiiu_lua_arena_init()) {
        return NULL;
    }

    u32 total = SMLUA_WIIU_ALIGN_UP((u32)sizeof(struct SmluaWiiUAllocHeader) + wanted);
    if ((sSmluaWiiULuaArenaOffset + total) > SMLUA_WIIU_LUA_ARENA_SIZE) {
        static u32 sLuaAllocOomLogs = 0;
        if (sLuaAllocOomLogs < 8) {
            sLuaAllocOomLogs++;
            SMLUA_WIIU_LOG("smlua: lua arena OOM wanted=%u total=%u off=%u cap=%u\n",
                           wanted, total, sSmluaWiiULuaArenaOffset, SMLUA_WIIU_LUA_ARENA_SIZE);
        }
        return NULL;
    }

    struct SmluaWiiUAllocHeader* header =
        (struct SmluaWiiUAllocHeader*)(sSmluaWiiULuaArena + sSmluaWiiULuaArenaOffset);
    sSmluaWiiULuaArenaOffset += total;
    header->size = wanted;
    header->tag = sSmluaWiiUAllocTag;
    header->reserved = 0;
    return (void*)((u8*)header + sizeof(struct SmluaWiiUAllocHeader));
}

static void* smlua_wiiu_lua_alloc(UNUSED void* ud, void* ptr, UNUSED size_t osize, size_t nsize) {
    if (nsize == 0) {
        // Bump arena allocator: frees are no-ops until smlua shutdown.
        return NULL;
    }

    u32 wanted = SMLUA_WIIU_ALIGN_UP((u32)nsize);
    if (ptr == NULL) {
        return smlua_wiiu_alloc_new(wanted);
    }

    struct SmluaWiiUAllocHeader* oldHeader =
        (struct SmluaWiiUAllocHeader*)((u8*)ptr - sizeof(struct SmluaWiiUAllocHeader));
    if (oldHeader->tag != sSmluaWiiUAllocTag) {
        static u32 sLuaAllocTagMismatchLogs = 0;
        if (sLuaAllocTagMismatchLogs < 8) {
            sLuaAllocTagMismatchLogs++;
            SMLUA_WIIU_LOG("smlua: alloc tag mismatch ptr=%p oldTag=0x%08x nsize=%u\n",
                           ptr, oldHeader->tag, (u32)nsize);
        }
        return NULL;
    }
    if (oldHeader->size >= wanted) {
        return ptr;
    }

    void* newPtr = smlua_wiiu_alloc_new(wanted);
    if (newPtr == NULL) {
        return NULL;
    }
    size_t copySize = oldHeader->size < wanted ? oldHeader->size : wanted;
    memcpy(newPtr, ptr, copySize);
    return newPtr;
}
#endif

static lua_State* smlua_create_lua_state(void) {
#ifdef TARGET_WII_U
    // Prefer donor-style allocator path on Wii U for stability in host/menu init.
    lua_State* L = luaL_newstate();
    if (L == NULL) {
        SMLUA_WIIU_LOG("smlua: luaL_newstate returned NULL, trying custom allocator\n");
        smlua_wiiu_lua_arena_reset();
        L = lua_newstate(smlua_wiiu_lua_alloc, NULL);
    }
    if (L == NULL) {
        SMLUA_WIIU_LOG("smlua: lua state allocation failed\n");
    }
    if (L != NULL) {
        lua_atpanic(L, smlua_lua_panic);
    }
#else
    lua_State* L = luaL_newstate();
#endif
    return L;
}

static bool smlua_exec_buffer(const char* buffer, size_t length, const char* chunkName) {
    lua_State* L = gLuaState;
    int rc = luaL_loadbuffer(L, buffer, length, chunkName);
    if (rc == LUA_OK) {
        rc = smlua_pcall(L, 0, 0, 0);
    }
    if (rc != LUA_OK) {
        LOG_LUA("Failed to load lua chunk '%s'.", chunkName);
        LOG_LUA("%s", smlua_to_string(L, lua_gettop(L)));
        lua_pop(L, lua_gettop(L));
        return false;
    }
    lua_pop(L, lua_gettop(L));
    return true;
}

void smlua_mod_error(void) {
    struct Mod* mod = gLuaActiveMod;
    if (mod == NULL) { mod = gLuaLastHookMod; }
    if (mod == NULL) { return; }
    char txt[255] = { 0 };
    snprintf(txt, 254, "'%s\\#ff0000\\' has script errors!", mod->name);
    static const struct DjuiColor color = { 255, 0, 0, 255 };
    djui_lua_error(txt, color);
}

void smlua_mod_warning(void) {
    struct Mod* mod = gLuaActiveMod;
    if (mod == NULL) { mod = gLuaLastHookMod; }
    if (mod == NULL) { return; }
    if (mod->ignoreScriptWarnings) { return; }
    char txt[255] = { 0 };
    snprintf(txt, 254, "'%s\\#ffe600\\' has script warnings!", mod->name);
    static const struct DjuiColor color = { 255, 230, 0, 255 };
    djui_lua_error(txt, color);
}

int smlua_error_handler(lua_State* L) {
    if (lua_type(L, -1) == LUA_TSTRING) {
        LOG_LUA("%s", lua_tostring(L, -1));
    }
    smlua_logline();
    smlua_dump_stack();
    // Propagate the original Lua error object back to lua_pcall.
    return 1;
}

int smlua_pcall(lua_State* L, int nargs, int nresults, UNUSED int errfunc) {
    gSmLuaConvertSuccess = true;
    lua_pushcfunction(L, smlua_error_handler);
    int errorHandlerIndex = 1;
    lua_insert(L, errorHandlerIndex);

    int rc = lua_pcall(L, nargs, nresults, errorHandlerIndex);

#ifdef TARGET_WII_U
    if (rc != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        if (err != NULL) {
            SMLUA_WIIU_LOG("smlua: pcall failed rc=%d err=%s\n", rc, err);
        } else {
            SMLUA_WIIU_LOG("smlua: pcall failed rc=%d err=(non-string)\n", rc);
        }
    }
#endif

    lua_remove(L, errorHandlerIndex);
    return rc;
}

void smlua_exec_file(const char* path) {
    lua_State* L = gLuaState;
    if (luaL_dofile(L, path) != LUA_OK) {
        LOG_LUA("Failed to load lua file '%s'.", path);
        LOG_LUA("%s", smlua_to_string(L, lua_gettop(L)));
    }
    lua_pop(L, lua_gettop(L));
}

void smlua_exec_str(const char* str) {
    if (str == NULL) { return; }
    smlua_exec_buffer(str, strlen(str), "smlua_exec_str");
}

#ifdef TARGET_WII_U
static const char* sWiiUMinimalLuaBootstrap =
    "math.randomseed(get_time())\n"
    "_SyncTable = {\n"
    "__index = function (t,k)\n"
    "local _table = rawget(t, '_table')\n"
    "return _table[k]\n"
    "end,\n"
    "__newindex = function (t,k,v)\n"
    "local _table = rawget(t, '_table')\n"
    "if _table[k] == v then return end\n"
    "_set_sync_table_field(t, k, v)\n"
    "end\n"
    "}\n"
    "_ReadOnlyTable = {\n"
    "__index = function (t,k)\n"
    "local _table = rawget(t, '_table')\n"
    "return _table[k]\n"
    "end,\n"
    "__newindex = function (_,k,_) error('Attempting to modify key `' .. k .. '` of read-only table') end,\n"
    "__metatable = false\n"
    "}\n"
    "function table.copy(t)\n"
    "return table_copy(t)\n"
    "end\n"
    "function table.deepcopy(t)\n"
    "return table_deepcopy(t)\n"
    "end\n"
    "function create_read_only_table(data)\n"
    "local t = {}\n"
    "local mt = {\n"
    "__index = data,\n"
    "__newindex = function(_, k, _) error('Attempting to modify key `' .. k .. '` of read-only table') end,\n"
    "__call = function() return table_copy(data) end,\n"
    "__metatable = false\n"
    "}\n"
    "setmetatable(t, mt)\n"
    "return t\n"
    "end\n"
    "COURSE_NONE = 0\n"
    "PLAYER_INTERACTIONS_PVP = 2\n"
    "HOOK_UPDATE = 0\n"
    "HOOK_ON_SYNC_VALID = 14\n"
    "HOOK_ON_PAUSE_EXIT = 17\n"
    "MAX_PLAYERS = 16\n";

static void smlua_exec_constants_wiiu(const char* constants) {
    if (constants != NULL && constants[0] != '\0') {
        if (smlua_exec_buffer(constants, strlen(constants), "smlua_constants_autogen")) {
            return;
        }
        LOG_INFO("Wii U Lua: full constants bootstrap failed, falling back to minimal constants.");
    } else {
        LOG_INFO("Wii U Lua: missing full constants payload, using minimal constants bootstrap.");
    }
    smlua_exec_str(sWiiUMinimalLuaBootstrap);
}
#endif

static void smlua_install_compat_globals(void) {
    // Some shipped behavior scripts call delete_at_dark() even when the
    // day-night-cycle mod is disabled. Provide a safe no-op fallback.
    static const char sCompatGlobals[] =
        "if type(delete_at_dark) ~= 'function' then\n"
        "  function delete_at_dark(_) end\n"
        "end\n";
    smlua_exec_buffer(sCompatGlobals, strlen(sCompatGlobals), "@compat_globals");
}

static bool smlua_path_ptr_is_plausible(const char* path) {
    uintptr_t p = (uintptr_t)path;
    return path != NULL && p >= 0x00100000u && p <= 0x7fffffffu;
}

int smlua_load_script(struct Mod* mod, struct ModFile* file, u16 remoteIndex, bool isModInit) {
    int rc = LUA_OK;
    if (!smlua_path_ptr_is_plausible(file->cachedPath)) {
        LOG_LUA("Failed to load lua script from mod '%s': invalid path pointer.",
                (mod != NULL && mod->name != NULL) ? mod->name : "unknown");
        return LUA_ERRFILE;
    }

    lua_State* L = gLuaState;

    s32 prevTop = lua_gettop(L);

    gSmLuaConvertSuccess = true;
    gLuaInitializingScript = 1;
    LOG_INFO("Loading lua script '%s'", file->cachedPath);

    FILE *f = f_open_r(file->cachedPath);
    if (!f) {
        LOG_LUA("Failed to load lua script '%s': File not found.", file->cachedPath);
        gLuaInitializingScript = 0;
        lua_settop(L, prevTop);
        return LUA_ERRFILE;
    }
    if (f_seek(f, 0L, SEEK_END) != 0) {
        LOG_LUA("Failed to load lua script '%s': Cannot seek to end.", file->cachedPath);
        gLuaInitializingScript = 0;
        f_close(f);
        f_delete(f);
        lua_settop(L, prevTop);
        return LUA_ERRFILE;
    }
    long fileSizeLong = f_tell(f);
    if (fileSizeLong < 0) {
        LOG_LUA("Failed to load lua script '%s': Cannot determine file size.", file->cachedPath);
        gLuaInitializingScript = 0;
        f_close(f);
        f_delete(f);
        lua_settop(L, prevTop);
        return LUA_ERRFILE;
    }
    if ((unsigned long)fileSizeLong > (unsigned long)(SIZE_MAX - 1)) {
        LOG_LUA("Failed to load lua script '%s': File too large.", file->cachedPath);
        gLuaInitializingScript = 0;
        f_close(f);
        f_delete(f);
        lua_settop(L, prevTop);
        return LUA_ERRMEM;
    }
    if (f_seek(f, 0L, SEEK_SET) != 0) {
        LOG_LUA("Failed to load lua script '%s': Cannot seek to start.", file->cachedPath);
        gLuaInitializingScript = 0;
        f_close(f);
        f_delete(f);
        lua_settop(L, prevTop);
        return LUA_ERRFILE;
    }

    size_t length = (size_t)fileSizeLong;
    u8 *buffer = calloc(length + 1, 1);
    if (!buffer) {
        LOG_LUA("Failed to load lua script '%s': Cannot allocate buffer.", file->cachedPath);
        gLuaInitializingScript = 0;
        f_close(f);
        f_delete(f);
        lua_settop(L, prevTop);
        return LUA_ERRMEM;
    }
    if (length > 0) {
        size_t bytesRead = f_read(buffer, 1, length, f);
        if (bytesRead < length) {
            LOG_LUA("Failed to load lua script '%s': Unexpected EOF (%llu/%llu bytes).",
                    file->cachedPath,
                    (unsigned long long)bytesRead,
                    (unsigned long long)length);
            gLuaInitializingScript = 0;
            free(buffer);
            f_close(f);
            f_delete(f);
            lua_settop(L, prevTop);
            return LUA_ERRFILE;
        }
    }
    buffer[length] = 0;
    f_close(f);
    f_delete(f);
#ifdef TARGET_WII_U
    SMLUA_WIIU_LOG("smlua: load '%s'\n", file->cachedPath);
#endif
    rc = luaL_loadbuffer(L, (const char*)buffer, length, file->cachedPath);
    free(buffer);
    if (rc != LUA_OK) {
        LOG_LUA("Failed to load lua script '%s'.", file->cachedPath);
        LOG_LUA("%s", smlua_to_string(L, lua_gettop(L)));
        gLuaInitializingScript = 0;
        lua_settop(L, prevTop);
        return rc;
    }
#ifdef TARGET_WII_U
    SMLUA_WIIU_LOG("smlua: loaded chunk '%s'\n", file->cachedPath);
#endif

    if (isModInit) {
        // check if this is the first time this mod has been loaded
        lua_getfield(L, LUA_REGISTRYINDEX, mod->relativePath);
        bool firstInit = (lua_type(L, -1) == LUA_TNIL);
        lua_pop(L, 1);
#ifdef TARGET_WII_U
        SMLUA_WIIU_LOG("smlua: mod init firstInit=%d '%s'\n", firstInit ? 1 : 0, mod->relativePath);
#endif

        // create mod's "global" table
        if (firstInit) {
#ifdef TARGET_WII_U
            SMLUA_WIIU_LOG("smlua: create env '%s'\n", mod->relativePath);
#endif
            lua_newtable(L); // create _ENV tables
            lua_newtable(L); // create metatable
            lua_getglobal(L, "_G"); // get global table

            // remove certain default functions
            lua_pushstring(L, "load");           lua_pushnil(L); lua_settable(L, -3);
            lua_pushstring(L, "loadfile");       lua_pushnil(L); lua_settable(L, -3);
            lua_pushstring(L, "loadstring");     lua_pushnil(L); lua_settable(L, -3);
            lua_pushstring(L, "collectgarbage"); lua_pushnil(L); lua_settable(L, -3);
            lua_pushstring(L, "dofile");         lua_pushnil(L); lua_settable(L, -3);

            // set global as the metatable
            lua_setfield(L, -2, "__index");
            lua_setmetatable(L, -2);

            // push to registry with path as name (must be unique)
            lua_setfield(L, LUA_REGISTRYINDEX, mod->relativePath);
#ifdef TARGET_WII_U
            SMLUA_WIIU_LOG("smlua: env registered '%s'\n", mod->relativePath);
#endif
        }

        // load mod's "global" table
        lua_getfield(L, LUA_REGISTRYINDEX, mod->relativePath);
        lua_setupvalue(L, 1, 1); // set upvalue (_ENV)

        // load per-file globals
        if (firstInit) {
#ifdef TARGET_WII_U
            SMLUA_WIIU_LOG("smlua: sync globals init '%s'\n", mod->relativePath);
#endif
            smlua_sync_table_init_globals(mod->relativePath, remoteIndex);
#ifdef TARGET_WII_U
            SMLUA_WIIU_LOG("smlua: sync globals done '%s'\n", mod->relativePath);
            SMLUA_WIIU_LOG("smlua: cobject per-file init '%s'\n", mod->relativePath);
#endif
            smlua_cobject_init_per_file_globals(mod->relativePath);
#ifdef TARGET_WII_U
            SMLUA_WIIU_LOG("smlua: cobject per-file done '%s'\n", mod->relativePath);
#endif
        }
    } else {
        // this block is run on files that are loaded for 'require' function
        // get the mod's global table
        lua_getfield(L, LUA_REGISTRYINDEX, mod->relativePath);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            LOG_LUA("mod environment not found");
            lua_settop(L, prevTop);
            return LUA_ERRRUN;
        }
        lua_setupvalue(L, -2, 1); // set _ENV
    }

    // run chunks
    LOG_INFO("Executing '%s'", file->relativePath);
#ifdef TARGET_WII_U
    SMLUA_WIIU_LOG("smlua: exec '%s/%s'\n", mod->relativePath, file->relativePath);
#endif
    rc = smlua_pcall(L, 0, 1, 0);
    if (rc != LUA_OK) {
        LOG_LUA("Failed to execute lua script '%s'.", file->cachedPath);
#ifdef TARGET_WII_U
        SMLUA_WIIU_LOG("smlua: exec failed rc=%d file='%s/%s'\n", rc, mod->relativePath, file->relativePath);
        {
            const char* err = lua_tostring(L, -1);
            if (err != NULL) {
                SMLUA_WIIU_LOG("smlua: exec error text: %s\n", err);
            }
        }
#endif
    }

    gLuaInitializingScript = 0;

    return rc;
}

void smlua_init(void) {
    smlua_shutdown();

#ifdef TARGET_WII_U
    sSmluaWiiULogBudget = 512;
    sSmluaWiiULogLimitNotified = false;
    smlua_wiiu_step("init_begin");
    LOG_INFO("wiiu-lua: smlua_init begin");
    SMLUA_WIIU_LOG("smlua: init begin\n");
#endif
    #ifdef TARGET_WII_U
    smlua_wiiu_step("create_state_begin");
    #endif
    gLuaState = smlua_create_lua_state();
    #ifdef TARGET_WII_U
    smlua_wiiu_step("create_state_done");
    #endif
    if (gLuaState == NULL) {
        LOG_ERROR("Failed to create Lua state");
        return;
    }
#ifdef TARGET_WII_U
    {
        void* allocUd = NULL;
        lua_Alloc allocf = lua_getallocf(gLuaState, &allocUd);
        SMLUA_WIIU_LOG("smlua: allocf=%p ud=%p\n", allocf, allocUd);
    }
#endif
    lua_State* L = gLuaState;

    // load libraries
#ifdef TARGET_WII_U
    smlua_wiiu_step("load_libs_begin");
    LOG_INFO("wiiu-lua: loading lua libs");
    SMLUA_WIIU_LOG("smlua: load libs\n");
#endif
    luaopen_base(L);
#if defined(DEVELOPMENT)
    luaL_requiref(L, "debug", luaopen_debug, 1);
    luaL_requiref(L, "io", luaopen_io, 1);
    luaL_requiref(L, "os", luaopen_os, 1);
    luaL_requiref(L, "package", luaopen_package, 1);
#endif
    luaL_requiref(L, "math", luaopen_math, 1);
    luaL_requiref(L, "string", luaopen_string, 1);
    luaL_requiref(L, "table", luaopen_table, 1);
    luaL_requiref(L, "coroutine", luaopen_coroutine, 1);
    luaL_requiref(L, "utf8", luaopen_utf8, 1);
    #ifdef TARGET_WII_U
    smlua_wiiu_step("load_libs_done");
    #endif

#ifdef TARGET_WII_U
    smlua_wiiu_step("bind_group_begin");
    LOG_INFO("wiiu-lua: binding hooks and functions");
    SMLUA_WIIU_LOG("smlua: bind hooks/functions\n");
#endif
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_hooks_begin");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_hooks begin\n");
    smlua_bind_hooks();
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_hooks_done");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_hooks done\n");
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_cobject_begin");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_cobject begin\n");
    smlua_bind_cobject();
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_cobject_done");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_cobject done\n");
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_functions_begin");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_functions begin\n");
    smlua_bind_functions();
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_functions_done");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_functions done\n");
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_functions_autogen_begin");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_functions_autogen begin\n");
    smlua_bind_functions_autogen();
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_functions_autogen_done");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_functions_autogen done\n");
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_sync_table_begin");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_sync_table begin\n");
    smlua_bind_sync_table();
    #ifdef TARGET_WII_U
    smlua_wiiu_step("bind_sync_table_done");
    #endif
    SMLUA_WIIU_LOG("smlua: bind_sync_table done\n");
    #ifdef TARGET_WII_U
    smlua_wiiu_step("init_require_begin");
    #endif
    SMLUA_WIIU_LOG("smlua: init_require begin\n");
    smlua_init_require_system();
    #ifdef TARGET_WII_U
    smlua_wiiu_step("init_require_done");
    #endif
    SMLUA_WIIU_LOG("smlua: init_require done\n");

    extern char gSmluaConstants[];
#ifdef TARGET_WII_U
    SMLUA_WIIU_LOG("smlua: exec_constants begin\n");
    smlua_exec_constants_wiiu(gSmluaConstants);
    SMLUA_WIIU_LOG("smlua: exec_constants done\n");
#else
    smlua_exec_str(gSmluaConstants);
#endif
    SMLUA_WIIU_LOG("smlua: compat_globals begin\n");
    smlua_install_compat_globals();
    SMLUA_WIIU_LOG("smlua: compat_globals done\n");

#ifdef TARGET_WII_U
    LOG_INFO("wiiu-lua: init globals");
    SMLUA_WIIU_LOG("smlua: init globals\n");
#endif
    smlua_cobject_init_globals();
    smlua_model_util_initialize();

    // load scripts
    mods_size_enforce(&gActiveMods);
    LOG_INFO("Loading scripts:");
    for (int i = 0; i < gActiveMods.entryCount; i++) {
        struct Mod* mod = gActiveMods.entries[i];
        LOG_INFO("    %s", mod->relativePath);
#ifdef TARGET_WII_U
        SMLUA_WIIU_LOG("smlua: load mod '%s' files=%d\n", mod->relativePath, mod->fileCount);
        u32 wiiuRootLuaCandidates = 0;
        u32 wiiuRootLuaLoaded = 0;
        u32 wiiuRootLuaCached = 0;
#endif
        gLuaLoadingMod = mod;
        gLuaActiveMod = mod;
        gLuaLastHookMod = mod;
        gLuaLoadingMod->customBehaviorIndex = 0;
        gPcDebug.lastModRun = gLuaActiveMod;
        for (int j = 0; j < mod->fileCount; j++) {
            struct ModFile* file = &mod->files[j];
            // skip loading non-lua files
            if (!(path_ends_with(file->relativePath, ".lua") || path_ends_with(file->relativePath, ".luac"))) {
                continue;
            }

            // skip loading scripts in subdirectories
            if (strchr(file->relativePath, '/') != NULL || strchr(file->relativePath, '\\') != NULL) {
                continue;
            }

#ifdef TARGET_WII_U
            wiiuRootLuaCandidates++;
            SMLUA_WIIU_LOG("smlua: root file candidate '%s/%s'\n", mod->relativePath, file->relativePath);
#endif
            gLuaActiveModFile = file;

            // file has been required by some module before this
            if (!smlua_get_cached_module_result(L, mod, file)) {
                smlua_mark_module_as_loading(L, mod, file);

                s32 prevTop = lua_gettop(L);
                int rc = smlua_load_script(mod, file, i, true);

                if (rc == LUA_OK) {
                    smlua_cache_module_result(L, mod, file, prevTop);
#ifdef TARGET_WII_U
                    wiiuRootLuaLoaded++;
#endif
                }
            } else {
#ifdef TARGET_WII_U
                wiiuRootLuaCached++;
                SMLUA_WIIU_LOG("smlua: root file cached '%s/%s'\n", mod->relativePath, file->relativePath);
#endif
            }

            lua_settop(L, 0);
        }
#ifdef TARGET_WII_U
        SMLUA_WIIU_LOG("smlua: mod summary '%s' rootCandidates=%u loaded=%u cached=%u\n",
                       mod->relativePath, wiiuRootLuaCandidates, wiiuRootLuaLoaded, wiiuRootLuaCached);
#endif
        gLuaActiveMod = NULL;
        gLuaActiveModFile = NULL;
        gLuaLoadingMod = NULL;
    }

    smlua_call_event_hooks(HOOK_ON_MODS_LOADED);
}

void smlua_update(void) {
    lua_State* L = gLuaState;
    if (L == NULL) { return; }

    if (network_allow_mod_dev_mode()) { smlua_live_reload_update(L); }

    audio_sample_destroy_pending_copies();

    smlua_call_event_hooks(HOOK_UPDATE);

    // Collect our garbage after calling our hooks.
    // If we don't, Lag can quickly build up from our mods.
    // Truth is smlua generates so much garbage that the
    // incremental collection fails to keep up after some time.
    // So, for now, stop the GC from running during the hooks
    // and perform a full GC at the end of the frame.
    // EDIT: That builds up lag over time, so we need to keep
    // doing incremental garbage collection.
    // The real fix would be to make smlua produce less
    // garbage.
    // lua_gc(L, LUA_GCSTOP, 0);
    // lua_gc(L, LUA_GCCOLLECT, 0);
}

void smlua_shutdown(void) {
    hardcoded_reset_default_values();
    smlua_text_utils_reset_all();
    smlua_audio_utils_reset_all();
    audio_custom_shutdown();
    smlua_clear_hooks();
    smlua_model_util_clear();
    smlua_level_util_reset();
    smlua_anim_util_reset();
    mod_storage_shutdown();
    mod_fs_shutdown();
    lua_State* L = gLuaState;
    if (L != NULL) {
        lua_close(L);
        gLuaState = NULL;
    }
#ifdef TARGET_WII_U
    smlua_wiiu_lua_arena_reset();
#endif
    gLuaLoadingMod = NULL;
    gLuaActiveMod = NULL;
    gLuaActiveModFile = NULL;
    gLuaLastHookMod = NULL;
}
