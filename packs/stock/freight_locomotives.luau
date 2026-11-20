-- openRTS's stock rules: freight locomotives (the pack openrts-stock,
-- docs/scripting.md "Stock rules"). RTS content does not name a car's
-- distributor, and openRTS infers a locomotive's as a 292 (with the EP flag,
-- which freight locomotives carry too, a 305). A freight locomotive has the
-- ВР 483 (testers, 2026-11-20), and with it the 418 (cab_brakes.hpp).
-- Known here by the series in the folder's or the file's name.
--
-- A distributor the file names stays; a car the menu gave another keeps it
-- (the player's choice, in a mod, wins over these rules).

-- Series as they stand in folder and file names: mdd_VL80s-1234,
-- RTS_2TE10M-0001, mdd_2ES4k-..., tsrLoco_2M62U-... A leading digit (the
-- sections) is allowed: 2TE10, 3ES4K.
local series = {
    -- electric
    "VL8", "VL10", "VL11", "VL15", "VL22", "VL23", "VL60K", "VL80", "VL82", "VL85",
    "ES4K", "ES5K", "ES6", "ES7", "ES10",
    -- diesel, shunting
    "TE3", "TE10", "TE116", "TE25", "M62", "DM62", "TEM", "CHME", "TGM",
}

-- Passenger ones among them: the 2ТЭ10УТ (120 km/h).
local passenger = { "TE10UT" }

local function named(names, list)
    for _, s in ipairs(list) do
        -- The series at the start of a word, after its sections' digit:
        -- "_VL10", "-2TE10", " 3ES4K". One ending in a digit takes no more
        -- digits ("VL8" is not "VL80"); "TEM", "CHME" take their number.
        local from, to = string.find(names, "%f[%w]%d?" .. s)
        local digit_after = from and string.find(string.sub(names, to + 1, to + 1), "%d")
        if from and not (digit_after and string.find(s, "%d$")) then
            return true
        end
    end
    return false
end

function car(c)
    if not c.engine then
        return
    end
    local names = string.upper(c.folder .. " " .. c.file)
    if named(names, series) and not named(names, passenger) then
        c.distributor = "483"
    end
end
