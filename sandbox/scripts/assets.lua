-- Asset-Tour -- shows the asset/model/material half of the API.
-- Put this script on any object (e.g. an Empty) and press Play:
--   * M -> plant the first Model asset of the project in front of the camera
--   * T -> build a material from the first Texture asset and put it on the plant
--   * C -> recolour the object under the crosshair
--   * L -> list every asset to the console (stderr)

local planted = nil     -- entity id of the last planted model
local mat     = nil     -- material GUID created from a texture asset

-- The first asset of a type, or nil when the project has none.
local function firstAsset(kind)
    local list = game.assets(kind)
    return list[1]
end

local function inFront(dist)
    local px, py, pz = game.cameraPos()
    local dx, dy, dz = game.cameraDir()
    local x, z = px + dx * dist, pz + dz * dist
    return x, game.terrainHeight(x, z), z
end

function start(e)
    local models   = game.assets("Model")
    local textures = game.assets("Texture")
    game.setHud(string.format("%d models, %d textures   [M] plant  [T] texture  [C] colour  [L] list",
                              #models, #textures))
end

function update(e, dt, t)
    -- Plant a model asset: game.spawn takes the asset by name or GUID and sizes
    -- the entity from the model's own bounding box.
    if game.keyPressed(game.KEY_M) then
        local a = firstAsset("Model")
        if a then
            local x, y, z = inFront(8.0)
            planted = game.spawn{
                model   = a.name,       -- "tree.glb" -- or a.id (the GUID)
                x = x, y = y, z = z,
                scale   = 1.0,
                physics = game.PHYSICS_STATIC,
                name    = "planted",
            }
            game.log("planted", a.name, "as entity", planted)
        else
            game.setHud("no Model assets in this project")
        end
    end

    -- Build a material from a texture asset and assign it to the planted object.
    if game.keyPressed(game.KEY_T) then
        local tex = firstAsset("Texture")
        if tex and not mat then
            mat = game.createMaterial{
                name         = "Scripted",
                texture      = tex.name,   -- name, relative path or GUID
                roughness    = 0.4,
                reflectivity = 0.1,
            }
            game.log("created material", mat, "from", tex.name)
        end
        if mat and planted then game.setMaterial(planted, mat) end
    end

    -- Recolour whatever the crosshair points at (each object gets its own
    -- material, so this never tints anything else).
    if game.keyPressed(game.KEY_C) then
        local px, py, pz = game.cameraPos()
        local dx, dy, dz = game.cameraDir()
        local hit = game.raycast(px, py, pz, dx, dy, dz, 200.0)
        if hit then
            game.setColor(hit, math.random(), math.random(), math.random())
            game.setHud("recoloured " .. (game.getName(hit) or "?"))
        end
    end

    if game.keyPressed(game.KEY_L) then
        for _, a in ipairs(game.assets()) do
            game.log(a.type, a.path, a.id)
        end
    end
end
