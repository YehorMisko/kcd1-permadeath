// Permadeath for Kingdom Come: Deliverance 1 -- in-process half.
//
// Ships as Bin\Win64Shared\LightFX64.dll, forwarding every AlienFX export to
// the renamed original (LightFX64_orig.dll), so the game loads us for free
// and the real SDK keeps working.
//
// Pairs with the Lua mod (Mods/Permadeath), which detects death and talks to
// us through kcd.log. We talk back by writing console commands into
// <gameroot>\.permadeath.cfg, which the mod runs via `exec` -- the only
// disk->Lua channel available, since KCD's Lua has no io module.
//
// Responsibilities (ported from permadeath_watch.py):
//   * tail kcd.log for the mod's markers
//   * work out which playline is live, from which save directory changes
//   * delete the identify-save the mod writes purely to reveal that
//   * remember lives per playline, across the reload that follows a death
//   * archive the playline when lives run out (MOVE, never delete)

// windows.h defines min/max as macros, which breaks std::  ::min() calls
// such as file_time_type::min(). Must come before the include.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

#include "lightfx_forwards.h"

#pragma comment(lib, "shell32.lib")   // SHGetKnownFolderPath
#pragma comment(lib, "ole32.lib")     // CoTaskMemFree

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
namespace {

constexpr wchar_t kExecFile[]    = L".permadeath.cfg";
constexpr wchar_t kStateFile[]   = L".permadeath_state.cfg";
constexpr wchar_t kDebugLog[]    = L"permadeath_dll.log";
constexpr wchar_t kArchiveDir[]  = L"saves_permadeath_archive";

constexpr char kMarkerIdentify[] = "PERMADEATH_IDENTIFY";
constexpr char kMarkerArm[]      = "PERMADEATH_ARM: armed";
constexpr char kMarkerDisarm[]   = "PERMADEATH_DISARM";
constexpr char kMarkerLives[]    = "[Permadeath] Lives left: ";

constexpr int kDefaultLives = 1;

std::atomic_bool g_running{false};
std::thread      g_thread;
fs::path         g_game_root;   // holds kcd.log
fs::path         g_saves;       // Saved Games\kingdomcome\saves
fs::path         g_archive;

// live state
std::map<std::wstring, int> g_playlines;   // playline dir -> lives left
std::wstring                g_target;      // playline currently armed
int                         g_last_lives = 0;
std::atomic_bool            g_archive_pending{false};  // set by the game-over hook

// Set the moment a run is archived because its lives ran out. The game-over
// hook consumes it to still show "The End" when the Lua side got there first
// -- see should_force_the_end() and end_grace_consumed(). 0 means "no run has
// just ended". Cleared on arm/disarm so a fresh run can never inherit it.
std::atomic<long long> g_ended_at_ms{0};
constexpr long long    kEndGraceMs = 30000;

long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Which playline the game itself has been touching, learned by watching its
// file opens (see the IAT hooks below). This is what lets a load identify the
// playline without the mod having to write a throwaway save every time.
std::mutex   g_seen_mtx;
std::wstring g_seen_read;    // last playline opened at all
std::wstring g_seen_write;   // last playline opened for writing

// Runs whose lives are gone but which we could not move, because the game was
// holding a save open -- i.e. the player hit Continue and beat us to it. Value
// is the folder's newest mtime at that moment, used to tell the abandoned run
// apart from a brand-new game that later reused the same playline slot.
// Archived at the next init, before the game has opened anything.
std::map<std::wstring, long long> g_pending;

// ---------------------------------------------------------------------------
void debug_log(const std::string& msg)
{
    std::ofstream f(g_game_root / kDebugLog, std::ios::app);
    if (!f.is_open())
        return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    char stamp[32];
    sprintf_s(stamp, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    f << stamp << msg << std::endl;
}

std::string narrow(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// ---------------------------------------------------------------------------
// Watching the game's own file opens for a "playlineN" path component.
//
// Runs on the game's threads, so it must stay cheap and must never log or
// allocate more than necessary -- a scan for one substring per open.
template <typename C>
std::wstring note_path(const C* path, bool writing)
{
    if (!path)
        return {};

    static const char kNeedle[] = "playline";
    constexpr size_t kNeedleLen = sizeof(kNeedle) - 1;

    for (const C* p = path; *p; ++p)
    {
        size_t i = 0;
        while (i < kNeedleLen && p[i] &&
               (char)(p[i] | 0x20) == kNeedle[i])   // ASCII-only lowercase
            ++i;
        if (i < kNeedleLen)
            continue;

        const C* d = p + kNeedleLen;
        if (*d < '0' || *d > '9')      // "playline" must be followed by its number
            continue;

        std::wstring name(kNeedle, kNeedle + kNeedleLen);
        while (*d >= '0' && *d <= '9')
            name.push_back((wchar_t)*d++);

        {
            std::lock_guard<std::mutex> lk(g_seen_mtx);
            g_seen_read = name;
            if (writing)
                g_seen_write = name;
        }
        return name;
    }
    return {};
}

// --- sealing a finished run ------------------------------------------------
// The run is over the instant the last life is spent, so from that instant its
// saves must not open. Moving the folder was never enough on its own: Windows
// will not rename a directory while a file inside it is open, so if the player
// hits Continue first the rename can only succeed AFTER their load finishes --
// which is how a player on GOG ended up mid-session in a run whose saves had
// just been archived.
//
// Refusing the open removes the race instead of trying to win it. Continue
// fails with a load error, every time, on any drive, whoever gets there first.
// The error is the point: the run is over, and it should not be loadable.
std::mutex   g_sealed_mtx;
std::wstring g_sealed;

void seal_playline(const std::wstring& playline)
{
    std::lock_guard<std::mutex> lk(g_sealed_mtx);
    g_sealed = playline;
}

void unseal_playline()
{
    std::lock_guard<std::mutex> lk(g_sealed_mtx);
    g_sealed.clear();
}

// Runs on the game's threads, on every file open, so keep it to a compare.
bool is_sealed(const std::wstring& playline)
{
    if (playline.empty())
        return false;
    std::lock_guard<std::mutex> lk(g_sealed_mtx);
    return !g_sealed.empty() && g_sealed == playline;
}

// Prefer a write: the game only ever writes saves into the playline actually
// being played, whereas the Load Game menu *reads* from all of them while it
// builds the list. Reads still work as a fallback because the save being
// loaded is opened after that enumeration finishes.
std::wstring take_observed_playline()
{
    std::lock_guard<std::mutex> lk(g_seen_mtx);
    std::wstring out = !g_seen_write.empty() ? g_seen_write : g_seen_read;
    // Consume it, so a stale sighting can never be inherited by the next load
    // -- notably by a brand-new game, which must NOT pick up a playline the
    // menu happened to touch on the way in.
    g_seen_read.clear();
    g_seen_write.clear();
    return out;
}

// --- IAT hooks -------------------------------------------------------------
// Patching the import table is just a pointer swap, so unlike the inline hook
// further down there is no instruction rewriting to get wrong. We hook every
// file-open entry point WHGame.dll imports rather than guessing which one the
// save code path uses.
using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileA_t = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using wfopen_t      = FILE*(__cdecl*)(const wchar_t*, const wchar_t*);
using fopen_t       = FILE*(__cdecl*)(const char*, const char*);

CreateFileW_t g_orig_CreateFileW = nullptr;
CreateFileA_t g_orig_CreateFileA = nullptr;
wfopen_t      g_orig_wfopen      = nullptr;
fopen_t       g_orig_fopen       = nullptr;

template <typename C>
bool mode_is_write(const C* mode)
{
    for (const C* m = mode; m && *m; ++m)
        if (*m == 'w' || *m == 'a' || *m == '+')
            return true;
    return false;
}

HANDLE WINAPI hook_CreateFileW(LPCWSTR name, DWORD access, DWORD share,
                               LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (is_sealed(note_path(name, (access & GENERIC_WRITE) != 0)))
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return g_orig_CreateFileW(name, access, share, sa, disp, flags, tmpl);
}

HANDLE WINAPI hook_CreateFileA(LPCSTR name, DWORD access, DWORD share,
                               LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags, HANDLE tmpl)
{
    if (is_sealed(note_path(name, (access & GENERIC_WRITE) != 0)))
    {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
    return g_orig_CreateFileA(name, access, share, sa, disp, flags, tmpl);
}

FILE* __cdecl hook_wfopen(const wchar_t* name, const wchar_t* mode)
{
    if (is_sealed(note_path(name, mode_is_write(mode))))
        return nullptr;
    return g_orig_wfopen(name, mode);
}

FILE* __cdecl hook_fopen(const char* name, const char* mode)
{
    if (is_sealed(note_path(name, mode_is_write(mode))))
        return nullptr;
    return g_orig_fopen(name, mode);
}

// Returns the previous entry, or null if the import isn't present.
void* iat_hook(HMODULE mod, const char* dll, const char* func, void* replacement)
{
    auto* base = reinterpret_cast<unsigned char*>(mod);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    const auto dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return nullptr;

    for (auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress);
         desc->Name; ++desc)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + desc->Name), dll) != 0)
            continue;

