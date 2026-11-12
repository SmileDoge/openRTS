--!strict
-- ВР 292 for openRTS's realistic air model, the built-in one written in
-- Luau (simulation/src/distributor.cpp does the same in C++): a sample to
-- start your own distributor from. docs/scripting.md describes the table.
--
-- Put it next to a .wag / .eng and name it there:
--     ORTSBrakeDistributor ( "vr292.luau" )
-- Try it on the stand first:
--     openrts-brake-stand --script vr292.luau

-- What the car's table holds (pressures in atm, volumes in m³).
type Car = {
	pipe: number,       -- ТМ here; lower it to take air from the pipe
	rate: number,       -- how fast the pipe changes, atm/s
	zr: number,         -- ЗР
	tc: number,         -- ТЦ
	ref: number,        -- a level to remember (here: the ЗР when braking began)
	braking: boolean,   -- the ВР is in its braking position (applying or lap), not release
	emergency: boolean, -- the emergency front has reached the car
	ep: number,         -- the ЭПТ target, atm (< 0: none)
	ep_wired: boolean,
	tc_max: number,     -- ТЦ of a full application
	ratio: number,      -- TripleValveRatio
	v_pipe: number,
	v_zr: number,
	v_tc: number,
	mass: number,       -- kg
	kind: string,       -- the built-in kind the car would get: "292", "483", "305"
	load: string,       -- the 483's load mode: "empty", "medium", "loaded"
	mountain: boolean,  -- the 483's mountain mode
	plug: string,       -- the 292's plug: "short" (К), "long" (Д), "no_accelerator" (УВ)
	state: { [string]: any }, -- yours, kept between steps
}

local STABILITY = 0.5 / 60   -- atm/s the ЗР flows back on a slow fall: no braking
local APPLY = 0.10           -- ЗР − ТМ that starts an application
local QUICK_SERVICE = 0.06   -- atm the quick-service valve takes from the pipe
local RELEASE = 0.12         -- a rise this far above the ЗР releases fully
local ZR_CHARGE = 35         -- s, the ЗР's charging time constant

-- atm/s by the plug: how fast the ТЦ fills, empties and fills in emergency.
-- К: release 9–12 s, emergency to 3.5 in 6–9 s; Д and УВ about twice as slow.
local RATES = {
	short = { fill = 0.5, release = 0.35, emergency = 0.5 },
	long = { fill = 0.25, release = 0.16, emergency = 0.25 },
	no_accelerator = { fill = 0.25, release = 0.16, emergency = 0.25 },
}

-- ТЦ up by `rise`, the air from the ЗР.
local function fill(c: Car, rise: number)
	c.tc += rise
	c.zr = math.max(c.zr - rise * c.v_tc / c.v_zr, 0)
end

function step(c: Car, dt: number)
	local p = c.pipe
	local rates = RATES[c.plug] or RATES.short
	if c.emergency then
		c.braking = true
		local rise = math.min(c.tc_max - c.tc, rates.emergency * dt)
		if rise > 0 and c.zr > c.tc then
			fill(c, rise)
		end
		return
	end
	-- The ЗР charges from the pipe while released.
	if p > c.zr and not c.braking then
		local charge = (p - c.zr) * dt / ZR_CHARGE
		c.zr += charge
		c.pipe = math.max(c.pipe - charge * c.v_zr / c.v_pipe, 0)
	end
	if not c.braking then
		-- A slow fall: the ЗР flows back into the pipe and nothing brakes.
		if p < c.zr then
			c.zr -= math.min(c.zr - p, STABILITY * dt)
		end
		if c.zr - p >= APPLY and p < 4.9 then
			c.braking = true
			c.ref = c.zr
			c.pipe = math.max(c.pipe - QUICK_SERVICE, 0)
		end
	end
	if c.braking then
		local ratio = if c.ratio > 0.5 then c.ratio else 3
		local target = math.min(c.tc_max, ratio * (c.ref - p))
		if c.tc < target and c.zr > c.tc then
			fill(c, math.min(target - c.tc, rates.fill * dt))
		end
		-- A rise above the ЗР releases fully.
		if p >= c.zr + RELEASE then
			c.braking = false
		end
	elseif c.tc > 0 then
		c.tc = math.max(c.tc - rates.release * dt, 0)
	end
end
