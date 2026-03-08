#pragma once
#include "ThirdParty/Injector/injector.hpp"
#include "ThirdParty/IniReader/IniReader.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

static char g_autoSaveLabel[128] = "Autosave";
static char g_pendingFolderName[64] = {};
static char g_pendingPrefix[256] = {};
static void* g_savesMgrThis = nullptr;
static bool g_isComRem = false;

static const uintptr_t OrigFunc = 0x0057C5A0;
static const uintptr_t LoadInfosFunc = 0x0057D310;

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

static void PatchSaveInfoXml()
{
    if (g_pendingFolderName[0] == 0) return;

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

    bool hasCustom = false;
    char customName[256] = {};

    if (g_pendingPrefix[0] && levelFullName[0]) {
        if (strcmp(g_pendingPrefix, levelFullName) != 0) {
            hasCustom = true;
            int mapLen = (int)strlen(levelFullName);
            if (strncmp(g_pendingPrefix, levelFullName, mapLen) == 0
                && g_pendingPrefix[mapLen] == ' ') {
                strncpy(customName, g_pendingPrefix + mapLen + 1, sizeof(customName) - 1);
            }
            else {
                strncpy(customName, g_pendingPrefix, sizeof(customName) - 1);
            }
        }
    }

    int maxIndex = GetMaxUnnamedAutoSaveIndex(profileName, g_pendingFolderName);
    int newIndex = hasCustom ? maxIndex : maxIndex + 1;
    if (newIndex < 1) newIndex = 1;

    char newName[512] = {};
    if (hasCustom) {
        sprintf(newName, "%s", customName);
    }
    else {
        if (levelFullName[0])
            sprintf(newName, "%s %s %d", levelFullName, g_autoSaveLabel, newIndex);
        else
            sprintf(newName, "%s %d", g_autoSaveLabel, newIndex);
    }

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

static void ReloadSaveInfos()
{
    if (!g_savesMgrThis) return;
    void* mgr = g_savesMgrThis;
    __asm {
        mov     ecx, mgr
        call    LoadInfosFunc
    }
}

static DWORD WINAPI PatchThread(LPVOID)
{
    Sleep(500);
    PatchSaveInfoXml();
    ReloadSaveInfos();
    return 0;
}

static void __cdecl PrepareFolderName(void* ediVal, void* ecxVal, void* ebpVal)
{
    g_pendingFolderName[0] = 0;
    g_pendingPrefix[0] = 0;
    g_savesMgrThis = ecxVal;

    if (!ediVal) return;
    char* folderPtr = *(char**)ediVal;
    if (!folderPtr) return;
    if (strncmp(folderPtr, "auto_", 5) != 0) return;

    strncpy(g_pendingFolderName, folderPtr, sizeof(g_pendingFolderName) - 1);

    void* ebpContent = nullptr;
    if (ebpVal && !IsBadReadPtr(ebpVal, 4))
        ebpContent = *(void**)ebpVal;

    if (ebpContent && !IsBadReadPtr(ebpContent, 4)) {
        char* fullStr = (char*)ebpContent;
        if (!IsBadReadPtr(fullStr, 1) && fullStr[0] != 0) {
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

    HANDLE hThread = CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

__declspec(naked) static void HookBeforeSaveGame()
{
    __asm {
        pushad
        mov     eax, [esp + 2Ch]
        push    eax
        push    ecx
        push    edi
        call    PrepareFolderName
        add     esp, 12
        popad
        jmp     OrigFunc
    }
}

void InitSaveLimits()
{
    CIniReader iniReader("VariousHacks.ini");

    uint32_t quicksave_limit = (uint32_t)iniReader.ReadInteger("GENERAL", "QuicksaveLimit", 5);
    uint32_t autosave_limit = (uint32_t)iniReader.ReadInteger("GENERAL", "AutosaveLimit", 10);

    if (quicksave_limit < 1)   quicksave_limit = 1;
    if (quicksave_limit > 255) quicksave_limit = 255;
    if (autosave_limit < 1)    autosave_limit = 1;
    if (autosave_limit > 255)  autosave_limit = 255;

    injector::WriteMemory<uint32_t>(0x0057BCB6, quicksave_limit);
    injector::WriteMemory<uint32_t>(0x0057BCBD, autosave_limit);

    DetectGameVersion();
    LoadAutoSaveLabel();

    injector::MakeCALL(0x0057C309, HookBeforeSaveGame, true);
}