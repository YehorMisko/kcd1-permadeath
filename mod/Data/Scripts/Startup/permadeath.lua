-- This file must NOT be called main.lua. Scripts/Startup/main.lua is a single
-- shared path -- the Cheat mod ships one too -- and the merged mod VFS is
-- last-mounted-wins with nothing logged, so whichever mod loses is silently
-- disabled. Verified in-game that the engine loads EVERY .lua in
-- Scripts/Startup, not just main.lua, so a unique name avoids the clash
-- outright and no chain-loading of the other mod is needed.
System.LogAlways("PERMADEATH: permadeath.lua loaded")

Permadeath = {}
Permadeath.pollMs = 200
Permadeath.wasDead = false
Permadeath.timerId = nil
Permadeath.armed = false
Permadeath.lives = 1
-- Written by the external watcher into the game root, pulled in via the
-- console's `exec` command -- this is the only disk->Lua channel we have
-- (Lua has no `io`), and it's how lives survive a death-reload.
Permadeath.execFile = ".permadeath.cfg"
-- Extra attempts after the immediate one, in case the first fires slightly
-- too early (mid death-animation) to take.
Permadeath.gameOverRetriesMs = {300, 800, 1500}
-- Last resort if the game-over screen doesn't take: boot to the main menu.
-- Long enough to let the death play out and the screen appear if it will.
Permadeath.forceMenuMs = 6000
-- Ask the DLL which playline was loaded, so a run flagged for permadeath
-- re-arms itself instead of the player retyping permadeath_on every load.
-- Costs nothing: the DLL learns it from the game's own save-file reads.
Permadeath.autoIdentify = true
Permadeath.identifyDelayMs = 2000

-- Game.SendInfoText renders raw strings on the HUD (confirmed live).
-- Game.ShowTutorial takes raw text too but draws nothing in retail builds,
-- and Game.ShowNotification wants a localization key -- so this is the one.
function Permadeath:say(msg)
	Game.SendInfoText(msg)
	System.LogAlways("PERMADEATH_UI: " .. msg)
end

-- The same script ships in every build, but only some of them have keys, so
-- prompts must not hardcode one input method. build_variants.py flips this to
-- false for the console-only variant.
Permadeath.hasKeybinds = true

function Permadeath:armHint()
	if self.hasKeybinds then
		return "press the permadeath key"
	end
	return "type permadeath_on"
end

-- ============================================================================
-- console commands
-- ============================================================================

-- Turning permadeath OFF needs a deliberate double press. A single stray
-- keypress silently removing the stakes is the worst failure this mod has,
-- and the player might not even notice -- but arming the wrong playline by
-- mistake still has to be undoable.
Permadeath.disarmConfirmMs = 4000
Permadeath.disarmPending = false

function Permadeath:cmdOn()
	if self.armed then
		if self.disarmPending then
			self.disarmPending = false
			self:cmdOff()
		else
			self.disarmPending = true
			self:say("Permadeath is ON - press again to turn it OFF")
			Script.SetTimer(self.disarmConfirmMs, function()
				if Permadeath.disarmPending then
					Permadeath.disarmPending = false
					Permadeath:say("Permadeath stays ON")
				end
			end)
		end
		return
	end

	-- Force a save. Nothing on disk records which playline is active, and
	-- KCD1 (unlike KCD2) never logs it on load -- but whichever playline
	-- directory just got written to is unambiguously the live one. That's
	-- how the watcher identifies the target without guessing.
	Game.RemoveSaveLock()
	if Game.IsLoadingEngineSaveGame() then
		self:say("Permadeath: still loading, try again in a moment")
		return
	end
	self.armed = true
	Game.SaveGameViaResting()
	-- Flags this playline as a permadeath run. The watcher remembers it, so
	-- from now on the run re-arms itself automatically on every load and
	-- this only ever needs typing once, when starting the run.
	System.LogAlways("PERMADEATH_ARM: armed - save written to identify playline")
	Script.SetTimer(3000, function()
		System.ExecuteCommand("exec " .. Permadeath.execFile)
		Permadeath:say("Permadeath ON - " .. tostring(Permadeath.lives) .. " lives remaining")
		System.LogAlways("[Permadeath] Lives left: " .. tostring(Permadeath.lives))
	end)
end

-- Persistent, not just for this session: the DLL drops the playline from its
-- state file when it sees this marker, so the run stays ordinary after a
-- reload too. Lives become editable again.
function Permadeath:cmdOff()
	self.armed = false
	self.disarmPending = false
	System.LogAlways("PERMADEATH_DISARM: permadeath removed from this run")
	self:say("Permadeath OFF - this run is no longer permadeath")