        // OriginalFirstThunk keeps the names; FirstThunk is the live table.
        // Some binaries omit the former, in which case FirstThunk holds both
        // until the loader overwrites it.
        const auto names_rva = desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + names_rva);
        auto* iat   = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; names->u1.AddressOfData; ++names, ++iat)
        {
            if (names->u1.Ordinal & IMAGE_ORDINAL_FLAG)     // imported by ordinal, no name
                continue;
            auto* imp = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(imp->Name, func) != 0)
                continue;

            DWORD old = 0;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void*), PAGE_READWRITE, &old))
                return nullptr;
            void* prev = reinterpret_cast<void*>(iat->u1.Function);
            iat->u1.Function = reinterpret_cast<ULONGLONG>(replacement);
            VirtualProtect(&iat->u1.Function, sizeof(void*), old, &old);
            return prev;
        }
    }
    return nullptr;
}

void install_playline_hooks(HMODULE game)
{
    int n = 0;
    if ((g_orig_CreateFileW = (CreateFileW_t)iat_hook(game, "KERNEL32.dll", "CreateFileW", &hook_CreateFileW))) ++n;
    if ((g_orig_CreateFileA = (CreateFileA_t)iat_hook(game, "KERNEL32.dll", "CreateFileA", &hook_CreateFileA))) ++n;

    // The CRT is imported through the api-ms-win-* apiset stubs, not ucrtbase.
    if ((g_orig_wfopen = (wfopen_t)iat_hook(game, "api-ms-win-crt-stdio-l1-1-0.dll", "_wfopen", &hook_wfopen))) ++n;
    if ((g_orig_fopen  = (fopen_t)iat_hook(game, "api-ms-win-crt-stdio-l1-1-0.dll", "fopen", &hook_fopen))) ++n;

    debug_log("playline hooks installed on " + std::to_string(n) + " of 4 entry points");
}

