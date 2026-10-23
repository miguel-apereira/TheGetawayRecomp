// SPDX-License-Identifier: MIT
//
// tgr-profile: a small sampling profiler for a running TheGetawayRecomp.exe
// (or any other process of the same user). It needs no admin rights, unlike
// xperf or the Visual Studio profiler.
//
//   tgr-profile <pid> [seconds=20] [top=30]
//
// About once a millisecond it suspends every thread of the process, records
// the instruction pointer and resumes it. At the end it resolves the samples
// to functions with DbgHelp (so the exe's .pdb must sit next to it) and
// prints the hottest functions of each thread that was not idle.

#include <windows.h>

#include <dbghelp.h>
#include <timeapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct ThreadSamples
{
    HANDLE handle = nullptr;
    std::map<DWORD64, unsigned> byAddress;
};

// Opens every thread of the process that is not open yet.
void addNewThreads(DWORD pid, std::map<DWORD, ThreadSamples>& threads)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != pid || threads.contains(entry.th32ThreadID))
            continue;
        const DWORD access = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION;
        if (HANDLE thread = OpenThread(access, FALSE, entry.th32ThreadID))
            threads[entry.th32ThreadID].handle = thread;
    }
    CloseHandle(snapshot);
}

std::string functionName(HANDLE process, DWORD64 address)
{
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (SymFromAddr(process, address, &displacement, symbol))
        return symbol->Name;
    IMAGEHLP_MODULE64 module{};
    module.SizeOfStruct = sizeof(module);
    if (SymGetModuleInfo64(process, address, &module))
        return std::string(module.ModuleName) + "!?";
    return "?";
}

bool isKernelWait(const std::string& name)
{
    return name.starts_with("Nt") || name.starts_with("Zw");
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fputs("usage: tgr-profile <pid> [seconds=20] [top=30]\n", stderr);
        return 2;
    }
    const DWORD pid = std::strtoul(argv[1], nullptr, 0);
    const double seconds = argc > 2 ? std::atof(argv[2]) : 20.0;
    const int top = argc > 3 ? std::atoi(argv[3]) : 30;

    HANDLE process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!process)
    {
        std::fprintf(stderr, "Cannot open process %lu (error %lu)\n", pid, GetLastError());
        return 1;
    }

    std::map<DWORD, ThreadSamples> threads;
    const ULONGLONG end = GetTickCount64() + static_cast<ULONGLONG>(seconds * 1000);
    ULONGLONG nextThreadScan = 0;
    unsigned rounds = 0;
    timeBeginPeriod(1);
    while (GetTickCount64() < end)
    {
        if (GetTickCount64() >= nextThreadScan)
        {
            addNewThreads(pid, threads);
            nextThreadScan = GetTickCount64() + 1000;
        }
        for (auto& [id, thread] : threads)
        {
            if (SuspendThread(thread.handle) == static_cast<DWORD>(-1))
                continue;
            CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(thread.handle, &context))
                ++thread.byAddress[context.Rip];
            ResumeThread(thread.handle);
        }
        ++rounds;
        Sleep(1);
    }
    timeEndPeriod(1);

    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitialize(process, nullptr, TRUE))
        std::fprintf(stderr, "SymInitialize failed (error %lu); names will be missing\n", GetLastError());

    std::printf("%u sampling rounds over %.0f s\n", rounds, seconds);
    for (auto& [id, thread] : threads)
    {
        std::map<std::string, unsigned> byFunction;
        unsigned total = 0;
        for (const auto& [address, count] : thread.byAddress)
        {
            byFunction[functionName(process, address)] += count;
            total += count;
        }
        std::vector<std::pair<unsigned, std::string>> sorted;
        unsigned waiting = 0;
        for (auto& [name, count] : byFunction)
        {
            sorted.emplace_back(count, name);
            if (isKernelWait(name))
                waiting += count;
        }
        std::sort(sorted.rbegin(), sorted.rend());
        CloseHandle(thread.handle);

        const double waitingPercent = total ? 100.0 * waiting / total : 100.0;
        std::printf("\nThread %lu: %u samples, %.0f%% waiting in the kernel\n", id, total, waitingPercent);
        if (waitingPercent > 97.0)
            continue;
        for (int i = 0; i < top && i < static_cast<int>(sorted.size()); ++i)
            std::printf("%6.2f%%  %s\n", 100.0 * sorted[i].first / total, sorted[i].second.c_str());
    }
    SymCleanup(process);
    CloseHandle(process);
    return 0;
}