end

function Permadeath:cmdLives(n)
	local v = tonumber(n)
	if not v or v < 1 or v > self.livesMax then
		self:say("Usage: permadeath_lives <1-" .. tostring(self.livesMax) .. ">")
		return
	end
	self.lives = math.floor(v)
	System.LogAlways("[Permadeath] Lives left: " .. tostring(self.lives))
	if self.armed then
		self:say("Lives overridden to " .. tostring(self.lives) .. " for this run")
	else
		self:say("Permadeath lives set to " .. tostring(self.lives))
	end
end

-- Called by the watcher through the exec file, not usually by hand.
function Permadeath:cmdSync(n)
	local v = tonumber(n)
	if v then
		self.lives = math.floor(v)
		self.armed = true
		System.LogAlways("PERMADEATH_SYNC: lives restored to " .. tostring(self.lives))
	end
end

-- A key can't pass an argument the way `permadeath_lives 3` can, so stepping
-- up and down gets a key each. Any count is reachable this way, rather than
-- the fixed 1/2/3/5 cycle this replaced.
Permadeath.livesMax = 99

-- Never step down to 0: the DLL reads "Lives left: N" as authoritative and
-- archives the run at 0, so only a real death may ever emit that.
function Permadeath:cmdLivesAdjust(delta)
	-- Arming the run is the moment of commitment, so it locks the life count.
	-- Otherwise a bad death is always one keypress away from being undone,
	-- and the stakes the mod exists to create quietly evaporate. Typing
	-- `permadeath_lives <n>` still works -- that is a deliberate act rather
	-- than a frustrated key-mash.
	if self.armed then
		self:say("Lives locked at " .. tostring(self.lives) .. " for this run")
		return
	end

	local v = self.lives + delta
	if v < 1 then
		v = 1
	elseif v > self.livesMax then
		v = self.livesMax
	end
	if v == self.lives then
		self:say("Lives: " .. tostring(self.lives) .. " (limit reached)")
		return
	end
	self.lives = v
	System.LogAlways("[Permadeath] Lives left: " .. tostring(self.lives))
	if self.armed then
		self:say("Lives for this run: " .. tostring(self.lives))
	else
		self:say("Lives set to " .. tostring(self.lives) .. " - " ..
			self:armHint() .. " to start the run")
	end
end

function Permadeath:cmdStatus()
	if self.armed then
		self:say("Permadeath ON - lives remaining: " .. tostring(self.lives))
	else
		-- Tell the player what to do next, not just that it is off: pick the
		-- number of lives first, then commit, because arming locks it in.
		self:say("Permadeath OFF - " .. tostring(self.lives) .. " lives selected. " ..
			"Set the lives you want, then " .. self:armHint() .. " to start the run.")
	end
	System.LogAlways("PERMADEATH_STATUS: armed is " .. tostring(self.armed) ..
		" lives is " .. tostring(self.lives))
end

System.AddCCommand("permadeath_on", "Permadeath:cmdOn()",
	"Arm permadeath for the run you are currently playing.")
System.AddCCommand("permadeath_off", "Permadeath:cmdOff()",
	"Disarm permadeath for this session.")
System.AddCCommand("permadeath_lives", "Permadeath:cmdLives(%line)",
	"Set how many lives this run gets, e.g. permadeath_lives 3")
System.AddCCommand("permadeath_status", "Permadeath:cmdStatus()",
	"Show whether permadeath is armed and how many lives remain.")
System.AddCCommand("permadeath_lives_inc", "Permadeath:cmdLivesAdjust(1)",
	"Add one life to this run. Bound to a key by default.")
System.AddCCommand("permadeath_lives_dec", "Permadeath:cmdLivesAdjust(-1)",
	"Remove one life from this run (never below 1). Bound to a key by default.")
System.AddCCommand("permadeath_sync", "Permadeath:cmdSync(%line)",
	"Internal: used by the DLL to restore lives after a reload.")

-- ============================================================================
-- death detection
-- ============================================================================

-- wh_pl_GameOverTest is a CVAR, not a command: writing 1 when it already
-- holds 1 changes nothing, so reset to 0 first for a real 0->1 transition.
--
-- More importantly it only takes effect while the player is ALIVE. Evidence:
-- it worked typed by hand while healthy, and worked on an instant console
-- kill (caught before the death sequence began), but never on a slow death
-- like fall damage. So rather than racing the death, cancel it -- put health
-- back first, then fire while the player is technically still alive.
function Permadeath:triggerGameOver()
	if player and player.soul then
		player.soul:SetState("health", 100)
	end
	System.ExecuteCommand("wh_pl_GameOverTest 0")
	System.ExecuteCommand("wh_pl_GameOverTest 1")
