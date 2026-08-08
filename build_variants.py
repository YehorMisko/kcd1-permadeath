"""Build the three shippable variants of the permadeath mod into dist/.

Keybinds only fire if the mod replaces two whole vanilla config files
(keybindSuperactions.xml and defaultProfile.xml), and whichever mod loads last
wins -- so the choice of build is really a choice about what to conflict with:

  Permadeath            -- built on the vanilla files, for a normal install
  PermadeathCheatCompat -- built on the Cheat mod's copies, so both work together
  PermadeathConsoleOnly -- ships no config at all, so it conflicts with nothing,
                           at the cost of needing -devmode for the console

Only the base files differ; our own block is injected from the same fragments
every time, so the builds can't drift apart.

Superseded build_defaultprofile.py, which only did the profile half.
"""
import os
import re
import shutil
import sys
import zipfile

ROOT = r'C:\Codestuff\KCD'
EXTRACT = os.path.join(ROOT, 'extracted_lua')
DIST = os.path.join(ROOT, 'dist')

# The live dev tree. Its Lua is the source of truth; its config XMLs are
# regenerated here too so dev and release can't diverge.
DEV = os.path.join(ROOT, 'build_mod', 'PermadeathTest')

SUPERACTIONS_FRAG = os.path.join(ROOT, 'frag', 'permadeath_superactions.xml')

# Actions must also exist in the actionmap named by the superaction's
# map="default", or the Controls entry appears, rebinds, and fires nothing.
# keyboard="_keybinds_ref_" takes the key from the keybinds system.
ACTIONS_FRAG = """\
\t\t<!-- PERMADEATH MOD BEGIN -->
\t\t<action consoleCMD="1" name="permadeath_on" onPress="1" keyboard="_keybinds_ref_"/>
\t\t<action consoleCMD="1" name="permadeath_status" onPress="1" keyboard="_keybinds_ref_"/>
\t\t<action consoleCMD="1" name="permadeath_lives_inc" onPress="1" keyboard="_keybinds_ref_"/>
\t\t<action consoleCMD="1" name="permadeath_lives_dec" onPress="1" keyboard="_keybinds_ref_"/>
\t\t<!-- PERMADEATH MOD END -->
"""

UI_GROUP = '  <ui_group name="permadeath" ui_label="@ui_keybinds_group_permadeath"/>\n'

# Three builds, differing only in how much of Libs/Config they replace.
#
# Keybinds cannot work without replacing BOTH keybindSuperactions.xml (the
# Controls-menu row) and defaultProfile.xml (the actionmap entry that actually
# dispatches). Those are whole-file replacements, so any other mod shipping
# either one conflicts. The console-only build sidesteps that entirely by
# shipping no config at all -- at the cost of needing -devmode for the console.
VARIANTS = {
    'Permadeath': {
        'profile': os.path.join(EXTRACT, 'vanilla_defaultProfile.xml'),
        'keybinds': os.path.join(EXTRACT, 'vanilla_keybindSuperactions.xml'),
        'desc': 'Permadeath / limited lives. Flag a run, and when the lives run out the run is over.',
    },
    'PermadeathCheatCompat': {
        'profile': os.path.join(EXTRACT, 'cheatmod', 'Libs__Config__defaultProfile.xml'),
        'keybinds': os.path.join(EXTRACT, 'cheatmod', 'Libs__Config__keybindSuperactions.xml'),
        'desc': 'Permadeath / limited lives. Cheat-mod-compatible build: keeps the Cheat mod keybinds.',
    },
    'PermadeathConsoleOnly': {
        'profile': None,
        'keybinds': None,
        'desc': 'Permadeath / limited lives. Console-only build: no keybinds, conflicts with nothing.',
    },
}

# Deliberately below 1.0: it works, but it has not been through many hands yet
# and it moves save folders around. Bump to 1.0 once real users have run it.
VERSION = '0.9'

MANIFEST = """<?xml version="1.0" encoding="utf-8"?>
<kcd_mod>
  <info>
    <name>%s</name>
    <description>%s</description>
    <author>Yehor Misko</author>
    <version>%s</version>
    <created_on>Wed Aug 05 2026</created_on>
  </info>
</kcd_mod>
"""


def read(path):
    with open(path, 'r', encoding='utf-8', newline='') as fh:
        return fh.read()


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'w', encoding='utf-8', newline='') as fh:
        fh.write(text)


