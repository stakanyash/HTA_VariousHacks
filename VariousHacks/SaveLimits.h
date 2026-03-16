#pragma once
#include "ThirdParty/Injector/injector.hpp"
#include "ThirdParty/IniReader/IniReader.h"
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "funcarg.h"

// Globals

static char g_autoSaveLabel[128] = "Autosave";      // localized autosave label from gamestrings.xml
static char g_pendingFolderName[64] = {};            // folder name of the save being processed (e.g. "auto_00000001")
static char g_pendingPrefix[256] = {};               // map name or custom save name extracted from stack
static char g_correctedName[512] = {};               // final corrected save name for FadingMsg
static void* g_savesMgrThis = nullptr;               // SavesManager instance pointer for ReloadSaveInfos
static bool g_isComRem = false;                      // true if running Community Remaster version

// Game addresses

static const uintptr_t OrigFunc = 0x0057C5A0; // SavesManager::SaveGame
static const uintptr_t LoadInfosFunc = 0x0057D310; // SavesManager::LoadInfos
static const uintptr_t AddrAddImpByStrIdFmt = 0x0040A8D0; // n_AddImportantFadingMsgByStrIdFormatted (Lua native, called via sArgStack)
static const uintptr_t OrigAfterSave = 0x0057CC70; // sub_57CC70, called after successful save in SavesManager::SaveGame

// Call AddImportantFadingMsgByStrIdFormatted via sArgStack
// The native expects sArgStack* in ecx. Build the stack manually and call the native directly.

static void CallAddImportantFadingMsgFormatted(const char* strId, const char* text)
{
    m3d::sArgStack stack;
    stack.clear();

    m3d::sArg* a0 = stack.newIn();
    a0->m_type = m3d::sArg::ARGTYPE_STRING;
    a0->m_s = const_cast<char*>(strId);

    m3d::sArg* a1 = stack.newIn();
    a1->m_type = m3d::sArg::ARGTYPE_STRING;
    a1->m_s = const_cast<char*>(text);

    void* pStack = &stack;
    __asm
    {
        mov  ecx, pStack
        call AddrAddImpByStrIdFmt
    }
}

// Detect game version
// Uses for levelinfo check because
// Community Remaster uses a levelinfo_hd file
// instead of levelinfo

static void DetectGameVersion()
{
    FILE* f = fopen("hta.exe", "rb");
    if (!f) return;
    fseek(f, 0x590680, SEEK_SET);
    char buf[128] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (strstr(buf, "Community Remaster"))
        g_isComRem = true;
}

// Load localized autosave label from gamestrings.xml
// Finds the "AutoSave" string entry and stores its value in g_autoSaveLabel.

static void LoadAutoSaveLabel()
{
    FILE* f = fopen("../data/if/strings/gamestrings.xml", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char* buf = new char[sz + 1];
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    const char* p = strstr(buf, "\"AutoSave\"");
    if (p) {
        const char* v = strstr(p, "value=\"");
        if (v) {
            v += 7;
            const char* end = strchr(v, '"');
            if (end) {
                int len = (int)(end - v);
                if (len > 0 && len < (int)sizeof(g_autoSaveLabel)) {
                    memcpy(g_autoSaveLabel, v, len);
                    g_autoSaveLabel[len] = 0;
                }
            }
        }
    }
    delete[] buf;
}

// Get full display name of a level from levelinfo.xml
// Looks up the technical level name (e.g. "r1m4") and returns its fullName attribute.

static void GetLevelFullName(const char* levelName, char* outName, int outSize)
{
    outName[0] = 0;
    const char* xmlPath = g_isComRem
        ? "data/if/diz/levelinfo_hd.xml"
        : "data/if/diz/levelinfo.xml";
    FILE* f = fopen(xmlPath, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char* buf = new char[sz + 1];
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    char needle[64];
    sprintf(needle, "name=\"%s\"", levelName);
    const char* p = strstr(buf, needle);
    if (p) {
        const char* fn = strstr(p, "fullName=\"");
        if (fn) {
            fn += 10;
            const char* end = strchr(fn, '"');
            if (end) {
                int len = (int)(end - fn);
                if (len > 0 && len < outSize) {
                    memcpy(outName, fn, len);
                    outName[len] = 0;
                }
            }
        }
    }
    delete[] buf;
}

// Read UnnamedAutoSaveIndex from a SaveInfo.xml
// Returns -1 if the attribute is not present (save has no index yet).

static int ReadUnnamedAutoSaveIndex(const char* xmlPath)
{
    FILE* f = fopen(xmlPath, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return -1; }
    fseek(f, 0, SEEK_SET);
    char* buf = new char[sz + 1];
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);
    int result = -1;
    const char* p = strstr(buf, "UnnamedAutoSaveIndex=\"");
    if (p) {
        p += 22;
        result = atoi(p);
    }
    delete[] buf;
    return result;
}

// Get the highest UnnamedAutoSaveIndex across all auto_* saves for a profile
// skipFolder is excluded from the search (the save currently being written).

static int GetMaxUnnamedAutoSaveIndex(const char* profileName, const char* skipFolder)
{
    int maxIndex = 0;
    char pattern[512];
    sprintf(pattern, "data/profiles/%s/saves/auto_*", profileName);
    WIN32_FIND_DATAA fd;
    HANDLE hp = FindFirstFileA(pattern, &fd);
    if (hp == INVALID_HANDLE_VALUE) return 0;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            && fd.cFileName[0] != '.'
            && strcmp(fd.cFileName, skipFolder) != 0)
        {
            char xmlPath[512];
            sprintf(xmlPath, "data/profiles/%s/saves/%s/SaveInfo.xml",
                profileName, fd.cFileName);
            int c = ReadUnnamedAutoSaveIndex(xmlPath);
            if (c > maxIndex) maxIndex = c;
        }
    } while (FindNextFileA(hp, &fd));
    FindClose(hp);
    return maxIndex;
}

