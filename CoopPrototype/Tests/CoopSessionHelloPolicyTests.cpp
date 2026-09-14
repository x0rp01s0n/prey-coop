#include "CoopSessionHelloPolicy.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <set>

namespace
{
using CoopSessionHelloPolicy::Decision;
using CoopSessionHelloPolicy::Freshness;

struct Hello
{
    uint32_t sequence;
    uint64_t runtimeNonce;
    float time;
    uint32_t address;
    uint16_t port;
};

class PeerBook
{
public:
    Decision Receive(const Hello& hello, bool admissionAuthorized = true)
    {
        const bool newPeer = !m_hasActive;
        const bool useTombstone = CoopSessionHelloPolicy::ShouldUseTombstone(
            m_hasActive,
            m_active,
            m_hasTombstone);
        const Freshness previous = useTombstone
            ? m_tombstone
            : (m_hasActive ? m_active : Freshness{});
        const bool nonceRetired = hello.runtimeNonce != 0 &&
            m_retiredNonces.find(hello.runtimeNonce) != m_retiredNonces.end();
        const float secondsSinceHello = previous.acceptedAt < 0.0f
            ? -1.0f
            : hello.time - previous.acceptedAt;
        const Decision preliminary = useTombstone
            ? CoopSessionHelloPolicy::ClassifyTombstoneRecovery(
                hello.runtimeNonce,
                previous.runtimeNonce,
                nonceRetired,
                secondsSinceHello,
                true)
            : CoopSessionHelloPolicy::Classify(
                hello.sequence,
                previous.sequence,
                hello.runtimeNonce,
                previous.runtimeNonce,
                nonceRetired,
                secondsSinceHello,
                false,
                true);
        if (preliminary == Decision::Reject ||
            !CoopSessionHelloPolicy::EndpointChangeAllowed(
                CoopSessionHelloPolicy::MatchesEndpoint(previous, hello.address, hello.port),
                preliminary))
        {
            return Decision::Reject;
        }

        const bool admissionRequired = newPeer || useTombstone || preliminary == Decision::Restart;
        const Decision accepted = useTombstone
            ? CoopSessionHelloPolicy::ClassifyTombstoneRecovery(
                hello.runtimeNonce,
                previous.runtimeNonce,
                nonceRetired,
                secondsSinceHello,
                admissionAuthorized)
            : CoopSessionHelloPolicy::Classify(
                hello.sequence,
                previous.sequence,
                hello.runtimeNonce,
                previous.runtimeNonce,
                nonceRetired,
                secondsSinceHello,
                admissionRequired,
                admissionAuthorized);
        if (accepted == Decision::Reject)
            return accepted;

        if (accepted == Decision::Restart && previous.runtimeNonce != 0)
            m_retiredNonces.insert(previous.runtimeNonce);
        m_active.runtimeNonce = CoopSessionHelloPolicy::RuntimeNonceAfterAcceptedHello(
            hello.runtimeNonce,
            previous.runtimeNonce);
        m_active.sequence = hello.sequence;
        m_active.acceptedAt = hello.time;
        m_active.address = hello.address;
        m_active.port = hello.port;
        m_hasActive = true;
        m_hasTombstone = false;
        return accepted;
    }

    void Remove()
    {
        if (!m_hasActive)
            return;
        m_tombstone = m_active;
        m_hasTombstone = true;
        if (m_active.runtimeNonce != 0)
            m_retiredNonces.insert(m_active.runtimeNonce);
        m_hasActive = false;
    }

    void AddRosterPlaceholder()
    {
        m_active = Freshness{};
        m_hasActive = true;
    }

    const Freshness& Active() const { return m_active; }
    bool HasActive() const { return m_hasActive; }

private:
    Freshness m_active;
    Freshness m_tombstone;
    std::set<uint64_t> m_retiredNonces;
    bool m_hasActive = false;
    bool m_hasTombstone = false;
};

void TestEndpointRebindRequiresFreshRuntimeAndAdmission()
{
    PeerBook peers;
    assert(peers.Receive({40, 11, 1.0f, 0x01020304u, 3000}) == Decision::Accept);

    assert(peers.Receive({1, 22, 2.99f, 0x05060708u, 4000}) == Decision::Reject);
    assert(peers.Active().runtimeNonce == 11);
    assert(peers.Active().address == 0x01020304u);

    assert(peers.Receive({1, 22, 3.0f, 0x05060708u, 4000}, false) == Decision::Reject);
    assert(peers.Active().runtimeNonce == 11);
    assert(peers.Active().address == 0x01020304u);

    assert(peers.Receive({1, 22, 3.0f, 0x05060708u, 4000}) == Decision::Restart);
    assert(peers.Active().runtimeNonce == 22);
    assert(peers.Active().address == 0x05060708u);

    assert(peers.Receive({41, 11, 4.0f, 0x01020304u, 3000}) == Decision::Reject);
    assert(peers.Receive({2, 22, 4.0f, 0x01020304u, 3000}) == Decision::Reject);
    assert(peers.Active().address == 0x05060708u);
}

void TestTombstoneRejectsStaleHelloAndAllowsExplicitRecovery()
{
    PeerBook peers;
    assert(peers.Receive({7, 31, 1.0f, 0x01020304u, 3000}) == Decision::Accept);
    assert(peers.Receive({8, 31, 1.1f, 0x01020304u, 3000}) == Decision::Accept);
    peers.Remove();
    assert(!peers.HasActive());

    assert(peers.Receive({8, 31, 5.0f, 0x01020304u, 3000}) == Decision::Reject);
    assert(peers.Receive({9, 31, 5.0f, 0x01020304u, 3000}) == Decision::Reject);
    assert(peers.Receive({1, 0, 5.0f, 0x01020304u, 3000}) == Decision::Reject);
    assert(peers.Receive({1, 32, 3.09f, 0x090a0b0cu, 5000}) == Decision::Reject);
    assert(peers.Receive({1, 32, 3.1f, 0x090a0b0cu, 5000}, false) == Decision::Reject);
    peers.AddRosterPlaceholder();
    assert(peers.Receive({10, 31, 5.0f, 0x01020304u, 3000}) == Decision::Reject);
    assert(peers.Receive({1, 32, 5.0f, 0x090a0b0cu, 5000}) == Decision::Restart);
    assert(peers.HasActive());
    assert(peers.Active().sequence == 1);
    assert(peers.Active().runtimeNonce == 32);

    peers.Remove();
    assert(peers.Receive({2, 32, 7.0f, 0x090a0b0cu, 5000}) == Decision::Reject);
    assert(peers.Receive({1, 33, 7.0f, 0x0d0e0f10u, 6000}) == Decision::Restart);
    assert(peers.Active().runtimeNonce == 33);
    assert(peers.Active().address == 0x0d0e0f10u);
    assert(peers.Receive({100, 31, 8.0f, 0x01020304u, 3000}) == Decision::Reject);
}
}

int main()
{
    TestEndpointRebindRequiresFreshRuntimeAndAdmission();
    TestTombstoneRejectsStaleHelloAndAllowsExplicitRecovery();
    std::cout << "SessionHello freshness behavior tests passed.\n";
}