def inject(text, anchor_re, block, what):
    """Insert block immediately after the line matching anchor_re."""
    m = re.search(anchor_re, text)
    if not m:
        sys.exit('could not find %s anchor' % what)
    nl = '\r\n' if '\r\n' in m.group(0) else '\n'
    if nl == '\r\n':
        block = block.replace('\r\n', '\n').replace('\n', '\r\n')
    return text[:m.end()] + block + text[m.end():]


def build_keybinds(base_path):
    text = read(base_path)
    if 'PERMADEATH MOD BEGIN' in text:
        sys.exit('%s is already patched' % base_path)
    # ui_group declarations sit together near the top; hang ours off the last
    # one so we don't care how many the base has.
    text = inject(text, r'[ \t]*<ui_group [^>]*/>[ \t]*\r?\n(?![\s\S]*?<ui_group )',
                  UI_GROUP, 'ui_group')
    text = inject(text, r'\r?\n(?=</keybinds>)', '\n' + read(SUPERACTIONS_FRAG) + '\n',
                  'end of keybinds')
    return text


def build_profile(base_path):
    text = read(base_path)
    if 'PERMADEATH MOD BEGIN' in text:
        sys.exit('%s is already patched' % base_path)
    return inject(text, r'[ \t]*<actionmap name="default" version="\d+">[ \t]*\r?\n',
                  ACTIONS_FRAG, 'default actionmap')


# Our script is deliberately NOT called main.lua: that path is shared with the
# Cheat mod (and any other mod using it), and the merged VFS is
# last-mounted-wins with nothing logged, so one of them silently loses.
# Confirmed in-game that the engine loads every .lua in Scripts/Startup, so a
# unique name sidesteps the clash and dev and release can be identical.
SCRIPT_NAME = 'permadeath.lua'

# <Language>_xml.pak -> the source table in build_loc/. Each row is
# key | English source | text actually shown, so the English table carries the
# same string twice and the Czech one carries the translation in column 3.
LOCALIZATIONS = {
    'English': 'text__permadeathtest.xml',
    'Czech':   'text__permadeathtest_czech.xml',
}

# Every command the mod registers, plus the table itself. Cheap insurance
# against shipping a mod that loads and does nothing.
LUA_REQUIRED = [
    'Permadeath = {}',
    'Permadeath.execFile',
    'function Permadeath:onDeath',
    'function Permadeath:poll',
    'UIAction.RegisterActionListener',
    '"permadeath_on"', '"permadeath_off"', '"permadeath_lives"',
    '"permadeath_status"', '"permadeath_lives_inc"', '"permadeath_lives_dec"',
    '"permadeath_sync"',
]


def release_lua(cfg):
    text = read(os.path.join(DEV, 'Data', 'Scripts', 'Startup', SCRIPT_NAME))

    # Prompts differ by build: "press the permadeath key" is wrong advice in a
    # build that ships no keys.
    if not cfg['keybinds']:
        text, n = re.subn(r'Permadeath\.hasKeybinds = true',
                          'Permadeath.hasKeybinds = false', text)
        if n != 1:
            sys.exit('expected exactly 1 hasKeybinds assignment, found %d' % n)

    # Comments may still mention the Cheat mod (that is where player:IsDead()
    # was found); what must not survive is executable code depending on it.
    code = re.sub(r'--\[\[[\s\S]*?\]\]', '', text)
    code = re.sub(r'--[^\n]*', '', code)
    if 'cheat' in code.lower():
        sys.exit('%s has code referencing the Cheat mod' % SCRIPT_NAME)

    missing = [k for k in LUA_REQUIRED if k not in text]
    if missing:
        sys.exit('%s is missing: %s' % (SCRIPT_NAME, ', '.join(missing)))

    return text


