#include "CoopReliableReorderBuffer.h"

#include <cstdint>
#include <limits>
#include <vector>

#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
            return __LINE__; \
    } while (false)

struct Envelope
{
    uint32_t sequence = 0;
    bool crossWorldControl = false;
};

struct Receiver
{
    void Receive(const Envelope& envelope, bool loadControlPump = false)
    {
        if (envelope.sequence == CoopSerialSequence::Next(ackFrontier))
        {
            Consume(envelope, loadControlPump);
            Envelope next = {};
            while (reordered.TakeNext(ackFrontier, next))
                Consume(next, loadControlPump);
        }
        else if (CoopSerialSequence::IsAfter(envelope.sequence, ackFrontier))
        {
            reordered.Store(ackFrontier, envelope.sequence, envelope);
            sentAckFrontiers.push_back(ackFrontier);
        }
        else
        {
            sentAckFrontiers.push_back(ackFrontier);
        }
    }

    void ReceiveTransferStart(const Envelope& envelope)
    {
        if (envelope.sequence != CoopSerialSequence::Next(ackFrontier) &&
            CoopSerialSequence::IsAfter(envelope.sequence, ackFrontier) &&
            !CoopReliableReorder::IsWithinForwardWindow(ackFrontier, envelope.sequence))
        {
            ackFrontier = CoopSerialSequence::Previous(envelope.sequence);
            reordered.DiscardAtOrBefore(ackFrontier);
        }
        Receive(envelope);
    }

    void Consume(const Envelope& envelope, bool loadControlPump)
    {
        ackFrontier = envelope.sequence;
        if (loadControlPump && !envelope.crossWorldControl)
            retired.push_back(envelope.sequence);
        else
            applied.push_back(envelope.sequence);
        sentAckFrontiers.push_back(ackFrontier);
    }

    uint32_t ackFrontier = 0;
    CoopReliableReorder::Buffer<Envelope> reordered;
    std::vector<uint32_t> applied;
    std::vector<uint32_t> retired;
    std::vector<uint32_t> sentAckFrontiers;
};

int main()
{
    Receiver shuffled;
    for (uint32_t index = 0; index < CoopReliableReorder::kWindowCapacity - 1; ++index)
    {
        const uint32_t sequence = (index * 13) % (CoopReliableReorder::kWindowCapacity - 1) + 2;
        shuffled.Receive({sequence, false});
        CHECK(shuffled.ackFrontier == 0);
    }

    CHECK(shuffled.reordered.Size() == CoopReliableReorder::kWindowCapacity - 1);
    shuffled.Receive({1, false});
    CHECK(shuffled.ackFrontier == CoopReliableReorder::kWindowCapacity);
    CHECK(shuffled.applied.size() == CoopReliableReorder::kWindowCapacity);
    for (uint32_t index = 0; index < shuffled.applied.size(); ++index)
        CHECK(shuffled.applied[index] == index + 1);
    CHECK(shuffled.sentAckFrontiers.size() == 2 * CoopReliableReorder::kWindowCapacity - 1);
    for (uint32_t index = 0; index < CoopReliableReorder::kWindowCapacity - 1; ++index)
        CHECK(shuffled.sentAckFrontiers[index] == 0);
    for (uint32_t index = 0; index < CoopReliableReorder::kWindowCapacity; ++index)
        CHECK(shuffled.sentAckFrontiers[CoopReliableReorder::kWindowCapacity - 1 + index] == index + 1);

    Receiver duplicate;
    duplicate.Receive({3, false});
    duplicate.Receive({3, false});
    CHECK(duplicate.reordered.Size() == 1);
    duplicate.Receive({2, false});
    duplicate.Receive({1, false});
    CHECK(duplicate.ackFrontier == 3);
    CHECK(duplicate.applied.size() == 3);
    CHECK(duplicate.applied[0] == 1 && duplicate.applied[1] == 2 && duplicate.applied[2] == 3);
    CHECK(duplicate.reordered.Size() == 0);

    Receiver bounded;
    bounded.Receive({static_cast<uint32_t>(CoopReliableReorder::kWindowCapacity + 1), false});
    CHECK(bounded.ackFrontier == 0);
    CHECK(bounded.reordered.Size() == 0);
    CHECK(bounded.sentAckFrontiers.back() == 0);
    CHECK(!CoopReliableReorder::IsWithinForwardWindow(0, CoopReliableReorder::kWindowCapacity + 1));

    Receiver loadControl;
    loadControl.Receive({2, true}, true);
    CHECK(loadControl.ackFrontier == 0);
    loadControl.Receive({1, false}, true);
    CHECK(loadControl.ackFrontier == 2);
    CHECK(loadControl.retired.size() == 1 && loadControl.retired[0] == 1);
    CHECK(loadControl.applied.size() == 1 && loadControl.applied[0] == 2);
    CHECK(loadControl.sentAckFrontiers[0] == 0);
    CHECK(loadControl.sentAckFrontiers[1] == 1);
    CHECK(loadControl.sentAckFrontiers[2] == 2);

    uint32_t nextSequence = 0;
    CoopReliableReorder::Buffer<uint32_t> transferStart;
    CHECK(transferStart.Store(0, 9, 9));
    CHECK(transferStart.Store(0, 11, 11));
    transferStart.DiscardAtOrBefore(9);
    CHECK(transferStart.Size() == 1);
    CHECK(transferStart.TakeNext(10, nextSequence));
    CHECK(nextSequence == 11);

    CoopReliableReorder::Buffer<uint32_t> wrapping;
    uint32_t wrapFrontier = std::numeric_limits<uint32_t>::max() - 1;
    CHECK(wrapping.Store(wrapFrontier, 1, 1));
    wrapFrontier = std::numeric_limits<uint32_t>::max();
    CHECK(wrapping.TakeNext(wrapFrontier, nextSequence));
    CHECK(nextSequence == 1);

    Receiver transferStartInWindow;
    for (uint32_t sequence = 1; sequence <= 473; ++sequence)
        transferStartInWindow.Receive({sequence, false});
    CHECK(CoopReliableReorder::IsWithinForwardWindow(473, 488));
    transferStartInWindow.ReceiveTransferStart({488, true});
    CHECK(transferStartInWindow.ackFrontier == 473);
    CHECK(transferStartInWindow.reordered.Size() == 1);
    for (uint32_t sequence = 474; sequence < 488; ++sequence)
        transferStartInWindow.Receive({sequence, false});
    CHECK(transferStartInWindow.ackFrontier == 488);
    CHECK(transferStartInWindow.applied.size() == 488);
    CHECK(transferStartInWindow.applied[487] == 488);

    Receiver transferStartOutsideWindow;
    for (uint32_t sequence = 1; sequence <= 473; ++sequence)
        transferStartOutsideWindow.Receive({sequence, false});
    CHECK(!CoopReliableReorder::IsWithinForwardWindow(473, 506));
    transferStartOutsideWindow.ReceiveTransferStart({506, true});
    CHECK(transferStartOutsideWindow.ackFrontier == 506);
    CHECK(transferStartOutsideWindow.applied.back() == 506);
    return 0;
}
