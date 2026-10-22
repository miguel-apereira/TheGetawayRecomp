// SPDX-License-Identifier: MIT
#include "app/Log.h"

#include <cstdio>
#include <mutex>

namespace tgr::log
{

namespace
{
std::mutex g_mutex;

const char* prefix(Level level)
{
    switch (level)
    {
    case Level::Debug:
        return "[debug]";
    case Level::Info:
        return "[info] ";
    case Level::Warn:
        return "[warn] ";
    case Level::Error:
        return "[error]";
    }
    return "[?]    ";
}
} // namespace

void write(Level level, std::string_view message)
{
    std::lock_guard lock(g_mutex);
    std::FILE* out = level >= Level::Warn ? stderr : stdout;
    std::fprintf(out, "%s %.*s\n", prefix(level), static_cast<int>(message.size()), message.data());
    std::fflush(out);
}

} // namespace tgr::log