# The mod loads by standing in for LightFX64.dll (an AlienFX lighting SDK the
# game loads at startup and does not need) and forwarding every export to the
# renamed original, so nothing is lost.
#
# The one genuinely dangerous case is running this twice: a naive version
# would rename OUR dll to LightFX64_orig.dll, destroy the real one and leave
# the proxy forwarding to itself. Hence the LightFX64_orig.dll guard.
INSTALLER = r"""@echo off
setlocal
set BIN=%~dp0Bin\Win64Shared

echo.
echo   Kingdom Come: Deliverance -- Permadeath
echo   ---------------------------------------
echo.

if not exist "%BIN%" (
  echo   ERROR: no Bin\Win64Shared folder next to this script.
  echo.
  echo   Copy the whole contents of this download into your
  echo   KingdomComeDeliverance folder ^(the one containing Bin and Mods^),
  echo   then run this script from there.
  goto :end
)

if not exist "%~dp0_dll\LightFX64.dll" (
  echo   ERROR: _dll\LightFX64.dll is missing from this download.
  goto :end
)

if exist "%BIN%\LightFX64_orig.dll" (
  echo   Already installed - updating the mod DLL only.
  goto :copy
)

if not exist "%BIN%\LightFX64.dll" (
  echo   ERROR: "%BIN%\LightFX64.dll" not found.
  echo   The mod loads by standing in for that file, so it must exist.
  echo   Verify the game files through Steam, then run this again.
  goto :end
)

echo   Backing up LightFX64.dll to LightFX64_orig.dll ...
ren "%BIN%\LightFX64.dll" LightFX64_orig.dll
if errorlevel 1 (
  echo   ERROR: could not rename it. Close the game and try again.
  goto :end
)

:copy
copy /y "%~dp0_dll\LightFX64.dll" "%BIN%\LightFX64.dll" >nul
if errorlevel 1 (
  echo   ERROR: could not copy the mod DLL. Close the game and try again.
  goto :end
)

rem --- the mods_old sweep -------------------------------------------------
rem A missing mod_status.xml means the game has never been launched since
rem patch 1.9. It reads that as "updated from an older KCD", moves everything
rem in Mods\ to mods_old\ once, and writes the file so it never repeats. A mod
rem installed before that first launch gets swept with it -- the DLL survives
rem (different folder), so the mod looks installed but no script ever runs.
rem Both halves below do nothing on an install that has already been launched.

if not exist "%~dp0mods_old\{MODDIR}" goto :status
if exist "%~dp0Mods\{MODDIR}" goto :status
echo   Found {MODDIR} in mods_old - moving it back into Mods ...
if not exist "%~dp0Mods" mkdir "%~dp0Mods"
move "%~dp0mods_old\{MODDIR}" "%~dp0Mods\" >nul

:status
if exist "%~dp0mod_status.xml" goto :installed
echo   This game install has never been launched. Writing mod_status.xml so
echo   the game does not move Mods\ into mods_old\ the first time it starts.
>"%~dp0mod_status.xml" echo ^<ModStatus^>
>>"%~dp0mod_status.xml" echo   ^<modFolderWasMoved^>true^</modFolderWasMoved^>
>>"%~dp0mod_status.xml" echo   ^<playerWasNotified^>true^</playerWasNotified^>
>>"%~dp0mod_status.xml" echo ^</ModStatus^>

:installed
echo.
echo   Installed.
echo.
echo   Launch the game, load or start a run, and press NUMPAD .
echo   to flag that run as permadeath. See README.txt for the rest.

:end
echo.
pause
"""

UNINSTALLER = r"""@echo off
setlocal
set BIN=%~dp0Bin\Win64Shared

echo.
echo   Removing the Permadeath DLL
echo.

if not exist "%BIN%\LightFX64_orig.dll" (
  echo   Nothing to undo - LightFX64_orig.dll not found.
  goto :end
)

del "%BIN%\LightFX64.dll" 2>nul
ren "%BIN%\LightFX64_orig.dll" LightFX64.dll
if errorlevel 1 (
  echo   ERROR: could not restore it. Close the game and try again.
  goto :end
)

echo   Original LightFX64.dll restored.
echo.
echo   Also delete the Mods folder for this mod if you want it fully gone.
echo   Your saves are untouched. Any archived runs stay in
echo   Documents\Saved Games\kingdomcome\saves_permadeath_archive.

:end
echo.
pause
"""


