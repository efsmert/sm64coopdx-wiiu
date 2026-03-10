sArenaSpawns = {}

function bhv_arena_spawn_init(obj)
    table.insert(sArenaSpawns, {
        pos = { x = obj.oPosX, y = obj.oPosY, z = obj.oPosZ },
        yaw = obj.oFaceAngleYaw,
        type = (obj.oBehParams >> 24) & 0xFF,
    })
    obj_mark_for_deletion(obj)
end

id_bhvArenaSpawn = hook_behavior(nil, OBJ_LIST_LEVEL, true, bhv_arena_spawn_init, nil)

-------------

function find_spawn_point()
    local spawnCount = #sArenaSpawns
    if spawnCount == 0 then
        return nil
    end

    if gGameModes[gGlobalSyncTable.gameMode].teamSpawns then
        local s = gPlayerSyncTable[0]
        local team = (s ~= nil) and s.team or nil
        if team ~= nil then
            local teamSpawns = {}
            for i = 1, spawnCount do
                local spawn = sArenaSpawns[i]
                if spawn ~= nil and spawn.type == team then
                    table.insert(teamSpawns, spawn)
                end
            end
            if #teamSpawns > 0 then
                return teamSpawns[math.random(#teamSpawns)]
            end
        end
    end

    local spawn = sArenaSpawns[math.random(spawnCount)]
    return spawn
end

function on_level_init()
    sArenaSpawns = {}
end

hook_event(HOOK_ON_LEVEL_INIT, on_level_init)
