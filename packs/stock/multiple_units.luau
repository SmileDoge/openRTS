-- openRTS's stock rules: multiple units (the pack openrts-stock,
-- docs/scripting.md "Stock rules"). RTS content cannot tell a multiple
-- unit's car from a locomotive's train: its cars are Type ( Engine ) or
-- Wagon, with the ЭПТ as a coach has it. Known here by the series in the
-- folder's or the file's name.
--
-- - The compressor governor 6.5-8.0 atm (testers, 2026-11-16; the АК-11Б
--   cuts out between 0.3 and 0.9 MPa with 0.14-0.2 MPa between in and out,
--   Крылов, «Автоматические тормоза подвижного состава», 1983, §19). The
--   installation's files mostly give 105-130 psi (7.1-8.8).
-- - With the ЭПТ, the ЭВР 305-001 of multiple units (Крылов §38-39): its
--   release valve lets the cylinders out in 3.5-4.5 s, a coach's 305-000 in
--   8-10.
--
-- A car the menu gave another distributor keeps it (the player's choice,
-- in a mod, wins over these rules).

-- Series as they stand in folder and file names: mdd_ED4M-0004,
-- zdsEMU_ER9T-668, mdd_ER2T-...
local series = { "ED2T", "ED4M", "ED9M", "ED9T", "ED9E", "ER1", "ER2", "ER9", "ET2" }

local function is_multiple_unit(c)
    local names = string.upper(c.folder .. " " .. c.file)
    for _, s in ipairs(series) do
        -- The series at the start of a word: "_ED4M", "-ER9T", " ER2".
        if string.find(names, "%f[%w]" .. s) then
            return true
        end
    end
    return false
end

function car(c)
    if not is_multiple_unit(c) then
        return
    end
    if c.ep then
        c.distributor = "305-001"
    end
    if c.engine then
        c.compressor_on = 6.5
        c.compressor_off = 8.0
    end
end