README = """Permadeath for Kingdom Come: Deliverance
=======================================

Flag a run as permadeath and give it however many lives you like. Spend them
all and the run is over: you get the game-over ending, and the run's saves are
moved out of the way so you cannot load it again.

Your saves are never deleted. A finished run is MOVED to
    Documents\\Saved Games\\kingdomcome\\saves_permadeath_archive\\
so if you ever want it back, move the folder back into saves\\.


INSTALLING
----------
1. Close the game.
2. Copy everything in this download into your KingdomComeDeliverance folder
   (the one containing Bin\\ and Mods\\).
3. Run Install-Permadeath.cmd.

That last step swaps in a DLL the game already loads, which is how the mod
gets to run native code. Uninstall-Permadeath.cmd puts the original back.

Run the installer even if you copied the files in by hand -- besides the DLL
it also deals with the mods_old problem described below.


THE KEYS AND MESSAGES DO NOTHING
--------------------------------
Almost always this: the game moved your Mods folder away.

The first time a KCD install is launched after patch 1.9, the game assumes you
have updated from an older version, moves everything in Mods\ into mods_old\,
and tells you so. It only ever does this once. If you installed mods before
that first launch, they go with it -- and because the mod's DLL lives
somewhere else it still loads, so the mod looks installed while no script of
it is running. Nothing counts your deaths and nothing ever happens at 0 lives.

Install-Permadeath.cmd now prevents this, and moves the mod back if it has
already happened. To check by hand: look for a mods_old folder next to Mods,
and look in kcd.log for

    [Mod] 0 mods loaded from mods/

If you see that line, the game loaded no mods at all. Move the folders from
mods_old back into Mods and launch again.


USING IT
--------
{KEYS}
Set the number of lives FIRST, then flag the run. Flagging locks the count in,
so a bad death is not one keypress away from being undone. You only ever flag
a run once -- it is remembered for that save, across quits and restarts.

{MASHING}
IF YOU HIT CONTINUE ON THE LAST DEATH you get thrown back to the main menu.
That is intended, not a bug. The run closes the instant the last life is
spent and its saves stop loading from that moment, so even if you are quick
enough to reach the Continue button it will not take you anywhere.

WHEN TO TURN IT ON - whenever you like. A new game or a playthrough you are
already forty hours into, both work the same. Flag the run whenever you decide
you want it and it applies from that moment on.

If you flag a long-running save, know that when the run ends the WHOLE
playline is archived - every save belonging to that playthrough, including the
ones from before you flagged it. Nothing is deleted and you can move the
folder back, but it is worth knowing first.

Works in both normal and Hardcore Mode.

Console commands (the console needs -devmode in the launch options):

    permadeath_on            flag this run (run it TWICE to turn it off)
    permadeath_off           turn permadeath off for this run, no confirmation
    permadeath_lives <n>     set lives (works even after flagging)
    permadeath_status        show whether this run is permadeath, and lives

Turning permadeath off takes two goes on purpose. permadeath_on on a run that
is already flagged only warns you; run it again within about four seconds and
it turns off. In the console, run it once then press Up and Enter. Or use
permadeath_off, which does it in one.


WHICH BUILD TO USE
------------------
{CONFLICT}
"""

KEYS_BOUND = """Default keys (rebindable under Controls -> Permadeath):

    NUMPAD .        flag this run as permadeath / press twice to turn off
    NUMPAD +        add a life        (before you flag the run)
    NUMPAD -        remove a life     (before you flag the run)
    F9              show lives remaining
"""

KEYS_CONSOLE = """This build has no keybinds at all -- everything is done from the console,
which means you must add  -devmode  to the game's launch options, then press
the ~ key in game to open it.
"""

# Only the keybind builds can be mashed into doing the wrong thing: the flag
# key doubles as the disarm confirmation, and arming takes ~3s to confirm. In
# the console the same double-run is deliberate and documented further down,
# so warning about it there would just be confusing.
MASHING_BOUND = """ONE PRESS AT A TIME - do not mash the keys. Press once, wait for the message
to appear and clear, then press again if you need to. This matters most for
the flag key: the run is flagged the instant you press it, but the
confirmation takes a few seconds to come up, and in the meantime that same key
has become the "turn it off" confirmation. Tapping it twice flags the run and
then offers to unflag it; a third tap unflags it. Press once and wait.
"""

MASHING_CONSOLE = """ONE COMMAND AT A TIME - run permadeath_on once and wait for the message before
running anything else. Flagging a run takes a few seconds to confirm, and
running it again in the meantime is read as "turn it off" (see below).
"""

CONFLICT_NOTES = {
    'Permadeath': """This is the standard build. It replaces two whole vanilla config files,
Libs\\Config\\keybindSuperactions.xml and Libs\\Config\\defaultProfile.xml,
because a keybind will not fire unless it is declared in both.

Any other mod shipping either file will conflict -- whichever loads last wins.

  * Using the Cheat mod?      Use the PermadeathCheatCompat build instead.
  * Using some other mod
    that changes keybinds?    Use the PermadeathConsoleOnly build instead.""",

    'PermadeathCheatCompat': """This is the Cheat-mod-compatible build. Its copies of
Libs\\Config\\keybindSuperactions.xml and Libs\\Config\\defaultProfile.xml
already contain the Cheat mod's own entries, so install this INSTEAD of the
standard build and both mods keep their keybinds.

Install only ONE Permadeath build. If you also use a third mod that changes
keybinds, use the PermadeathConsoleOnly build instead.""",

    'PermadeathConsoleOnly': """This build ships no config files at all -- only the script and the DLL -- so
it cannot conflict with any other mod's keybinds. That is the trade-off: no
keys, console only, and the console needs -devmode.

If you do want keybinds, use the standard Permadeath build, or the
PermadeathCheatCompat build if you also run the Cheat mod.""",
}


