#pragma once
#include "ThirdParty/Injector/injector.hpp"
#include "ThirdParty/IniReader/IniReader.h"

void InitSaveLimits()
{
    CIniReader iniReader("VariousHacks.ini");

    uint32_t quicksave_limit = (uint32_t)iniReader.ReadInteger("GENERAL", "QuicksaveLimit", 5);
    uint32_t autosave_limit  = (uint32_t)iniReader.ReadInteger("GENERAL", "AutosaveLimit", 10);

    if (quicksave_limit < 1)   quicksave_limit = 1;
    if (quicksave_limit > 255) quicksave_limit = 255;
    if (autosave_limit < 1)    autosave_limit = 1;
    if (autosave_limit > 255)  autosave_limit = 255;

    // mov [esi+68h], quicksave_limit
    injector::WriteMemory<uint32_t>(0x0057BCB6, quicksave_limit);

    // mov [esi+6Ch], autosave_limit
    injector::WriteMemory<uint32_t>(0x0057BCBD, autosave_limit);
}