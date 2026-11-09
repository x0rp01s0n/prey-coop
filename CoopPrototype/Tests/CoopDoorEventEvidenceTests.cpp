#include "../Src/CoopAreaObjectJournal.h"
#include "../Src/CoopDoorRangePolicy.h"
#include "../Src/CoopReliableReorderBuffer.h"

#include <cassert>
#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char** argv)
{
    assert(argc == 2);
    using namespace CoopProtocol;
    using CoopDoorRangePolicy::AcceptNetworkEdge;
    static_assert(sizeof(AreaObjectEventPacket) == 184, "Overlap proof must not grow the packet");
    static_assert((kAreaObjectDoorStateFlagSenderInside & kAreaObjectDoorStateFlagMask) != 0);

    // The native event may precede its supporting position packet.
    assert(!AcceptNetworkEdge(true, false, false, false));
    assert(AcceptNetworkEdge(true, false, false, true));
    for (const bool occupied : {false, true})
        for (const bool explicitOpen : {false, true})
            for (const bool senderInside : {false, true})
            {
                assert(AcceptNetworkEdge(true, occupied, explicitOpen, senderInside) ==
                    (occupied || explicitOpen || senderInside));
                // An outside sender cannot close a door on another occupant.
                assert(AcceptNetworkEdge(false, occupied, explicitOpen, senderInside) == !occupied);
            }

    AreaObjectEventPacket open;
    open.eventKind = kAreaObjectEventDoorOpen;
    open.eventId = 41;
    open.levelId = 7;
    open.targetGuid = 9;
    open.sourcePeerHash = 11;
    open.postVersion = 3;
    open.value = 1;
    open.flags = kAreaObjectDoorStateFlagSnapshot | kAreaObjectDoorStateFlagPowered |
        kAreaObjectDoorStateFlagExplicitOpen | kAreaObjectDoorStateFlagSenderInside;
    const auto durableFlags = open.flags & ~kAreaObjectDoorStateFlagSenderInside;
    CoopAreaObjectJournal journal;
    assert(journal.Record("hardware", open));
    assert((open.flags & kAreaObjectDoorStateFlagSenderInside) != 0);
    std::ostringstream stored;
    assert(journal.ExportLevelJsonl("hardware", stored));
    const auto path = std::filesystem::path(argv[1]) / "door-journal.jsonl";
    {
        std::ofstream file(path);
        file << stored.str();
        assert(file.good());
    }
    CoopAreaObjectJournal replay;
    CoopAreaObjectJournal::ReplayStats stats;
    auto check = [&](const AreaObjectEventPacket& packet, std::string&) {
        assert(packet.flags == durableFlags);
        assert(packet.eventId == open.eventId && packet.targetGuid == open.targetGuid);
        assert(packet.sourcePeerHash == open.sourcePeerHash && packet.postVersion == open.postVersion);
        return true;
    };
    assert(replay.ReplayLevelJsonl(path, "hardware", check, stats));
    assert(stats.applied == 1 && stats.rejected == 0);

    // Even an input journal containing the live bit must not replay that proof.
    auto stale = stored.str();
    const auto flagsKey = std::string("\"flags\":") + std::to_string(durableFlags);
    const auto flagsOffset = stale.find(flagsKey);
    assert(flagsOffset != std::string::npos);
    stale.replace(flagsOffset, flagsKey.size(), "\"flags\":" + std::to_string(open.flags));
    {
        std::ofstream file(path);
        file << stale;
        assert(file.good());
    }
    replay.Reset();
    assert(replay.ReplayLevelJsonl(path, "hardware", check, stats));
    assert(stats.applied == 1);

    // The bit is not globally reserved in unrelated event payloads.
    auto other = open;
    other.eventKind = kAreaObjectEventDoorLocked;
    journal.Reset();
    assert(journal.Record("hardware", other));
    std::ostringstream otherStored;
    assert(journal.ExportLevelJsonl("hardware", otherStored));
    {
        std::ofstream file(path);
        file << otherStored.str();
        assert(file.good());
    }
    replay.Reset();
    assert(replay.ReplayLevelJsonl(path, "hardware", [&](const auto& packet, std::string&) {
        assert(packet.flags == other.flags);
        return true;
    }, stats));

    // Use the production reorder buffer: a delayed old Open cannot overtake Close.
    CoopReliableReorder::Buffer<AreaObjectEventPacket> ordered;
    auto close = open;
    close.value = 0;
    close.flags &= ~kAreaObjectDoorStateFlagSenderInside;
    close.eventId = 42;
    uint32_t frontier = 0;
    AreaObjectEventPacket next;
    assert(ordered.Store(frontier, 2, close));
    assert(!ordered.TakeNext(frontier, next));
    assert(ordered.Store(frontier, 1, open));
    assert(!ordered.Store(frontier, 1, open));
    assert(ordered.TakeNext(frontier, next) && next.value == 1);
    ++frontier;
    assert(ordered.TakeNext(frontier, next) && next.value == 0);
    ++frontier;
    assert(!ordered.Store(frontier, 1, open));
    assert(!ordered.Store(frontier, 2, close));
    assert(ordered.Size() == 0);
    std::cout << "Door evidence, journal sanitization, packet size and ordered delivery: PASS\n";
}