end

function Permadeath:onDeath()
	self.lives = self.lives - 1
	if self.lives < 0 then
		self.lives = 0
	end

	-- The watcher reads this line: it decrements its own counter, persists
	-- it, and archives the run when it reaches zero.
	System.LogAlways("[Permadeath] Lives left: " .. tostring(self.lives))

	if self.lives > 0 then
		self:say("YOU DIED - lives remaining: " .. tostring(self.lives))
		return
	end

	self:say("YOUR RUN IS OVER - no lives remaining")
	self:triggerGameOver()
	for _, delay in ipairs(self.gameOverRetriesMs) do
		Script.SetTimer(delay, function() Permadeath:triggerGameOver() end)
	end
	-- The game-over screen has proven unreliable on a real death, so fall
	-- back to kicking the player out to the main menu. Their saves are
	-- archived by now, so there is nothing left to load anyway.
	Script.SetTimer(self.forceMenuMs, function()
		System.LogAlways("PERMADEATH: forcing main menu")
		System.ExecuteCommand("wh_sys_ForceMainMenu 0")
		System.ExecuteCommand("wh_sys_ForceMainMenu 1")
	end)
end

-- player:IsDead() is a real entity-level API (confirmed live, and used by
-- the Cheat mod's cheat_resurrect_npc). Polling it is far more reliable
-- than the Lua hooks we tried before: SinglePlayer:ProcessActorDamage and
-- the HitDeathReactions:* functions are both dead code for the player.
function Permadeath:poll()
	if player and player.IsDead then
		-- health<=0 goes true before IsDead() does on drawn-out deaths, and
		-- that earlier moment is the only window where the game-over screen
		-- still takes effect.
		local dead = player:IsDead()
		if not dead and player.soul then
			local hp = player.soul:GetState("health")
			if hp and hp <= 0 then
				dead = true
			end
		end
		if dead and not self.wasDead then
			self.wasDead = true
			if self.armed then
				System.LogAlways("PERMADEATH_EVENT: player died")
				self:onDeath()
			else
				System.LogAlways("PERMADEATH: player died (not armed, ignoring)")
			end
		elseif not dead and self.wasDead then
			self.wasDead = false
		end
	end
	self.timerId = Script.SetTimer(self.pollMs, function() Permadeath:poll() end)
end

function Permadeath:start()
	if self.timerId then
		Script.KillTimer(self.timerId)
	end
	self.wasDead = false
	self.armed = false
	-- Pull state back from the watcher. If the file exists it contains a
	-- permadeath_sync line, which re-arms us and restores the life count
	-- for this run; if not, we simply stay disarmed.
	self.timerId = Script.SetTimer(self.pollMs, function() Permadeath:poll() end)
	System.LogAlways("PERMADEATH: death poll started")

	-- Work out which playline this is, so permadeath only re-arms itself for
	-- runs the player actually flagged. KCD1 never logs which save was
	-- loaded, so the only way to find out is to write one and see which
	-- directory changes -- hence the forced save. Without this the player
	-- would have to type permadeath_on after every single load.
	if self.autoIdentify then
		Script.SetTimer(self.identifyDelayMs, function() Permadeath:identify() end)
	end
end

-- No longer writes a save. The DLL watches the game's own file opens, so by
-- the time a load finishes it already knows which playline was read -- this
-- just tells it the load is done and asks for the answer.
function Permadeath:identify()
	if Game.IsLoadingEngineSaveGame() then
		Script.SetTimer(2000, function() Permadeath:identify() end)
		return
	end
	System.LogAlways("PERMADEATH_IDENTIFY: asking DLL which playline was loaded")
	-- Give the DLL a moment to see the line and reply with this run's state
	-- (or nothing, if it isn't a permadeath run).
	Script.SetTimer(1500, function()
		System.ExecuteCommand("exec " .. Permadeath.execFile)
		if Permadeath.armed then
			Permadeath:say("Permadeath: " .. tostring(Permadeath.lives) .. " lives remaining")
		end
	end)
end

-- Gameplay globals (player, etc.) don't exist when Startup scripts first
-- run at boot; this event fires once the loading screen finishes.
function Permadeath:uiActionListener(actionName, eventName, argTable)
	if actionName == "sys_loadingimagescreen" and eventName == "OnEnd" then
		Permadeath:start()
	end
end

UIAction.RegisterActionListener(Permadeath, "", "", "uiActionListener")