def readme(name, cfg):
    bound = bool(cfg['keybinds'])
    out = (README
           .replace('{KEYS}', KEYS_BOUND if bound else KEYS_CONSOLE)
           .replace('{MASHING}', MASHING_BOUND if bound else MASHING_CONSOLE)
           .replace('{CONFLICT}', CONFLICT_NOTES[name]))
    if '{' in out.replace('{n}', ''):
        raise SystemExit('readme(%s): unsubstituted placeholder left in template' % name)
    return out


def build_variant(name, cfg):
    out = os.path.join(DIST, name, 'Mods', name)
    if os.path.exists(os.path.join(DIST, name)):
        shutil.rmtree(os.path.join(DIST, name))

    write(os.path.join(out, 'mod.manifest'), MANIFEST % (name, cfg['desc'], VERSION))
    if cfg['keybinds']:
        write(os.path.join(out, 'Data', 'Libs', 'Config', 'keybindSuperactions.xml'),
              build_keybinds(cfg['keybinds']))
        write(os.path.join(out, 'Data', 'Libs', 'Config', 'defaultProfile.xml'),
              build_profile(cfg['profile']))
    write(os.path.join(out, "Data", "Scripts", "Startup", SCRIPT_NAME), release_lua(cfg))

    # --- pack Data/ into data.pak (store-mode zip, as the game expects) ----
    data_root = os.path.join(out, 'Data')
    pak = os.path.join(out, 'Data', 'data.pak')
    entries = []
    for root, _, files in os.walk(data_root):
        for f in files:
            full = os.path.join(root, f)
            entries.append((full, os.path.relpath(full, data_root).replace('\\', '/')))
    with zipfile.ZipFile(pak + '.tmp', 'w', zipfile.ZIP_STORED) as z:
        for full, arc in entries:
            z.write(full, arc)
    for sub in ('Libs', 'Scripts'):
        shutil.rmtree(os.path.join(data_root, sub), ignore_errors=True)
    os.replace(pak + '.tmp', pak)

    # Localization only supplies the Controls-menu labels, so the console-only
    # build has no use for it. The XML inside each <Language>_xml.pak must be
    # named after the mod folder, or the menu silently shows raw @ui_ keys.
    if cfg['keybinds']:
        os.makedirs(os.path.join(out, 'Localization'), exist_ok=True)
        for lang, src in LOCALIZATIONS.items():
            loc = read(os.path.join(ROOT, 'build_loc', src))
            loc_xml = os.path.join(out, 'text__%s.xml' % name.lower())
            write(loc_xml, loc)
            with zipfile.ZipFile(os.path.join(out, 'Localization', '%s_xml.pak' % lang),
                                 'w', zipfile.ZIP_STORED) as z:
                z.write(loc_xml, os.path.basename(loc_xml))
            os.remove(loc_xml)

    # --- the rest of the download ------------------------------------------
    root_dir = os.path.join(DIST, name)
    dll = os.path.join(ROOT, 'dll', 'permadeath.dll')
    if os.path.exists(dll):
        os.makedirs(os.path.join(root_dir, '_dll'), exist_ok=True)
        shutil.copy(dll, os.path.join(root_dir, '_dll', 'LightFX64.dll'))
    else:
        print('  WARNING: dll/permadeath.dll missing -- build it first')

    write(os.path.join(root_dir, 'Install-Permadeath.cmd'),
          INSTALLER.replace('{MODDIR}', name))
    write(os.path.join(root_dir, 'Uninstall-Permadeath.cmd'), UNINSTALLER)
    write(os.path.join(root_dir, 'README.txt'), readme(name, cfg))

    print('built %s (%d files in data.pak)' % (root_dir, len(entries)))


def refresh_dev():
    """Keep the dev tree's config XMLs generated from the same fragments."""
    cfg = VARIANTS['PermadeathCheatCompat']
    write(os.path.join(DEV, 'Data', 'Libs', 'Config', 'keybindSuperactions.xml'),
          build_keybinds(cfg['keybinds']))
    write(os.path.join(DEV, 'Data', 'Libs', 'Config', 'defaultProfile.xml'),
          build_profile(cfg['profile']))
    print('refreshed dev config XMLs in %s' % DEV)


if __name__ == '__main__':
    for name, cfg in VARIANTS.items():
        build_variant(name, cfg)
    refresh_dev()