// ---------------------------------------------------------------------------
// state file: one "playlineN=lives" per line, kept next to kcd.log
void load_state()
{
    g_playlines.clear();
    g_pending.clear();
    std::ifstream f(g_game_root / kStateFile);
    if (!f.is_open())
        return;

    // "playline0=2" or "playline0=0,ended=<mtime ticks>". The second form is
    // only written when a run ended but could not be moved; older state files
    // have only the first, so parsing stops at the comma either way.
    for (std::string line; std::getline(f, line);)
    {
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const auto key = std::wstring(line.begin(), line.begin() + eq);
        const auto rest = line.substr(eq + 1);
        const auto comma = rest.find(',');

        try
        {
            g_playlines[key] = std::stoi(rest.substr(0, comma));
        }
        catch (...) { continue; }

        if (comma == std::string::npos)
            continue;
        const auto tag = rest.find("ended=", comma);
        if (tag == std::string::npos)
            continue;
        try
        {
            g_pending[key] = std::stoll(rest.substr(tag + 6));
        }
        catch (...) {}
    }
}

void save_state()
{
    std::ofstream f(g_game_root / kStateFile, std::ios::trunc);
    if (!f.is_open())
        return;
    for (const auto& [name, lives] : g_playlines)
    {
        f << narrow(name) << '=' << lives;
        if (const auto it = g_pending.find(name); it != g_pending.end())
            f << ",ended=" << it->second;
        f << std::endl;
    }
}

// ---------------------------------------------------------------------------
// the exec channel: console commands the Lua mod picks up via `exec`
void write_exec(int lives)
{
    std::ofstream f(g_game_root / kExecFile, std::ios::trunc);
    if (f.is_open())
        f << "permadeath_sync " << lives << std::endl;
}

void clear_exec()
{
    std::error_code ec;
    fs::remove(g_game_root / kExecFile, ec);
}

// ---------------------------------------------------------------------------
std::map<std::wstring, fs::file_time_type> playline_mtimes()
{
    std::map<std::wstring, fs::file_time_type> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(g_saves, ec))
    {
        if (!e.is_directory(ec))
            continue;
        const auto name = e.path().filename().wstring();
        if (name.rfind(L"playline", 0) != 0)
            continue;

        auto newest = fs::file_time_type::min();
        for (const auto& f : fs::directory_iterator(e.path(), ec))
        {
            const auto t = f.last_write_time(ec);
            if (!ec && t > newest)
                newest = t;
        }
        out[name] = newest;
    }
    return out;
}

std::map<std::wstring, std::set<std::wstring>> playline_files()
{
    std::map<std::wstring, std::set<std::wstring>> out;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(g_saves, ec))
    {
        if (!e.is_directory(ec))
            continue;
        const auto name = e.path().filename().wstring();
        if (name.rfind(L"playline", 0) != 0)
            continue;
        std::set<std::wstring> files;
        for (const auto& f : fs::directory_iterator(e.path(), ec))
            files.insert(f.path().filename().wstring());
        out[name] = std::move(files);
    }
    return out;
}

