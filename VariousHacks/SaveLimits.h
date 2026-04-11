#pragma once
#include "ThirdParty/Injector/injector.hpp"
#include "ThirdParty/IniReader/IniReader.h"
#include <cstdio>
#include <cstring>
#include <windows.h>
#include "funcarg.h"

struct PendingSaveData {
    char folderName[64];
    char prefix[256];
    char profileName[256];
    int index;
    bool hasCustom;
    char levelFullName[256];
    void* savesMgrThis;
};

// Globals

static char g_autoSaveLabel[128] = "Autosave";      // localized autosave label from gamestrings.xml
static char g_correctedName[512] = {};              // final corrected save name for FadingMsg
static void* g_savesMgrThis = nullptr;              // SavesManager instance pointer for ReloadSaveInfos
static bool g_isComRem = false;                     // true if running Community Remaster version
static PendingSaveData g_pendingSaveData = {};      // pending save data for patching after save
static bool g_hasPendingSave = false;               // flag indicating pending save exists

// Game addresses

static const uintptr_t OrigFunc = 0x0057C5A0;               // SavesManager::AutoSave
static const uintptr_t LoadInfosFunc = 0x0057D310;          // SavesManager::LoadInfos
static const uintptr_t AddrAddImpByStrIdFmt = 0x0040A8D0;   // n_AddImportantFadingMsgByStrIdFormatted (Lua native, called via sArgStack)
static const uintptr_t OrigAfterSave = 0x0057CC70;          // sub_57CC70, called after successful save in SavesManager::SaveGame

// CallAddImportantFadingMsgFormatted:
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

// DetectGameVersion:
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

// LoadAutoSaveLabel:
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

// GetLevelFullName:
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

// ReadUnnamedAutoSaveIndex:
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

// GetLastUsedAutoSaveIndex:
// Get the UnnamedAutoSaveIndex from the most recently modified auto_* save for a profile.
// skipFolder is excluded from the search (the save currently being written).
// Uses SaveInfo.xml file modification time, not directory time.
// Returns 0 if no saves found.

static int GetLastUsedAutoSaveIndex(const char* profileName, const char* skipFolder)
{
    int lastIndex = 0;
    FILETIME bestTime = {};
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

            // Get SaveInfo.xml modification time
            WIN32_FIND_DATAA xmlFd;
            HANDLE hXml = FindFirstFileA(xmlPath, &xmlFd);
            if (hXml != INVALID_HANDLE_VALUE) {
                FindClose(hXml);

                // Use file time of SaveInfo.xml
                if (CompareFileTime(&xmlFd.ftLastWriteTime, &bestTime) > 0) {
                    int c = ReadUnnamedAutoSaveIndex(xmlPath);
                    if (c > 0) {
                        bestTime = xmlFd.ftLastWriteTime;
                        lastIndex = c;
                    }
                }
            }
        }
    } while (FindNextFileA(hp, &fd));
    FindClose(hp);
    return lastIndex;
}

// FindProfileByFolder:
// Determines which profile owns a given save folder.
// Scans all profiles for "saves/<folder>" and selects the profile
// whose matching folder has the most recent last write time.

static bool FindProfileByFolder(const char* folder, char* outProfile, int outProfileSz)
{
    outProfile[0] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE hp = FindFirstFileA("data/profiles/*", &fd);
    if (hp == INVALID_HANDLE_VALUE) return false;

    FILETIME bestTime = {};
    char bestProfile[256] = {};

    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
            char candidate[512];
            sprintf(candidate, "data/profiles/%s/saves/%s", fd.cFileName, folder);

            WIN32_FIND_DATAA fdDir;
            HANDLE hd = FindFirstFileA(candidate, &fdDir);
            if (hd != INVALID_HANDLE_VALUE) {
                FindClose(hd);
                if (CompareFileTime(&fdDir.ftLastWriteTime, &bestTime) > 0) {
                    bestTime = fdDir.ftLastWriteTime;
                    strncpy(bestProfile, fd.cFileName, sizeof(bestProfile) - 1);
                    bestProfile[sizeof(bestProfile) - 1] = 0;
                }
            }
        }
    } while (FindNextFileA(hp, &fd));
    FindClose(hp);

    if (bestProfile[0]) {
        strncpy(outProfile, bestProfile, outProfileSz - 1);
        outProfile[outProfileSz - 1] = 0;
        return true;
    }
    return false;
}

// ComputeCorrectedName:
// Compute the corrected save name for FadingMsg
// Called before the save is written, using pendingPrefix from the stack.
// - If pendingPrefix matches a level fullName -> unnamed autosave -> "LevelName Autosave N"
// - Otherwise -> custom named save -> use pendingPrefix as-is
// Fills outData with all computed values for later use in PatchSaveInfoXml.