// Find which profile owns a given save folder
// Searches all profiles for the folder and returns the profile name.

static bool FindProfileByFolder(const char* folder, char* outProfile, int outProfileSz)
{
    outProfile[0] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE hp = FindFirstFileA("data/profiles/*", &fd);
    if (hp == INVALID_HANDLE_VALUE) return false;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            char candidate[512];
            sprintf(candidate, "data/profiles/%s/saves/%s", fd.cFileName, folder);
            DWORD attr = GetFileAttributesA(candidate);
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
                strncpy(outProfile, fd.cFileName, outProfileSz - 1);
                outProfile[outProfileSz - 1] = 0;
                FindClose(hp);
                return true;
            }
        }
    } while (FindNextFileA(hp, &fd));
    FindClose(hp);
    return false;
}

// Compute the corrected save name for FadingMsg
// Called before the save is written, using g_pendingPrefix from the stack.
// - If g_pendingPrefix matches a level fullName -> unnamed autosave -> "LevelName Autosave N"
// - Otherwise -> custom named save -> use g_pendingPrefix as-is

static void ComputeCorrectedName()
{
    g_correctedName[0] = 0;
    if (g_pendingFolderName[0] == 0) return;
    if (g_pendingPrefix[0] == 0) return;

    char profileName[256] = {};
    if (!FindProfileByFolder(g_pendingFolderName, profileName, sizeof(profileName)))
        return;

    int maxIndex = GetMaxUnnamedAutoSaveIndex(profileName, g_pendingFolderName);

    // Check if pendingPrefix is a known level fullName (unnamed autosave)
    bool hasCustom = true;
    const char* xmlPath = g_isComRem
        ? "data/if/diz/levelinfo_hd.xml"
        : "data/if/diz/levelinfo.xml";
    FILE* f = fopen(xmlPath, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        if (sz > 0) {
            fseek(f, 0, SEEK_SET);
            char* buf = new char[sz + 1];
            fread(buf, 1, (size_t)sz, f);
            buf[sz] = 0;
            fclose(f);
            char needle[270];
            sprintf(needle, "fullName=\"%s\"", g_pendingPrefix);
            if (strstr(buf, needle))
                hasCustom = false;
            delete[] buf;
        }
        else fclose(f);
    }

    int newIndex = hasCustom ? maxIndex : maxIndex + 1;
    if (newIndex < 1 && !hasCustom) newIndex = 1;

    if (hasCustom)
        sprintf(g_correctedName, "%s", g_pendingPrefix);
    else
        sprintf(g_correctedName, "%s %s %d", g_pendingPrefix, g_autoSaveLabel, newIndex);
}

// Patch SaveInfo.xml after the game writes it
// Replaces the Name attribute with the corrected name and adds
// UnnamedAutoSaveIndex and IsAutoSave attributes.

// UnnamedAutoSaveIndex used for numbering autosaves without a custom name.
// IsAutoSave just a flag that this is a AutoSave. Anyway this is a request from E Jet.

