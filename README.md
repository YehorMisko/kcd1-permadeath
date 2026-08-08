# Permadeath for Kingdom Come: Deliverance

Source for a permadeath / adjustable-lives mod for Kingdom Come: Deliverance 1.

Pick how many lives a playthrough gets. Spend them all and that run is over — you get
the game's ending, and the run's saves are moved out of the way so it cannot be
continued.

Download: https://www.nexusmods.com/kingdomcomedeliverance/mods/2326

---

## About the DLL, for anyone checking

Antivirus heuristics dislike this mod, and the reason is worth stating plainly rather
than leaving you to guess.

`LightFX64.dll` is a **proxy DLL**. It ships under the name of a DLL the game already
loads from `Bin\Win64Shared`, and forwards all four of that DLL's exports to the
original, which the installer renames to `LightFX64_orig.dll`. That is how a mod runs
native code in this game, and it is the same technique used by Skyrim's script
extender, ENB, ASI loaders, and the Kingdom Come: Deliverance 2 mod that inspired this
one. It is also, unavoidably, the shape of a DLL sideloading attack — an unsigned DLL
wearing a vendor DLL's name, copied into place by a `.cmd` script. No automated scanner
can tell the two apart. Hence this repository.

Everything the DLL does happens inside the game's own process:

- hooks one function in the game's `WHGame.dll` so the final death shows the ending
  screen instead of the ordinary death screen
- hooks `CreateFileW` / `CreateFileA` / `_wfopen` / `fopen` so that once a run has
  ended, the game can no longer read that run's save folder
- moves finished runs' save folders into an archive folder under
  `Documents\Saved Games\kingdomcome` — **moved, never deleted**
- writes a plain-text log, `permadeath_dll.log`, next to the game executable

It has no network capability of any kind; its only imports are `KERNEL32`, `SHELL32`
and `ole32`. It does not touch the registry, start processes, or write to any process
other than the one it is loaded into.

The game-over hook is installed by **signature scan**, not a hardcoded address: it
searches the single `.text` section of `WHGame.dll` for the function's first 32 bytes
and refuses to patch unless there is exactly one match. Steam, Epic and GOG ship
different builds of `WHGame.dll` at the same game version, and an address read off one
of them is meaningless on the others. `tools/check_build.ps1` runs that same scan
read-only so you can check your own install before trusting the mod with it.

---

## Layout

| path | what it is |
|---|---|
| `dll/permadeath.cpp` | the whole DLL — hooks, archiving, state file |
| `dll/lightfx_forwards.h` | export forwards to the original `LightFX64.dll` |
| `dll/build.bat` | MSVC build; produces `permadeath.dll`, renamed on install |
| `mod/Data/Scripts/Startup/permadeath.lua` | in-game side: death detection, keybinds, console commands |
| `build_loc/` | UI strings — English and Czech |
| `frag/permadeath_superactions.xml` | the keybind declarations this mod adds |
| `tools/check_build.ps1` | read-only build checker (see above) |
| `build_variants.py` | builds the three release variants |

### Building the DLL

```
cd dll
build.bat
```

Needs Visual Studio 2022 with the C++ toolchain; `build.bat` calls `vcvars64.bat` and
compiles with `/LD /EHsc /O2 /std:c++17`. Output is `permadeath.dll`, which the release
installer places as `Bin\Win64Shared\LightFX64.dll`.

### Localization

The Controls-menu labels ship in English and Czech. Each language becomes a
`Localization/<Language>_xml.pak` holding a single `text__<modfolder>.xml`; if that
inner file is not named after the mod folder, the menu silently shows raw `@ui_` keys
instead of text. Each row is `key | English source | text actually shown`, so the
English table repeats itself and the Czech one carries the translation in the third
column. To add a language, drop a table in `build_loc/` and add it to `LOCALIZATIONS`
in `build_variants.py`.

### Building the release variants

`build_variants.py` is included as a record of how the published files were produced.
It will not run as-is against a fresh clone: it expects a local workspace with vanilla
`defaultProfile.xml` and `keybindSuperactions.xml` extracted from your own game install
(and, for the Cheat Mod compatible variant, that mod's versions of the same two files).
Those are not redistributed here — see below.

---

## Not in this repository, on purpose

- **`defaultProfile.xml` and `keybindSuperactions.xml`.** Keybinds in this game cannot
  work without replacing these two vanilla config files, so the released mod ships
  modified copies — but they are Warhorse's files, and the Cheat Mod compatible variant
  merges another modder's copies on top. `frag/permadeath_superactions.xml` is the part
  this mod actually adds, and that is here.
- **Anything from the KCD2 mod credited below.** None of their code, files or assets
  are used in this mod.
- Game binaries, save files, and the reverse-engineering scratch work used to find the
  game-over function.

---

## Credits

Huge thanks to **zhiletka**, author of
[Permanent Death for Kingdom Come: Deliverance 2](https://www.nexusmods.com/kingdomcomedeliverance2/mods/1679).
They published their DLL source alongside their mod, and reading it is what made this
one possible — specifically two ideas:

- KCD's Lua cannot read a file. Their mod gets around it by writing console commands to
  disk and running them with `exec`. That one idea is the entire reason this mod can
  remember your lives across a death and a reload.
- Shipping a mod DLL as a stand-in for one the game already loads from
  `Bin\Win64Shared`, forwarding the original's exports so nothing breaks.

KCD1 needed a fairly different approach in the end — it never logs deaths or which save
you loaded, so the log-watching that works in KCD2 was a dead end here, and the
internals are their own thing. But the map came from them.

To be clear: **none of their code, files or assets are used here.** What I took were
ideas, from source they chose to publish. If they would like anything about this credit
changed, get in touch and I will change it.

---

## Licence

MIT — see [LICENSE](LICENSE).