static void ComputeCorrectedName(const char* folderName, const char* prefix, const char* profileName, PendingSaveData& outData)
{
    outData.index = 0;
    outData.hasCustom = false;
    outData.levelFullName[0] = 0;

    g_correctedName[0] = 0;
    if (folderName[0] == 0) return;
    if (prefix[0] == 0) return;
    if (profileName[0] == 0) return;

    // Determine if prefix matches a level fullName
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

            // Search for level fullName match
            bool foundLevel = false;
            const char* searchPos = buf;

            // Extract clean prefix (without autosave label)
            char cleanPrefix[256];
            const char* autoPos = strstr(prefix, g_autoSaveLabel);
            if (autoPos) {
                int cleanLen = (int)(autoPos - prefix);
                if (cleanLen > 0 && cleanLen < (int)sizeof(cleanPrefix)) {
                    memcpy(cleanPrefix, prefix, cleanLen);
                    while (cleanLen > 0 && cleanPrefix[cleanLen - 1] == ' ') cleanLen--;
                    cleanPrefix[cleanLen] = 0;
                }
                else {
                    strcpy(cleanPrefix, prefix);
                }
            }
            else {
                strcpy(cleanPrefix, prefix);
            }

            while (!foundLevel) {
                const char* fnAttr = strstr(searchPos, "fullName=\"");
                if (!fnAttr) break;
                fnAttr += 10;
                const char* fnEnd = strchr(fnAttr, '"');
                if (!fnEnd) break;

                int fnLen = (int)(fnEnd - fnAttr);
                if (fnLen > 0 && fnLen < 256) {
                    char fullLevelName[256];
                    memcpy(fullLevelName, fnAttr, fnLen);
                    fullLevelName[fnLen] = 0;

                    // Check if clean prefix matches fullName
                    if (strcmp(fullLevelName, cleanPrefix) == 0) {
                        foundLevel = true;
                        strncpy(outData.levelFullName, fullLevelName, sizeof(outData.levelFullName) - 1);
                        outData.levelFullName[sizeof(outData.levelFullName) - 1] = 0;
                    }
                }
                searchPos = fnEnd + 1;
            }

            outData.hasCustom = !foundLevel;
            delete[] buf;
        }
        else fclose(f);
    }
    else {
        // Can't read XML - assume custom
        outData.hasCustom = true;
    }

    int lastIndex = GetLastUsedAutoSaveIndex(profileName, folderName);

    uint32_t autosave_limit = *(uint8_t*)0x0057BCBD;

    int newIndex = 0;
    if (outData.hasCustom) {
        newIndex = 0; // Custom saves don't need index
    }
    else {
        newIndex = lastIndex + 1;
        if (newIndex > (int)autosave_limit)
            newIndex = 1;
        if (newIndex < 1) newIndex = 1;
    }

    outData.index = newIndex;

    if (outData.hasCustom) {
        // Custom save: remove autosave label from prefix if present
        const char* autoPos = strstr(prefix, g_autoSaveLabel);
        if (autoPos) {
            int cleanLen = (int)(autoPos - prefix);
            char cleanPrefix[256];
            memcpy(cleanPrefix, prefix, cleanLen);
            // Trim trailing space
            while (cleanLen > 0 && cleanPrefix[cleanLen - 1] == ' ') cleanLen--;
            cleanPrefix[cleanLen] = 0;
            sprintf(g_correctedName, "%s", cleanPrefix);
        }
        else {
            sprintf(g_correctedName, "%s", prefix);
        }
    }
    else if (outData.levelFullName[0]) {
        sprintf(g_correctedName, "%s %s %d", outData.levelFullName, g_autoSaveLabel, newIndex);
    }
    else {
        sprintf(g_correctedName, "%s %d", prefix, newIndex);
    }
}

// PatchSaveInfoXml:
// Patch SaveInfo.xml after the game writes it
// Replaces the Name attribute with the corrected name and adds
// UnnamedAutoSaveIndex and IsAutoSave attributes.

// UnnamedAutoSaveIndex used for numbering autosaves without a custom name.
// IsAutoSave just a flag that this is a AutoSave. Anyway this is a request from E Jet.