static void PatchSaveInfoXml()
{
    if (g_pendingFolderName[0] == 0) return;

    // Find which profile owns this save folder
    WIN32_FIND_DATAA fd;
    HANDLE hp = FindFirstFileA("data/profiles/*", &fd);
    if (hp == INVALID_HANDLE_VALUE) return;

    char profileName[256] = {};
    char xmlPath[512] = {};
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            char candidate[512];
            sprintf(candidate, "data/profiles/%s/saves/%s/SaveInfo.xml",
                fd.cFileName, g_pendingFolderName);
            FILE* f = fopen(candidate, "rb");
            if (f) {
                fclose(f);
                strcpy(xmlPath, candidate);
                strcpy(profileName, fd.cFileName);
                break;
            }
        }
    } while (FindNextFileA(hp, &fd));
    FindClose(hp);

    if (xmlPath[0] == 0) return;

    FILE* f = fopen(xmlPath, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char* buf = new char[sz + 1];
    fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);

    // Read LevelName to look up the full display name
    char levelName[64] = {};
    char* lvlAttr = strstr(buf, "LevelName=\"");
    if (lvlAttr) {
        lvlAttr += 11;
        char* lvlEnd = strchr(lvlAttr, '"');
        if (lvlEnd) {
            int len = (int)(lvlEnd - lvlAttr);
            if (len > 0 && len < (int)sizeof(levelName)) {
                memcpy(levelName, lvlAttr, len);
                levelName[len] = 0;
            }
        }
    }

    char levelFullName[256] = {};
    if (levelName[0])
        GetLevelFullName(levelName, levelFullName, sizeof(levelFullName));

    // Determine if this is a custom-named save or an unnamed autosave
    bool hasCustom = false;
    char customName[256] = {};
    if (g_pendingPrefix[0] && levelFullName[0]) {
        if (strcmp(g_pendingPrefix, levelFullName) != 0) {
            hasCustom = true;
            int mapLen = (int)strlen(levelFullName);
            // Strip the level name prefix if present (e.g. "LevelName CustomName" -> "CustomName")
            if (strncmp(g_pendingPrefix, levelFullName, mapLen) == 0
                && g_pendingPrefix[mapLen] == ' ')
                strncpy(customName, g_pendingPrefix + mapLen + 1, sizeof(customName) - 1);
            else
                strncpy(customName, g_pendingPrefix, sizeof(customName) - 1);
        }
    }

    int maxIndex = GetMaxUnnamedAutoSaveIndex(profileName, g_pendingFolderName);
    int newIndex = hasCustom ? maxIndex : maxIndex + 1;
    if (newIndex < 1 && !hasCustom) newIndex = 1;

    // Build the final save name
    char newName[512] = {};
    if (hasCustom)
        sprintf(newName, "%s", customName);
    else if (levelFullName[0])
        sprintf(newName, "%s %s %d", levelFullName, g_autoSaveLabel, newIndex);
    else
        sprintf(newName, "%s %d", g_autoSaveLabel, newIndex);

    // Rebuild XML with patched Name and new attributes
    char* nameAttr = strstr(buf, "Name=\"");
    if (!nameAttr) { delete[] buf; return; }
    nameAttr += 6;
    char* nameEnd = strchr(nameAttr, '"');
    if (!nameEnd) { delete[] buf; return; }

    char* lvlAttr2 = strstr(buf, "LevelName=\"");
    if (!lvlAttr2) { delete[] buf; return; }
    lvlAttr2 += 11;
    char* lvlEnd2 = strchr(lvlAttr2, '"');
    if (!lvlEnd2) { delete[] buf; return; }

    char* gameTime = strstr(buf, "<GameTime");
    if (!gameTime) { delete[] buf; return; }

    char newBuf[4096];
    newBuf[0] = 0;

    int part1Len = (int)(nameAttr - buf);
    memcpy(newBuf, buf, part1Len);
    newBuf[part1Len] = 0;
    strcat(newBuf, newName);
    strcat(newBuf, "\"");

    int part2Start = (int)(nameEnd + 1 - buf);
    int part2Len = (int)(lvlEnd2 + 1 - buf) - part2Start;
    strncat(newBuf, buf + part2Start, part2Len);

    char attribStr[128];
    sprintf(attribStr, "\n\tUnnamedAutoSaveIndex=\"%d\"\n\tIsAutoSave=\"True\">", newIndex);
    strcat(newBuf, attribStr);
    strcat(newBuf, "\n\t");
    strcat(newBuf, gameTime);

    FILE* fw = fopen(xmlPath, "wb");
    if (fw) {
        fwrite(newBuf, 1, strlen(newBuf), fw);
        fclose(fw);
    }

    delete[] buf;
    g_pendingFolderName[0] = 0;
}

// Reload save list in SavesManager
// Forces the UI to refresh after patching XML.

static void ReloadSaveInfos()
{
    if (!g_savesMgrThis) return;
    void* mgr = g_savesMgrThis;
    __asm {
        mov  ecx, mgr
        call LoadInfosFunc
    }
}

// Background thread: patch XML and reload saves
// Runs with a delay to ensure the game has finished writing SaveInfo.xml.

static DWORD WINAPI PatchThread(LPVOID)
{
    Sleep(200);
    PatchSaveInfoXml();
    ReloadSaveInfos();
    return 0;
}

static void EnableAutosaveSuppression()
{
    injector::WriteMemory<uint8_t>(0x0057C554 + 0, 0x83, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 1, 0xC4, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 2, 0x08, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 3, 0x90, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 4, 0x90, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 5, 0x90, true);
}