// Delete the save the mod wrote purely to reveal which playline is live.
// Only removes files that did not exist a moment ago, so a genuine save can
// never be caught by mistake.
void remove_identify_saves(const std::wstring& playline,
                           const std::map<std::wstring, std::set<std::wstring>>& before)
{
    const auto after = playline_files();
    const auto it_after = after.find(playline);
    if (it_after == after.end())
        return;

    std::set<std::wstring> prior;
    if (const auto it = before.find(playline); it != before.end())
        prior = it->second;

    int removed = 0;
    for (const auto& fn : it_after->second)
    {
        if (prior.count(fn))
            continue;
        if (fn.size() < 4 || _wcsicmp(fn.c_str() + fn.size() - 4, L".whs") != 0)
            continue;

        const auto path = g_saves / playline / fn;
        for (int attempt = 0; attempt < 5; ++attempt)   // game may still hold it
        {
            std::error_code ec;
            if (fs::remove(path, ec))
            {
                ++removed;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    }
    if (removed)
        debug_log("cleaned up " + std::to_string(removed) + " identify save(s)");
}

// Returns the single playline whose contents changed, or empty if ambiguous.
std::wstring detect_changed(const std::map<std::wstring, fs::file_time_type>& baseline)
{
    const auto now = playline_mtimes();
    std::wstring found;
    int count = 0;
    for (const auto& [name, t] : now)
    {
        const auto it = baseline.find(name);
        if (it == baseline.end() || t > it->second)
        {
            found = name;
            ++count;
        }
    }
    return count == 1 ? found : std::wstring();
}

// ---------------------------------------------------------------------------
// Never deletes: the run is MOVED aside so a mistake stays recoverable.
bool archive_playline(const std::wstring& playline)
{
    if (playline.empty())
        return false;

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t stamp[32];
    swprintf_s(stamp, L"_%04d%02d%02d_%02d%02d%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::error_code ec;
    fs::create_directories(g_archive, ec);

    const auto src = g_saves / playline;
    const auto dst = g_archive / (playline + stamp);

    // Retry, but only briefly. Saves are not held open at the moment of death,
    // so attempt 0 nearly always succeeds and the folder is gone before the
    // ending is even interactive. If it is STILL failing seconds later, the
    // game has a save open -- which means the player hit Continue and is
    // loading right now.
    //
    // This loop used to run for ten seconds, and that was the worst bug in the
    // mod. Reported on GOG: the player hit Continue, we retried all through
    // the load, and the moment the game released the handle a late retry
    // succeeded -- moving the save folder out from under a live session. He
    // was playing a run whose saves had just been archived. Others got a
    // "could not load save" error and a crash, from us renaming mid-load.
    //
    // A KCD load takes far longer than this window, so losing here means we
    // lose cleanly and hand off to the deferred path instead of finishing the
    // job inside somebody's game. Never win a race by damaging the winner.
    constexpr int kAttempts = 8;   // ~2s
    for (int attempt = 0; attempt < kAttempts; ++attempt)
    {
        ec.clear();
        fs::rename(src, dst, ec);
        if (!ec)
        {
            // The folder is gone, so there is nothing left to refuse -- and
            // leaving it sealed would block a new game that later reuses this
            // playline slot.
            unseal_playline();
            debug_log("ARCHIVED " + narrow(playline) + " -> " +
                      narrow(dst.filename().wstring()) +
                      (attempt ? " (after " + std::to_string(attempt) + " retries)" : ""));
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    // Still sealed, deliberately: the folder is stuck but its saves stay
    // unloadable for the rest of the session, and archive_pending_runs() takes
    // it at the next start. The run is over either way.
    debug_log("could not archive " + narrow(playline) + " (" + ec.message() +
              ") -- leaving it sealed; it will be archived at next start");
    return false;
}

// Newest file mtime in a playline folder, as a raw tick count. Only ever
// compared against another value produced here on the same machine, so the
// units being implementation-defined does not matter.
long long newest_mtime_ticks(const std::wstring& playline)
{
    std::error_code ec;
    auto newest = fs::file_time_type::min();
    for (const auto& f : fs::directory_iterator(g_saves / playline, ec))
    {
        const auto t = f.last_write_time(ec);
        if (!ec && t > newest)
            newest = t;
    }
    return newest.time_since_epoch().count();
}

// Called when archive_playline gave up. The run stays in saves\ for now, which
// is the safe outcome: the player keeps whatever they loaded, and we take it
// at the next launch instead of yanking it mid-session.
void mark_pending(const std::wstring& playline)
{
    g_playlines[playline] = 0;
    g_pending[playline] = newest_mtime_ticks(playline);
    save_state();
    debug_log("marked " + narrow(playline) + " to be archived at next start");
}

// Runs from init, before WHGame.dll is even loaded and long before the game
// has opened a save -- the one moment archiving cannot collide with anything.
void archive_pending_runs()
{
    if (g_pending.empty())
        return;

    std::error_code ec;
    for (auto it = g_pending.begin(); it != g_pending.end();)
    {
        const auto& name = it->first;
        if (!fs::exists(g_saves / name, ec))
        {
            it = g_pending.erase(it);
            continue;
        }

        // If the folder has been written since the run ended, the player
        // started a NEW game on that playline slot. Archiving it would take a
        // run that has nothing to do with permadeath, so let it go instead.
        if (newest_mtime_ticks(name) > it->second)
        {
            debug_log(narrow(name) + " was reused by a new game since that run "
                      "ended -- leaving it alone");
            g_playlines.erase(name);
            it = g_pending.erase(it);
            continue;
        }

        debug_log("archiving " + narrow(name) + ", left over from a finished run");
        if (archive_playline(name))
            g_playlines.erase(name);
        it = g_pending.erase(it);
    }
    save_state();
}

// ---------------------------------------------------------------------------
void process_line(const std::string& line,
                  std::map<std::wstring, fs::file_time_type>& baseline)
{
    // --- the player turned permadeath off for this run ---------------------
    // Has to be persistent: dropping the playline from the state file is what
    // stops the next load from silently re-arming it.
    if (line.find(kMarkerDisarm) != std::string::npos)
    {
        if (!g_target.empty())
        {
            debug_log("disarmed " + narrow(g_target) + " -- no longer a permadeath run");
            g_playlines.erase(g_target);
            save_state();
            g_target.clear();
        }
        g_ended_at_ms.store(0);
        clear_exec();
        return;
    }

    // --- which playline is live -------------------------------------------
    const bool is_identify = line.find(kMarkerIdentify) != std::string::npos;
    const bool is_arm      = line.find(kMarkerArm) != std::string::npos;

    if (is_identify || is_arm)
    {
        // Loading or flagging a run means any ending still owed from a run
        // that just finished is no longer owed -- we are somewhere else now.
        g_ended_at_ms.store(0);

        std::wstring changed;

        if (is_arm)
        {
            // Arming still forces one save. It is the only thing that works on
            // a brand-new game, where no playline directory exists yet for the
            // file hooks to have seen -- and it happens once per run, not once
            // per load, which was the actual complaint.
            const auto before_files = playline_files();
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));

            changed = detect_changed(baseline);
            if (!changed.empty())
                remove_identify_saves(changed, before_files);
            baseline = playline_mtimes();
        }
        else
        {
            // A plain load costs nothing now: the file hooks already saw which
            // playline the game read on its way in.
            changed = take_observed_playline();
            if (changed.empty())
                debug_log("no playline observed -- file hooks may not be installed");
        }

        if (changed.empty())
        {
            debug_log("could not identify playline");
            clear_exec();
            g_target.clear();
            return;
        }

        if (is_arm)
        {
            // Flagging a run for the first time: prefer whatever the mod last
            // reported, since the player has typically just set lives and we
            // had no playline to file it under yet.
            int lives = kDefaultLives;
            if (const auto it = g_playlines.find(changed); it != g_playlines.end())
                lives = it->second;
            else if (g_last_lives > 0)
                lives = g_last_lives;

            g_playlines[changed] = lives;
            g_target = changed;
            save_state();
            write_exec(lives);
            debug_log("ARMED on " + narrow(changed) + " -- " + std::to_string(lives) + " lives");
        }
        else
        {
            // Plain load: only re-arm if the player flagged THIS run before,
            // otherwise an ordinary playline would inherit permadeath state.
            const auto it = g_playlines.find(changed);
            if (it != g_playlines.end())
            {
                g_target = changed;
                write_exec(it->second);
                debug_log(narrow(changed) + " is a permadeath run -- " +
                          std::to_string(it->second) + " lives, re-armed");
            }
            else
            {
                g_target.clear();
                clear_exec();
                debug_log(narrow(changed) + " is not a permadeath run -- ignoring");
            }
        }
        return;
    }

    // --- the mod is the authority on the life count ------------------------
    const auto pos = line.find(kMarkerLives);
    if (pos == std::string::npos)
        return;

    int lives = 0;
    try
    {
        lives = std::stoi(line.substr(pos + sizeof(kMarkerLives) - 1));
    }
    catch (...)
    {
        return;
    }

    g_last_lives = lives;
    if (!g_target.empty())
    {
        g_playlines[g_target] = lives;
        save_state();
    }

    if (lives > 0)
    {
        write_exec(lives);
        debug_log("lives remaining: " + std::to_string(lives));
        return;
    }

    if (g_target.empty())
    {
        debug_log("out of lives but no playline armed -- nothing archived");
        return;
    }

    seal_playline(g_target);
    debug_log("lives exhausted -- archiving " + narrow(g_target));
    if (archive_playline(g_target))
        g_playlines.erase(g_target);
    else
        mark_pending(g_target);
    save_state();
    clear_exec();
    g_target.clear();

    // We got here before the game showed its game-over screen, so the hook is
    // about to find nothing armed. Let it know the ending is still owed.
    g_ended_at_ms.store(now_ms());
}

// ---------------------------------------------------------------------------
void watch_log()
{
    const auto log_path = g_game_root / L"kcd.log";

    // Don't replay the previous session: start from the end of the file as it
    // stands, and reset whenever the game recreates it on launch.
    std::error_code ec;
    auto pos = (std::streamoff)fs::file_size(log_path, ec);
    if (ec)
        pos = 0;

    auto baseline = playline_mtimes();
    debug_log("watcher started");

    while (g_running)
    {
        // Short tick: this is also how quickly we notice a final death, and
        // that latency is part of the window in which the player can still
        // act on a run that is over. Each pass is only a file-size check.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // The game-over hook fired on a final death; finish the job here,
        // off the game's thread.
        if (g_archive_pending.exchange(false))
        {
            // No settling delay: archive_playline retries instead, so the run
            // is moved as soon as it can be. Waiting here just widened the
            // window for the player to hit Continue and try to load a save
            // from a run that has already ended.
            if (!g_target.empty())
            {
                debug_log("final death confirmed -- archiving " + narrow(g_target));
                if (archive_playline(g_target))
                    g_playlines.erase(g_target);
                else
                    mark_pending(g_target);   // records lives=0 for us
                save_state();
                clear_exec();
                g_target.clear();
            }
        }

        const auto size = (std::streamoff)fs::file_size(log_path, ec);
        if (ec)
            continue;

        if (size < pos)
        {
            debug_log("log rotated");
            pos = 0;
            baseline = playline_mtimes();
        }
        if (size == pos)
            continue;

        std::ifstream f(log_path, std::ios::binary);
        if (!f.is_open())
            continue;
        f.seekg(pos);

        for (std::string line; g_running && std::getline(f, line);)
        {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            process_line(line, baseline);
        }
        pos = (std::streamoff)f.tellg();
        if (pos < 0)
            pos = size;
    }
}

// ---------------------------------------------------------------------------
// Native game-over hook.
//
// WHGame.dll+0x123b270 is TriggerGameOver(subsystem, game_over_id) -- found by
// disassembling the wh_pl_GameOverTest cvar's change-callback (0x1283324),
// which tail-calls it with id 0x14. Real deaths reach it too, from
// 0xaa453f (id 0 = DiedUnknown) and 0xaa455b (id from a cause lookup table).
//
// Row 20 ("Test") is the only row with game_over_type_id 1 -- the distinct
// "The End" artwork. So on the player's final death we swap the id for 20 and
// the game shows it through its own code path. This replaces the old trick of
// poking the cvar from Lua, which was a race we only sometimes won.
// The function is located by SIGNATURE, not by a fixed offset, because the
// storefronts ship different builds of WHGame.dll. Steam's is 47,896,576 bytes
// and has this function at 0x123b270; Epic's is 47,870,976 and has it at
// 0x1237928. Version 0.9 hardcoded the Steam offset and patched it without
// checking, so on Epic it wrote a 15-byte jump into the middle of an unrelated
// function -- the hook never fired, and something else's code was destroyed.
// It happened not to crash, which is luck, not safety.
//
// Rule this bought: never patch an address that has not been verified to hold
// what you think it holds.
//
// The signature is the first 32 bytes of TriggerGameOver -- the four-
// instruction prologue we steal, plus enough of the body to be unique. It
// stops just before the first RIP-relative displacement, which is the first
// thing that differs between the two builds. Verified unique in both.
constexpr unsigned char kSigTriggerGameOver[] = {
    0x48, 0x89, 0x5C, 0x24, 0x10,             // mov  [rsp+10h], rbx
    0x48, 0x89, 0x74, 0x24, 0x18,             // mov  [rsp+18h], rsi
    0x57,                                     // push rdi
    0x48, 0x83, 0xEC, 0x60,                   // sub  rsp, 60h     <- 15 stolen
    0x83, 0x79, 0x18, 0x00,                   // cmp  dword [rcx+18h], 0
    0x8B, 0xF2,                               // mov  esi, edx
    0x48, 0x8B, 0xF9,                         // mov  rdi, rcx
    0x0F, 0x85, 0x48, 0x01, 0x00, 0x00,       // jne  +148h
    0x48, 0x8B,                               // (start of a rip-relative mov)
};

constexpr int    kGameOverIdTheEnd = 20;
constexpr size_t kStolenBytes      = 15;   // 4 whole instructions

using TriggerGameOverFn = long long(__fastcall*)(void*, int);
TriggerGameOverFn g_orig_trigger = nullptr;

bool should_force_the_end()
{
    // "Is this death the last one?" -- answered from the stored count, which
    // at this instant is still the value from BEFORE this death. So 1 means
    // this death spends the last life.
    //
    // That relies on an ordering, so it is worth writing down: the game calls
    // TriggerGameOver within milliseconds of the death, whereas our stored
    // count only drops once the Lua poll notices (<=200ms) AND the watcher
    // reads the log line (<=500ms). We therefore win by roughly 200-700ms.
    // Verified in-game on a 2-life run: the first death produced
    // "lives remaining: 1" with no ending, and only the second death fired
    // this. Do not "fix" this to <= 0 -- the decrement has not happened yet.
    if (g_target.empty())
        return false;
    const auto it = g_playlines.find(g_target);
    return it != g_playlines.end() && it->second <= 1;
}

// The other half of that race, and the reason it needs one.
//
// The ordering above holds for deaths where the game puts the game-over screen
// up straight away. It does NOT hold for every death: a fall death plays a
// ragdoll and a death camera first, which is long enough for the Lua poll to
// report "Lives left: 0", for the watcher to archive the run, and for g_target
// to be cleared -- all before TriggerGameOver is finally called. The hook then
// found nothing armed, left the id alone, and the player got the ordinary
// death screen instead of the ending. Reported on a fall death, Epic, 0.9.
//
// So: when a run is archived we stamp the time, and a game-over arriving
// shortly after still gets the ending. Consumed on first use, and cleared
// whenever a run is armed or disarmed, so it can only ever apply to the death
// that ended that run -- never to a later one on a different playline.
bool end_grace_consumed()
{
    const long long at = g_ended_at_ms.load();
    if (at == 0 || now_ms() - at > kEndGraceMs)
        return false;
    g_ended_at_ms.store(0);
    return true;
}

long long __fastcall hook_trigger_game_over(void* self, int game_over_id)
{
    // Logged unconditionally: game-over is rare, and knowing whether this hook
    // fired at all -- and with which id -- is the first question worth asking
    // about any "I did not get the ending" report.
    debug_log("game over: id " + std::to_string(game_over_id));

    if (!should_force_the_end())
    {
        if (end_grace_consumed())
        {
            debug_log("  run already archived by the script side -- id -> " +
                      std::to_string(kGameOverIdTheEnd));
            game_over_id = kGameOverIdTheEnd;
        }
        return g_orig_trigger(self, game_over_id);
    }

    debug_log("final death -- game_over_id " + std::to_string(game_over_id) +
              " -> " + std::to_string(kGameOverIdTheEnd));
    game_over_id = kGameOverIdTheEnd;

    // Seal on the game's own thread, before this call even returns, so the
    // saves are already unloadable by the time the death screen is drawn.
    // Nothing the player can click gets in ahead of this.
    seal_playline(g_target);

    // Archive from here rather than waiting for the mod to report the death:
    // showing the ending tears the level down, which kills the Lua poll timer
    // before it can log "[Permadeath] Lives left: 0", so relying on that meant
    // the run was never archived. This hook IS the moment of the final death,
    // so it is the authoritative trigger. Hand the work to the watcher thread
    // -- never do file I/O on the game's thread from inside a hook.
    g_archive_pending = true;
    return g_orig_trigger(self, game_over_id);
}

std::string hex_off(uintptr_t v)
{
    char buf[32];
    sprintf_s(buf, "%llX", (unsigned long long)v);
    return buf;
}

// Scan .text for the signature. Requires exactly one match: zero means this is
// a build we do not know, more than one means the signature is not specific
// enough to trust. Either way we refuse to patch rather than guess -- a wrong
// guess corrupts the game, while refusing only costs the ending screen.
unsigned char* find_trigger_game_over(uintptr_t base)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    constexpr size_t kSigLen = sizeof(kSigTriggerGameOver);
    unsigned char* found = nullptr;
    int hits = 0;

    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && hits < 2; ++i)
    {
        if (memcmp(sec[i].Name, ".text", 5) != 0)
            continue;

        auto* start = reinterpret_cast<unsigned char*>(base + sec[i].VirtualAddress);
        const size_t len = sec[i].Misc.VirtualSize;
        if (len < kSigLen)
            continue;

        for (size_t off = 0; off + kSigLen <= len; ++off)
        {
            if (start[off] != kSigTriggerGameOver[0])
                continue;
            if (memcmp(start + off, kSigTriggerGameOver, kSigLen) != 0)
                continue;
            if (++hits > 1)
                break;
            found = start + off;
        }
    }

    if (hits != 1)
    {
        debug_log("game-over signature: " + std::to_string(hits) +
                  " matches in .text -- refusing to patch this build of WHGame.dll");
        return nullptr;
    }

    debug_log("game-over function found at WHGame.dll+0x" +
              hex_off(reinterpret_cast<uintptr_t>(found) - base));
    return found;
}

bool install_game_over_hook(uintptr_t base)
{
    unsigned char* target = find_trigger_game_over(base);
    if (!target)
        return false;

    // Trampoline: the stolen instructions, then an absolute jump back.
    // Allocated separately because our DLL is far outside rel32 range.
    auto* tramp = static_cast<unsigned char*>(
        VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp)
        return false;

    memcpy(tramp, target, kStolenBytes);
    unsigned char* p = tramp + kStolenBytes;
    *p++ = 0xFF; *p++ = 0x25;                       // jmp qword ptr [rip+0]
    *reinterpret_cast<uint32_t*>(p) = 0; p += 4;
    *reinterpret_cast<uint64_t*>(p) = reinterpret_cast<uint64_t>(target + kStolenBytes);

    g_orig_trigger = reinterpret_cast<TriggerGameOverFn>(tramp);

    // Overwrite the prologue with an absolute jump to our hook.
    DWORD old = 0;
    if (!VirtualProtect(target, kStolenBytes, PAGE_EXECUTE_READWRITE, &old))
        return false;

    unsigned char patch[kStolenBytes];
    memset(patch, 0x90, kStolenBytes);              // pad remainder with nops
    patch[0] = 0xFF; patch[1] = 0x25;
    *reinterpret_cast<uint32_t*>(patch + 2) = 0;
    *reinterpret_cast<uint64_t*>(patch + 6) = reinterpret_cast<uint64_t>(&hook_trigger_game_over);

    memcpy(target, patch, kStolenBytes);
    VirtualProtect(target, kStolenBytes, old, &old);
    FlushInstructionCache(GetCurrentProcess(), target, kStolenBytes);
    return true;
}

// WHGame.dll is loaded after us (we come in via Win64Shared at startup), so
// wait for it rather than assuming it is already mapped.
void hook_thread()
{
    for (int i = 0; i < 600 && g_running; ++i)
    {
        if (const HMODULE m = GetModuleHandleW(L"WHGame.dll"))
        {
            std::this_thread::sleep_for(std::chrono::seconds(2));  // let it settle
            install_playline_hooks(m);
            if (install_game_over_hook(reinterpret_cast<uintptr_t>(m)))
                debug_log("game-over hook installed");
            else
                debug_log("game-over hook NOT installed -- lives and archiving "
                          "still work, but the last death shows the ordinary "
                          "death screen instead of the ending");
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    debug_log("WHGame.dll never appeared -- no game-over hook");
}

// ---------------------------------------------------------------------------
void init(HMODULE self)
{
    // We live in <game>\Bin\Win64Shared\, so the game root is three up.
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(self, buf, MAX_PATH * 2);
    g_game_root = fs::path(buf).parent_path().parent_path().parent_path();

    PWSTR saved = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &saved)))
    {
        g_saves   = fs::path(saved) / L"kingdomcome" / L"saves";
        g_archive = fs::path(saved) / L"kingdomcome" / kArchiveDir;
        CoTaskMemFree(saved);
    }

    debug_log("---- permadeath dll init ----");
    debug_log("game root: " + narrow(g_game_root.wstring()));
    debug_log("saves    : " + narrow(g_saves.wstring()));

    load_state();
    for (const auto& [name, lives] : g_playlines)
        debug_log("known permadeath run: " + narrow(name) + " (" + std::to_string(lives) + " lives)");

    // Before anything else touches the saves folder.
    archive_pending_runs();

    // Stale exec files would wrongly arm the next run that loads.
    clear_exec();

    g_running = true;
    g_thread = std::thread(watch_log);
    std::thread(hook_thread).detach();
}

}  // namespace

// ---------------------------------------------------------------------------
BOOL APIENTRY DllMain(HMODULE hinstDLL, DWORD fdwReason, LPVOID)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hinstDLL);
        init(hinstDLL);
        break;

    case DLL_PROCESS_DETACH:
        g_running = false;
        if (g_thread.joinable())
            g_thread.detach();   // process is going away; don't block teardown
        break;

    default:
        break;
    }
    return TRUE;
}
