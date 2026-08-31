#include "CoopHookStats.h"

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace CoopHookStats
{
namespace
{
constexpr std::size_t kMaxSlots = 48;

std::vector<std::unique_ptr<Slot>>& Registry()
{
    static std::vector<std::unique_ptr<Slot>> registry;
    return registry;
}

uint64_t NowNs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}
}

Slot* Register(const char* name)
{
    auto& registry = Registry();
    if (registry.size() >= kMaxSlots || !name || !name[0])
        return nullptr;
    registry.push_back(std::make_unique<Slot>());
    Slot& slot = *registry.back();
    slot.name = name;
    return &slot;
}

Scoped::Scoped(Slot* slot)
    : m_slot(slot)
    , m_startNs(NowNs())
{
}

Scoped::~Scoped()
{
    if (!m_slot)
        return;
    const uint64_t elapsed = NowNs() - m_startNs;
    m_slot->calls.fetch_add(1, std::memory_order_relaxed);
    m_slot->totalNs.fetch_add(elapsed, std::memory_order_relaxed);
}

std::string BuildReport()
{
    auto& registry = Registry();
    std::string out;
    for (const auto& slotPtr : registry)
    {
        const Slot& slot = *slotPtr;
        const uint64_t calls = slot.calls.load(std::memory_order_relaxed);
        if (calls == 0)
            continue;
        const uint64_t ns = slot.totalNs.load(std::memory_order_relaxed);
        if (!out.empty())
            out += '|';
        out += slot.name;
        out += ':';
        out += std::to_string(calls);
        out += ':';
        out += std::to_string(ns / 1000); // microseconds
    }
    return out;
}

void ResetAll()
{
    for (auto& slotPtr : Registry())
    {
        slotPtr->calls.store(0, std::memory_order_relaxed);
        slotPtr->totalNs.store(0, std::memory_order_relaxed);
    }
}
}