static void DisableAutosaveSuppression()
{
    injector::WriteMemory<uint8_t>(0x0057C554 + 0, 0xFF, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 1, 0x92, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 2, 0xB0, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 3, 0x00, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 4, 0x00, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 5, 0x00, true);
}

// Extract save info from the stack before SaveGame executes
// Captures the save folder name and the pending prefix (map/custom name)
// so function can compute the corrected name before the game proceeds.

static void __cdecl PrepareFolderName(void* ediVal, void* ecxVal, void* ebpVal)
{
    g_pendingFolderName[0] = 0;
    g_pendingPrefix[0] = 0;
    g_correctedName[0] = 0;
    g_savesMgrThis = ecxVal;

    if (!ediVal) return;
    char* folderPtr = *(char**)ediVal;
    if (!folderPtr) return;
    if (strncmp(folderPtr, "auto_", 5) == 0)
    {
        EnableAutosaveSuppression();
    }
    else
    {
        DisableAutosaveSuppression();
        return;
    }

    strncpy(g_pendingFolderName, folderPtr, sizeof(g_pendingFolderName) - 1);

    // Read the full save name string from ebp (e.g. "LevelName Autosave 1" or "CustomName Autosave 1")
    void* ebpContent = nullptr;
    if (ebpVal && !IsBadReadPtr(ebpVal, 4))
        ebpContent = *(void**)ebpVal;

    if (ebpContent && !IsBadReadPtr(ebpContent, 1)) {
        char* fullStr = (char*)ebpContent;
        if (!IsBadReadPtr(fullStr, 1) && fullStr[0] != 0) {
            // Extract the prefix before " Autosave" (map name or custom name)
            char autoWithSpace[130];
            sprintf(autoWithSpace, " %s", g_autoSaveLabel);
            const char* autoPos = strstr(fullStr, autoWithSpace);
            if (autoPos) {
                int prefLen = (int)(autoPos - fullStr);
                if (prefLen > 0 && prefLen < (int)sizeof(g_pendingPrefix)) {
                    memcpy(g_pendingPrefix, fullStr, prefLen);
                    g_pendingPrefix[prefLen] = 0;
                }
            }
        }
    }

    // Compute the corrected name now (synchronously) so HookAfterSave can use it
    ComputeCorrectedName();

    // Launch background thread to patch XML and reload saves after game writes them
    HANDLE hThread = CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

// Hook at SavesManager::SaveGame call site
// Intercepts the call to SaveGame, captures stack state, then jumps to original.

__declspec(naked) static void HookBeforeSaveGame()
{
    __asm {
        pushad
        mov  eax, [esp + 2Ch]   // ebpVal
        push eax
        push ecx                // ecxVal (SavesManager* this)
        push edi                // ediVal (save folder name ptr)
        call PrepareFolderName
        add  esp, 12
        popad
        jmp  OrigFunc
    }
}

// Show corrected FadingMsg after save completes
// Called from HookAfterSave in the main thread, after SaveGame succeeds.

static void AfterSaveHookImpl()
{
    if (g_correctedName[0] != '\0')
    {
        CallAddImportantFadingMsgFormatted("GameWasSaved", g_correctedName);
        g_correctedName[0] = '\0';
    }
}

// Hook at sub_57CC70 call site (post-save success path)
// Fires in the main thread after a successful save, allowing safe FadingMsg call.

__declspec(naked) static void HookAfterSave()
{
    __asm {
        pushad
        call AfterSaveHookImpl
        popad
        mov  eax, edi
        jmp  OrigAfterSave
    }
}

void InitSaveLimits()
{
    CIniReader iniReader("VariousHacks.ini");

    uint32_t quicksave_limit = (uint32_t)iniReader.ReadInteger("GENERAL", "QuicksaveLimit", 5);
    uint32_t autosave_limit = (uint32_t)iniReader.ReadInteger("GENERAL", "AutosaveLimit", 10);

    if (quicksave_limit < 1)   quicksave_limit = 1;
    if (quicksave_limit > 255) quicksave_limit = 255;
    if (autosave_limit < 1)   autosave_limit = 1;
    if (autosave_limit > 255) autosave_limit = 255;

    injector::WriteMemory<uint32_t>(0x0057BCB6, quicksave_limit);
    injector::WriteMemory<uint32_t>(0x0057BCBD, autosave_limit);

    DetectGameVersion();
    LoadAutoSaveLabel();

    injector::MakeCALL(0x0057C309, HookBeforeSaveGame, true);
    injector::MakeCALL(0x0057C366, HookAfterSave, true);

    // Known issues:

    // Autosave numbering breaks when switching profiles, because the code does not detect
    // profile changes at runtime. It's only my fault, lol.
}