static void PatchSaveInfoXml(const PendingSaveData& data)
{
    if (data.folderName[0] == 0) return;
    if (data.profileName[0] == 0) return;

    char xmlPath[512];
    sprintf(xmlPath, "data/profiles/%s/saves/%s/SaveInfo.xml",
        data.profileName, data.folderName);

    FILE* f = fopen(xmlPath, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz <= 0) { fclose(f); return; }
    fseek(f, 0, SEEK_SET);
    char* buf = new char[sz + 1];
    size_t bytesRead = fread(buf, 1, (size_t)sz, f);
    buf[sz] = 0;
    fclose(f);

    if (bytesRead < (size_t)sz) { delete[] buf; return; }

    // Build the final save name using pre-computed values
    char newName[512] = {};
    if (data.hasCustom) {
        // Custom-named save: remove autosave label from prefix if present
        const char* autoPos = strstr(data.prefix, g_autoSaveLabel);
        if (autoPos) {
            int cleanLen = (int)(autoPos - data.prefix);
            char cleanPrefix[256];
            memcpy(cleanPrefix, data.prefix, cleanLen);
            while (cleanLen > 0 && cleanPrefix[cleanLen - 1] == ' ') cleanLen--;
            cleanPrefix[cleanLen] = 0;
            sprintf(newName, "%s", cleanPrefix);
        }
        else {
            sprintf(newName, "%s", data.prefix);
        }
    }
    else if (data.levelFullName[0]) {
        sprintf(newName, "%s %s %d", data.levelFullName, g_autoSaveLabel, data.index);
    }
    else {
        // No level full name - prefix already contains "LevelName AutoSave"
        sprintf(newName, "%s %d", data.prefix, data.index);
    }

    // Rebuild XML with patched Name and new attributes
    char* nameAttr = strstr(buf, "Name=\"");
    if (!nameAttr) { delete[] buf; return; }
    nameAttr += 6;
    char* nameEnd = strchr(nameAttr, '"');
    if (!nameEnd) { delete[] buf; return; }

    char* lvlAttr2 = strstr(buf, "LevelName=\"");
    char* lvlEnd2 = nullptr;
    if (lvlAttr2) {
        lvlAttr2 += 11;
        lvlEnd2 = strchr(lvlAttr2, '"');
    }

    char* tagClose = strchr(nameEnd, '>');
    if (!tagClose) { delete[] buf; return; }

    // Find <GameTime
    char* gameTime = strstr(buf, "<GameTime");
    if (!gameTime) { delete[] buf; return; }

    // Extract LevelName value if present
    char levelNameVal[256] = {};
    if (lvlAttr2 && lvlEnd2) {
        int lvlLen = (int)(lvlEnd2 - lvlAttr2);
        if (lvlLen > 0 && lvlLen < (int)sizeof(levelNameVal)) {
            memcpy(levelNameVal, lvlAttr2, lvlLen);
            levelNameVal[lvlLen] = 0;
        }
    }

    // Rebuild the SaveInfo opening tag
    char newBuf[1024];
    int pos = 0;
    pos += sprintf(newBuf + pos, "<SaveInfo\n\tName=\"%s\"", newName);
    if (levelNameVal[0]) {
        pos += sprintf(newBuf + pos, "\n\tLevelName=\"%s\"", levelNameVal);
    }
    if (!data.hasCustom) {
        pos += sprintf(newBuf + pos, "\n\tUnnamedAutoSaveIndex=\"%d\"\n\tIsAutoSave=\"True\">", data.index);
    }
    else {
        pos += sprintf(newBuf + pos, ">");
    }
    pos += sprintf(newBuf + pos, "\n\t");

    // Find end of opening tag
    char* tagEnd = strchr(tagClose, '\n');
    if (!tagEnd) tagEnd = tagClose + 1;
    else tagEnd++;

    // Append rest of file from gameTime
    strcat(newBuf, gameTime);

    FILE* fw = fopen(xmlPath, "wb");
    if (fw) {
        fwrite(newBuf, 1, strlen(newBuf), fw);
        fclose(fw);
    }

    delete[] buf;
}

// ReloadSaveInfos:
// Reload save list in SavesManager
// Forces the UI to refresh after patching XML.
// (Now called from PatchThread using data->savesMgrThis)
static void ReloadSaveInfos()
{
    if (!g_savesMgrThis) return;
    void* mgr = g_savesMgrThis;
    __asm {
        mov  ecx, mgr
        call LoadInfosFunc
    }
}

// EnableAutosaveSuppression:
// Enables the suppression of game's own FadingMsg for avoiding two FadingMsg (game's and custom one) 
// at once on AutoSave

static void EnableAutosaveSuppression()
{
    injector::WriteMemory<uint8_t>(0x0057C554 + 0, 0x83, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 1, 0xC4, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 2, 0x08, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 3, 0x90, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 4, 0x90, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 5, 0x90, true);
}

// DisableAutosaveSuppression:
// Disables the suppression of game's own FadingMsg. Disables when save type is manual or quick

