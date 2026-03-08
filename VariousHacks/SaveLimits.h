#pragma once
#include "ThirdParty/Injector/injector.hpp"
#include "ThirdParty/IniReader/IniReader.h"
#include <cstdio>
#include <cstring>
#include <windows.h>

static char g_autoSaveLabel[128] = "Autosave";
static char g_pendingFolderName[64] = {};
static char g_pendingSaveName[256] = {};
static void* g_savesMgrThis = nullptr;

static const uintptr_t OrigFunc = 0x0057C5A0;
static const uintptr_t LoadInfosFunc = 0x0057D310;

static void LoadAutoSaveLabel()
{
    FILE* f = fopen("../data/if/strings/gamestrings.xml", "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buf = new char[sz + 1];
    fread(buf, 1, sz, f);
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

static void PatchSaveInfoXml()
{
    if (g_pendingFolderName[0] == 0) return;

    WIN32_FIND_DATAA fd;
    HANDLE hp = FindFirstFileA("data/profiles/*", &fd);
    if (hp == INVALID_HANDLE_VALUE) return;

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
    fseek(f, 0, SEEK_SET);
    char* buf = new char[sz + 1];
    fread(buf, 1, sz, f);
    buf[sz] = 0;
    fclose(f);

    char* nameAttr = strstr(buf, "Name=\"");
    if (nameAttr) {
        nameAttr += 6;
        char* nameEnd = strchr(nameAttr, '"');
        if (nameEnd) {
            char* trimEnd = nameEnd - 1;
            while (trimEnd > nameAttr && *trimEnd == ' ') trimEnd--;
            while (trimEnd > nameAttr && *trimEnd >= '0' && *trimEnd <= '9') trimEnd--;
            while (trimEnd > nameAttr && *trimEnd == ' ') trimEnd--;

            char newBuf[4096];
            int prefixLen = (int)(trimEnd - buf) + 1;
            int suffixStart = (int)(nameEnd - buf);

            memcpy(newBuf, buf, prefixLen);
            newBuf[prefixLen] = 0;

            char numStr[32];
            sprintf(numStr, " %d", atoi(g_pendingFolderName + 5) + 1);
            strcat(newBuf, numStr);
            strcat(newBuf, buf + suffixStart);

            FILE* fw = fopen(xmlPath, "wb");
            if (fw) {
                fwrite(newBuf, 1, strlen(newBuf), fw);
                fclose(fw);
            }
        }
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

static void __cdecl PrepareFolderName(void* ediVal, void* ecxVal)
{
    g_pendingFolderName[0] = 0;
    g_pendingSaveName[0] = 0;
    g_savesMgrThis = ecxVal;

    if (!ediVal) return;
    char* folderPtr = *(char**)ediVal;
    if (!folderPtr) return;
    if (strncmp(folderPtr, "auto_", 5) != 0) return;

    strncpy(g_pendingFolderName, folderPtr, sizeof(g_pendingFolderName) - 1);

    // проверяем saveName — если не пустой, нумерацию не добавляем
    char* saveNamePtr = nullptr;
    if (ecxVal)
        saveNamePtr = *(char**)ecxVal;

    if (saveNamePtr && saveNamePtr[0] != 0) {
        // имя передано — просто копируем без номера
        strncpy(g_pendingSaveName, saveNamePtr, sizeof(g_pendingSaveName) - 1);
    }
    else {
        // имя пустое — добавляем номер
        int num = atoi(folderPtr + 5) + 1;
        sprintf(g_pendingSaveName, "%s %d", g_autoSaveLabel, num);
    }

    HANDLE hThread = CreateThread(nullptr, 0, PatchThread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

__declspec(naked) static void HookBeforeSaveGame()
{
    __asm {
        pushad
        push    ecx
        push    edi
        call    PrepareFolderName
        add     esp, 8
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

    LoadAutoSaveLabel();

    injector::MakeCALL(0x0057C309, HookBeforeSaveGame, true);
}