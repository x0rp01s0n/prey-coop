#pragma once

#include "CoopFilesystem.h"

#include <filesystem>

#include <Prey/CrySystem/IConsole.h>
#include <Prey/CrySystem/ISystem.h>

namespace CoopProfilePath
{
inline std::filesystem::path GetPreyProfileRoot()
{
    std::filesystem::path savedGames = CoopFilesystem::EnvironmentPath("USERPROFILE");
    if (savedGames.empty())
        return {};

    savedGames /= "Saved Games";
    if (gEnv && gEnv->pConsole)
    {
        if (ICVar* userFolder = gEnv->pConsole->GetCVar("sys_user_folder"))
        {
            const char* value = userFolder->GetString();
            if (value && value[0])
            {
                const std::filesystem::path configured = CoopFilesystem::FromUtf8(value);
                return configured.is_absolute() ? configured : savedGames / configured;
            }
        }
    }

    return savedGames / "Arkane Studios" / "Prey";
}
}