static void DisableAutosaveSuppression()
{
    injector::WriteMemory<uint8_t>(0x0057C554 + 0, 0xFF, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 1, 0x92, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 2, 0xB0, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 3, 0x00, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 4, 0x00, true);
    injector::WriteMemory<uint8_t>(0x0057C554 + 5, 0x00, true);
}

// PrepareFolderName:
// Prepares autosave renaming context before SaveGame executes.
// Captures the target autosave folder name, resolves the corresponding profile,
// and extracts the save prefix (map/custom name) from the provided data.
// Then computes the corrected save name and schedules a patch operation.

static void __cdecl PrepareFolderName(void* ediVal, void* ecxVal, void* ebpVal)
{
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

    // Allocate data on heap so it won't be overwritten by subsequent calls
    PendingSaveData* data = new PendingSaveData();
    memset(data, 0, sizeof(PendingSaveData));

    strncpy(data->folderName, folderPtr, sizeof(data->folderName) - 1);
    data->folderName[sizeof(data->folderName) - 1] = 0;

    // Resolve profile
    FindProfileByFolder(data->folderName, data->profileName, sizeof(data->profileName));

    void* ebpContent = nullptr;
    if (ebpVal && !IsBadReadPtr(ebpVal, 4))
        ebpContent = *(void**)ebpVal;

    if (ebpContent && !IsBadReadPtr(ebpContent, 1)) {
        char* fullStr = (char*)ebpContent;
        if (!IsBadReadPtr(fullStr, 1) && fullStr[0] != 0) {
            // Find the last occurrence of " <digits>" at the end of string
            int len = (int)strlen(fullStr);
            int lastSpacePos = -1;

            // Scan backwards to find the last space followed by a number
            for (int i = len - 1; i >= 0; i--) {
                if (fullStr[i] == ' ') {
                    // Check if everything after space is a number
                    bool allDigits = true;
                    for (int j = i + 1; j < len; j++) {
                        if (fullStr[j] < '0' || fullStr[j] > '9') {
                            allDigits = false;
                            break;
                        }
                    }
                    if (allDigits && i + 1 < len) {
                        lastSpacePos = i;
                        break;
                    }
                }
            }

            if (lastSpacePos > 0) {
                // Extract prefix (everything before " <number>")
                int prefLen = lastSpacePos;
                if (prefLen > 0 && prefLen < (int)sizeof(data->prefix)) {
                    memcpy(data->prefix, fullStr, prefLen);
                    data->prefix[prefLen] = 0;
                }
            }
            else {
                // No trailing number - use entire string as prefix (custom save)
                int copyLen = min(len, (int)sizeof(data->prefix) - 1);
                memcpy(data->prefix, fullStr, copyLen);
                data->prefix[copyLen] = 0;
            }
        }
    }

    data->savesMgrThis = ecxVal;

    ComputeCorrectedName(data->folderName, data->prefix, data->profileName, *data);

    // Store in global for patching after save
    memcpy(&g_pendingSaveData, data, sizeof(PendingSaveData));
    g_hasPendingSave = true;

    delete data;
}

// HookBeforeSaveGame:
// Hook at SavesManager::SaveGame call site
// Intercepts the call to SaveGame, captures stack state, then jumps to original.
// eax = ebpVal (from stack)
// ecx = this (SavesManager*)
// edi = pointer to save folder name

__declspec(naked) static void HookBeforeSaveGame()
{
    __asm {
        pushad
        mov  eax, [esp + 2Ch]
        push eax
        push ecx
        push edi
        call PrepareFolderName
        add  esp, 12
        popad
        jmp  OrigFunc
    }
}

// AfterSaveHookImpl:
// Show corrected FadingMsg after save completes
// Called from HookAfterSave in the main thread, after SaveGame succeeds.

static void AfterSaveHookImpl()
{
    // Patch XML immediately
    if (g_hasPendingSave && g_pendingSaveData.folderName[0] != 0) {
        PendingSaveData copy = g_pendingSaveData;
        g_hasPendingSave = false;
        PatchSaveInfoXml(copy);
        if (copy.savesMgrThis) {
            void* mgr = copy.savesMgrThis;
            __asm {
                mov  ecx, mgr
                call LoadInfosFunc
            }
        }
    }

    if (g_correctedName[0] != '\0')
    {
        CallAddImportantFadingMsgFormatted("GameWasSaved", g_correctedName);
        g_correctedName[0] = '\0';
    }
}

// HookAfterSave:
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

// InitSaveLimits:
// Init all patches to save limits, HookBeforeSaveGame and HookAfterSave

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
}