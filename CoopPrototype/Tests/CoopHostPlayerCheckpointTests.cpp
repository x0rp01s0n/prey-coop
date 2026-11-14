#include "CoopPlayerStateStore.h"
#include "CoopProtocol.h"

#include <algorithm>
#include <cstdlib>
#include <charconv>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using CoopPlayerStateStore::Store;

namespace
{
int checks = 0;
fs::path fixtureRoot;
const std::string account = "player_account_0x29.state";
const std::string otherAccount = "player_account_0x2A.state";
const std::string fingerprintA = "1234_0xAAAAAAAA";
const std::string fingerprintB = "2345_0xBBBBBBBB";
const std::string fingerprintA2 = "3456_0xCCCCCCCC";

const std::string snapshotX =
    "fixture=valid\nversion=1\ninventory=QBeam:1,Gloo:1,Banana:7\n"
    "layout=QBeam:1,1:4,2;Gloo:5,1:3,1;Banana:8,1:1,1\n"
    "equipped=Gloo\nquickSelect=1:Gloo,2:QBeam\n"
    "receipt=host19:stable23:accepted1:committed1\nend=valid\n";
const std::string snapshotXY =
    "fixture=valid\nversion=1\ninventory=QBeam:1,Gloo:1,Banana:8,Pistol:1\n"
    "layout=QBeam:1,1:4,2;Gloo:5,1:3,1;Banana:8,1:1,1;Pistol:1,3:2,1\n"
    "equipped=Pistol\nquickSelect=1:Gloo,2:QBeam,3:Pistol\n"
    "receipt=host19:stable23:accepted1:committed1;host19:stable24:accepted1:committed1\n"
    "end=valid\n";
const std::string snapshotOther =
    "fixture=valid\nversion=1\ninventory=Wrench:1\nlayout=Wrench:1,1:2,1\n"
    "equipped=Wrench\nquickSelect=1:Wrench\nreceipt=none\nend=valid\n";

void Check(bool condition, const std::string& label)
{
    ++checks;
    if (!condition)
    {
        std::cerr << "FAIL " << label << '\n';
        std::exit(1);
    }
}

void CheckOk(bool result, const std::string& error, const std::string& label)
{
    Check(result, label + (error.empty() ? std::string() : ": " + error));
}

void Write(const fs::path& path, const std::string& bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out)
        throw std::runtime_error("cannot write test fixture " + path.string());
}

std::string Read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read test fixture " + path.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool Validate(const fs::path& path, std::string& error)
{
    const std::string bytes = Read(path);
    const std::string end = "end=valid\n";
    if (bytes.rfind("fixture=valid\n", 0) != 0 || bytes.size() < end.size() ||
        bytes.compare(bytes.size() - end.size(), end.size(), end) != 0)
    {
        error = "invalid fixture payload";
        return false;
    }
    return true;
}

bool ReadAndValidatePlayerSidecarFile(
    const std::string& path, std::string& payload, bool& hasHash, std::string& error)
{
    payload = Read(path);
    hasHash = Validate(path, error);
    return hasHash;
}

bool StartsWith(std::string_view value, std::string_view prefix)
{
    return value.substr(0, prefix.size()) == prefix;
}

std::string AfterPrefix(std::string_view value, std::string_view prefix)
{
    return std::string(value.substr(prefix.size()));
}

#include "HostCheckpointRecoveryActual.inc"

std::map<std::string, std::string> Files(const fs::path& root)
{
    std::map<std::string, std::string> result;
    if (!fs::exists(root))
        return result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
    {
        if (entry.is_regular_file() && !entry.is_symlink())
            result.emplace(fs::relative(entry.path(), root).generic_string(), Read(entry.path()));
    }
    return result;
}

fs::path CheckpointAccountFile(const Store& store, const std::string& fingerprint)
{
    std::vector<fs::path> matches;
    for (const auto& entry : fs::recursive_directory_iterator(store.CheckpointDirectory(fingerprint)))
    {
        if (entry.path().filename() == account)
            matches.push_back(entry.path());
    }
    Check(matches.size() == 1, "single published checkpoint generation has one exact account file");
    return matches.front();
}

struct Fixture
{
    fs::path root;
    Store store;
    std::string error;

    explicit Fixture(const std::string& name) : root(fixtureRoot / name), store(root, Validate)
    {
        CheckOk(store.Open("save_A", fingerprintA, error), error, "initialize " + name);
        Check(store.Timeline() != 0, "initialized timeline is nonzero");
    }

    void Live(const std::string& bytes, const std::string& key = "save_A")
    {
        Write(store.LiveDirectory(key) / account, bytes);
    }

    void Capture(const std::string& key = "save_A", const std::string& fingerprint = fingerprintA)
    {
        error.clear();
        CheckOk(store.CaptureCheckpoint(key, fingerprint, error), error, "capture native checkpoint");
    }
};

void TestLiveAndReconnect()
{
    Fixture f("live-reconnect");
    f.Live(snapshotX);
    f.Capture();
    const auto checkpoint = Files(f.store.CheckpointDirectory(fingerprintA));
    const uint64_t timeline = f.store.Timeline();
    f.Live(snapshotXY);
    Check(Files(f.store.CheckpointDirectory(fingerprintA)) == checkpoint,
        "runtime upload preserves every checkpoint byte, including receipts and layout");
    Store reopened(f.root, Validate);
    CheckOk(reopened.Open("save_A", fingerprintA, f.error), f.error, "ordinary same-save reopen");
    Check(reopened.Timeline() == timeline && reopened.RecoveryAllowed(),
        "ordinary reopen retains durable timeline and permits matching-lineage recovery");
    Check(Read(reopened.LiveDirectory("save_A") / account) == snapshotXY,
        "ordinary reconnect retains complete newest live inventory snapshot");
    CheckOk(reopened.RotateTimeline("save_A", fingerprintA, f.error), f.error, "explicit rollback");
    Check(reopened.Timeline() != 0 && reopened.Timeline() != timeline,
        "explicit same-slot rollback rotates timeline despite unchanged save key");
    Check(Read(reopened.LiveDirectory("save_A") / account) == snapshotX,
        "rollback rebases all live inventory, receipts and layout from checkpoint");
    Store afterRollback(f.root, Validate);
    CheckOk(afterRollback.Open("save_A", fingerprintA, f.error), f.error, "reopen after rollback");
    Check(afterRollback.Timeline() == reopened.Timeline() &&
        Read(afterRollback.LiveDirectory("save_A") / account) == snapshotX,
        "later reconnect cannot fall forward to abandoned runtime state");
    Check(Files(afterRollback.CheckpointDirectory(fingerprintA)) == checkpoint,
        "rollback never mutates immutable checkpoint generation");
}

void TestDescendantAndSameSlot()
{
    Fixture f("forward-and-overwrite");
    f.Live(snapshotX);
    f.Capture();
    const auto checkpointA = Files(f.store.CheckpointDirectory(fingerprintA));
    const uint64_t timeline = f.store.Timeline();
    f.Live(snapshotXY);
    f.Live(snapshotOther, "save_B");
    Write(f.store.LiveDirectory("save_B") / otherAccount, snapshotXY);
    CheckOk(f.store.AdvanceLiveSave("save_A", "save_B", f.error), f.error, "advance complete live state to descendant");
    Check(Read(f.store.LiveDirectory("save_B") / account) == snapshotXY &&
        !fs::exists(f.store.LiveDirectory("save_B") / otherAccount),
        "reused destination slot removes an offline account's future inventory absent from source");
    Check(f.store.Timeline() == timeline && f.store.SaveKey() == "save_A" &&
        f.store.Fingerprint() == fingerprintA, "live advance does not publish save identity before checkpoint capture");
    f.Capture("save_B", fingerprintB);
    Check(f.store.Timeline() == timeline && f.store.SaveKey() == "save_B" &&
        f.store.Fingerprint() == fingerprintB, "forward descendant advances identity without abandoning lineage");
    Check(Files(f.store.CheckpointDirectory(fingerprintA)) == checkpointA,
        "descendant save cannot overwrite parent checkpoint");
    CheckOk(f.store.RestoreCheckpoint("save_B", fingerprintB, f.error), f.error, "restore descendant checkpoint");
    Check(Read(f.store.LiveDirectory("save_B") / account) == snapshotXY,
        "descendant checkpoint retains complete newly acquired inventory and receipt pair");
    const uint64_t afterRestore = f.store.Timeline();
    Check(afterRestore != timeline, "explicit descendant restore rotates rather than reusing forward lineage");

    f.Live(snapshotXY);
    Check(Files(f.store.CheckpointDirectory(fingerprintA)) == checkpointA,
        "same-slot runtime mutations do not implicitly create a new checkpoint");
    f.Capture("save_A", fingerprintA2);
    Check(f.store.Timeline() == afterRestore && f.store.Fingerprint() == fingerprintA2,
        "successful same-slot save advances content fingerprint and preserves forward lineage");
    Check(Files(f.store.CheckpointDirectory(fingerprintA)) == checkpointA,
        "same slot with different fingerprint keeps old checkpoint generation immutable");
    CheckOk(f.store.RotateTimeline("save_A", fingerprintA, f.error), f.error, "restore older same-slot fingerprint");
    Check(Read(f.store.LiveDirectory("save_A") / account) == snapshotX,
        "old fingerprint restores X rather than newer same-slot X plus Y");
    CheckOk(f.store.RotateTimeline("save_A", fingerprintA2, f.error), f.error, "restore newer same-slot fingerprint");
    Check(Read(f.store.LiveDirectory("save_A") / account) == snapshotXY,
        "new fingerprint restores its own full snapshot");
}

void TestMissingAndCorruptCheckpoints()
{
    Fixture missing("missing-checkpoint");
    missing.Live(snapshotX);
    missing.Capture();
    missing.Live(snapshotXY, "save_B");
    const uint64_t before = missing.store.Timeline();
    Check(!missing.store.RotateTimeline("save_B", fingerprintB, missing.error),
        "missing checkpoint reports no exact restore rather than selecting future live state");
    Check(Files(missing.store.LiveDirectory("save_B")).empty(),
        "missing checkpoint empties target live slot despite a newer runtime snapshot");
    Check(missing.store.Timeline() != 0 && missing.store.Timeline() != before,
        "missing checkpoint still fences old journals with a new durable timeline");
    Store reopened(missing.root, Validate);
    CheckOk(reopened.Open("save_B", fingerprintB, missing.error), missing.error, "reopen empty missing-checkpoint lineage");
    Check(Files(reopened.LiveDirectory("save_B")).empty(), "reopen cannot resurrect missing-checkpoint future inventory");

    Fixture corrupt("corrupt-checkpoint");
    corrupt.Live(snapshotX);
    corrupt.Capture();
    Write(CheckpointAccountFile(corrupt.store, fingerprintA), "broken snapshot\n");
    corrupt.Live(snapshotXY);
    Check(!corrupt.store.RotateTimeline("save_A", fingerprintA, corrupt.error),
        "corrupt exact checkpoint fails closed");
    Check(Files(corrupt.store.LiveDirectory("save_A")).empty(),
        "corrupt checkpoint cannot restore the future live inventory as fallback");
}

void TestAtomicFailures()
{
    Fixture f("write-failures");
    f.Live(snapshotX);
    f.Capture();
    const fs::path manifest = f.store.CheckpointDirectory(fingerprintA) / "manifest.state";
    const std::string oldManifest = Read(manifest);
    const std::string oldMetadata = Read(f.root / "active_timeline.state");
    const uint64_t timeline = f.store.Timeline();
    f.Live(snapshotXY);
    fs::create_directory(fs::path(manifest.string() + ".tmp"));
    Check(!f.store.CaptureCheckpoint("save_A", fingerprintA, f.error),
        "real filesystem manifest staging failure rejects checkpoint publication");
    Check(Read(manifest) == oldManifest && Read(f.root / "active_timeline.state") == oldMetadata,
        "failed checkpoint write preserves prior manifest and timeline metadata bytes");
    Check(f.store.Timeline() == timeline && f.store.Fingerprint() == fingerprintA,
        "failed checkpoint publication does not advance in-memory identity");
    fs::remove(fs::path(manifest.string() + ".tmp"));
    CheckOk(f.store.RestoreCheckpoint("save_A", fingerprintA, f.error), f.error, "restore after failed checkpoint write");
    Check(Read(f.store.LiveDirectory("save_A") / account) == snapshotX,
        "failed write leaves original checkpoint fully readable");

    f.Live(snapshotXY);
    const std::string beforeRotation = Read(f.root / "active_timeline.state");
    fs::create_directory(f.root / "active_timeline.state.tmp");
    Check(!f.store.RotateTimeline("save_A", fingerprintA, f.error),
        "real filesystem metadata staging failure rejects timeline rotation");
    Check(Read(f.root / "active_timeline.state") == beforeRotation && Read(manifest) == oldManifest,
        "failed rotation preserves durable metadata and immutable checkpoint");
    fs::remove(f.root / "active_timeline.state.tmp");
}

void TestAdvanceFailures()
{
    Fixture invalid("advance-invalid-source");
    invalid.Live(snapshotX);
    invalid.Capture();
    invalid.Live(snapshotXY, "save_B");
    Write(invalid.store.LiveDirectory("save_A") / otherAccount, "corrupt source account\n");
    Check(!invalid.store.AdvanceLiveSave("save_A", "save_B", invalid.error),
        "invalid source account prevents forward live promotion");
    Check(invalid.store.Timeline() == 0 && !invalid.store.RecoveryAllowed(),
        "failed forward promotion invalidates timeline instead of admitting stale destination");
    Check(!invalid.store.CaptureCheckpoint("save_B", fingerprintB, invalid.error) &&
        !fs::exists(invalid.store.CheckpointDirectory(fingerprintB) / "manifest.state"),
        "failure fence prevents later capture of destination's abandoned future inventory");

    Fixture same("advance-same-slot");
    same.Live(snapshotXY);
    const uint64_t timeline = same.store.Timeline();
    CheckOk(same.store.AdvanceLiveSave("save_A", "save_A", same.error), same.error, "same-slot live advance noop");
    Check(same.store.Timeline() == timeline && Read(same.store.LiveDirectory("save_A") / account) == snapshotXY,
        "same-slot advance preserves all current live bytes and lineage");
    Check(!same.store.AdvanceLiveSave("not_current", "save_B", same.error) && same.store.Timeline() == 0,
        "forward promotion cannot use an unrelated historical source slot");
}

void TestValidationAndSymlinks()
{
    Fixture f("invalid-snapshot");
    f.Live(snapshotX);
    Write(f.store.LiveDirectory("save_A") / otherAccount, "corrupt account snapshot\n");
    Check(!f.store.CaptureCheckpoint("save_A", fingerprintA, f.error),
        "one invalid account prevents publishing a mixed checkpoint");
    Check(!fs::exists(f.store.CheckpointDirectory(fingerprintA) / "manifest.state"),
        "invalid mixed checkpoint has no published generation");
    fs::remove(f.store.LiveDirectory("save_A") / otherAccount);
    Write(f.store.LiveDirectory("save_A") / otherAccount, snapshotOther);
    f.Capture();
    f.Live(snapshotXY);
    Write(f.store.LiveDirectory("save_A") / otherAccount, snapshotXY);
    CheckOk(f.store.RestoreCheckpoint("save_A", fingerprintA, f.error), f.error, "restore multiple accounts");
    Check(Read(f.store.LiveDirectory("save_A") / account) == snapshotX &&
        Read(f.store.LiveDirectory("save_A") / otherAccount) == snapshotOther,
        "complete generation restores each account's distinct inventory and receipts");

    const fs::path outside = fixtureRoot / "outside.state";
    Write(outside, snapshotXY);
    Fixture sourceLink("live-symlink");
    fs::create_directories(sourceLink.store.LiveDirectory("save_A"));
    fs::create_symlink(outside, sourceLink.store.LiveDirectory("save_A") / account);
    Check(!sourceLink.store.CaptureCheckpoint("save_A", fingerprintA, sourceLink.error),
        "live account symlink is rejected rather than read through");
    Check(Read(outside) == snapshotXY, "source symlink rejection leaves external target untouched");

    Fixture checkpointLink("checkpoint-symlink");
    checkpointLink.Live(snapshotX);
    checkpointLink.Capture();
    const fs::path checkpointFile = CheckpointAccountFile(checkpointLink.store, fingerprintA);
    fs::remove(checkpointFile);
    fs::create_symlink(outside, checkpointFile);
    checkpointLink.Live(snapshotXY);
    Check(!checkpointLink.store.RotateTimeline("save_A", fingerprintA, checkpointLink.error),
        "checkpoint account symlink is rejected during restore");
    Check(Files(checkpointLink.store.LiveDirectory("save_A")).empty() && Read(outside) == snapshotXY,
        "rejected checkpoint symlink leaves no future live inventory or external mutation");

    Fixture child("live-child-directory");
    child.Live(snapshotX);
    fs::create_directory(child.store.LiveDirectory("save_A") / "unexpected");
    Check(!child.store.CaptureCheckpoint("save_A", fingerprintA, child.error),
        "unexpected nested live directory is rejected");

    Fixture aliases("duplicate-account-alias");
    aliases.Live(snapshotX);
    Write(aliases.store.LiveDirectory("save_A") / "player_account_29.state", snapshotOther);
    Check(!aliases.store.CaptureCheckpoint("save_A", fingerprintA, aliases.error),
        "alternate filename spellings of one logical account cannot publish two inventories");

    const fs::path externalRoot = fixtureRoot / "external-root";
    fs::create_directory(externalRoot);
    const fs::path linkedRoot = fixtureRoot / "linked-store-root";
    fs::create_directory_symlink(externalRoot, linkedRoot);
    Store rootLink(linkedRoot, Validate);
    Check(!rootLink.Open("save_A", fingerprintA, f.error), "store root symlink is rejected");
    Check(Files(externalRoot).empty(), "rejected root symlink never writes timeline metadata outside store");
}

void TestLegacyAndInputs()
{
    const fs::path legacyRoot = fixtureRoot / "legacy-migration";
    Store legacy(legacyRoot, Validate);
    Write(legacy.LiveDirectory("save_A") / account, snapshotX);
    std::string error;
    CheckOk(legacy.Open("save_A", fingerprintA, error), error, "initialize metadata around legacy live slot");
    Check(!legacy.RecoveryAllowed() && legacy.Timeline() != 0,
        "missing durable lineage does not implicitly authorize a legacy recovery journal");
    Check(Read(legacy.LiveDirectory("save_A") / account) == snapshotX,
        "first metadata migration preserves legacy live inventory instead of inventing checkpoint contents");

    const fs::path corruptRoot = fixtureRoot / "corrupt-timeline-metadata";
    Write(corruptRoot / "active_timeline.state", "not a valid timeline\n");
    Store corrupt(corruptRoot, Validate);
    Write(corrupt.LiveDirectory("save_A") / account, snapshotXY);
    Check(!corrupt.Open("save_A", fingerprintA, error),
        "corrupt metadata with no exact checkpoint reports a failed closed restore");
    Check(!corrupt.RecoveryAllowed() && corrupt.Timeline() != 0,
        "corrupt timeline metadata starts a fresh journal fence");
    Check(Files(corrupt.LiveDirectory("save_A")).empty(),
        "corrupt metadata cannot authorize preexisting future live snapshot");

    Store invalid(fixtureRoot / "invalid-inputs", Validate);
    Check(!invalid.Open("../save_A", fingerprintA, error), "save key traversal rejected");
    Check(!invalid.Open("save_A", "not-a-fingerprint", error), "nonhex fingerprint rejected");
    Check(!invalid.Open("save_A", "abcd", error), "undersized raw-hex fingerprint rejected");
    Check(!invalid.Open("save_A", "../1234_0xABCD", error), "fingerprint traversal rejected");
    Check(!fs::exists(fixtureRoot / "invalid-inputs" / "active_timeline.state"),
        "invalid identities cannot publish timeline metadata");
    Store rawHex(fixtureRoot / "raw-hex-fingerprint", Validate);
    CheckOk(rawHex.Open("save_A", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", error), error,
        "raw 128-bit hexadecimal fingerprint is accepted without a decimal digit");
}

void TestRecoveryMetadata()
{
    const fs::path journal = fixtureRoot / "metadata" / "journal.state";
    const std::string metadata =
        "recoveryAccount=0000000000000029\nrecoveryHost=0000000000000013\n"
        "recoveryTimeline=00000000000000a1\n"
        "recoverySaveKey=0x1234_campaign1_0xABCDEF_manual0\nrecoveryRevision=7\n";
    const auto payload = [&](const std::string& fields)
    {
        return "fixture=valid\n" + fields + "inventory=QBeam:1\nreceipt=paired\nend=valid\n";
    };
    const auto parse = [&](const std::string& fields, RecoveryJournalMetadata& parsed)
    {
        Write(journal, payload(fields));
        return ParseRecoveryJournalMetadata(journal.string(), parsed);
    };
    RecoveryJournalMetadata parsed;
    std::string original;
    Write(journal, payload(metadata));
    Check(ParseRecoveryJournalMetadata(journal.string(), parsed, &original) &&
        parsed.accountToken == 0x29 && parsed.hostAccountToken == 0x13 &&
        parsed.timelineId == 0xa1 && parsed.revision == 7 && original == payload(metadata),
        "actual journal parser retains full validated payload and exact identity fields");
    const std::string timelineField = "recoveryTimeline=00000000000000a1\n";
    std::string legacy = metadata;
    legacy.erase(legacy.find(timelineField), timelineField.size());
    Check(!parse(legacy, parsed), "legacy journal without timeline cannot auto-qualify for recovery");
    for (const std::string value : {"0", "nothex", "10000000000000000", "a1junk", ""})
    {
        std::string bad = metadata;
        bad.replace(bad.find(timelineField), timelineField.size(), "recoveryTimeline=" + value + "\n");
        Check(!parse(bad, parsed), "zero, malformed or oversized timeline metadata rejected");
    }
    for (const std::string field : {
        "recoveryAccount=29\n", "recoveryHost=13\n", "recoveryTimeline=a1\n",
        "recoverySaveKey=other\n", "recoveryRevision=8\n"})
        Check(!parse(metadata + field, parsed), "duplicate journal scope field rejected");
    for (const std::string value : {"0", "-1", "7tail", "18446744073709551616", ""})
    {
        std::string bad = metadata;
        const std::string revisionField = "recoveryRevision=7\n";
        bad.replace(bad.find(revisionField), revisionField.size(), "recoveryRevision=" + value + "\n");
        Check(!parse(bad, parsed), "invalid or overflowing decimal recovery revision rejected");
    }
    std::string windowsMetadata;
    for (const char ch : metadata)
    {
        if (ch == '\n') windowsMetadata += '\r';
        windowsMetadata += ch;
    }
    Check(parse(windowsMetadata, parsed) && parsed.timelineId == 0xa1,
        "Windows CRLF journal metadata parses without changing lineage");
    Write(journal, "corrupt payload\n" + metadata);
    Check(!ParseRecoveryJournalMetadata(journal.string(), parsed),
        "invalid-integrity dependency rejects journal before metadata selection");

    const std::string saveA = "0xAAAA_campaign1_0xABCDEF_manual0";
    const std::string saveB = "0xBBBB_campaign1_0xABCDEF_autosave1";
    Check(IsRecoverySaveKey(saveA) && IsRecoverySaveKey(saveB),
        "GUID-bearing native save labels remain valid components");
    Check(IsRecoverySaveKey("0xCFDE2E9B_campaign0_coopcheckpointlivea-v20-20261110") &&
        IsRecoverySaveKey("0xC016BF0B_campaign0_coopofflinedrop-20261110-204131165"),
        "actual custom native labels need no GUID when durable lineage matches");
    Check(!IsRecoverySaveKey("../manual0") && !IsRecoverySaveKey(".."),
        "recovery metadata cannot address a path outside its scoped directory");
    Check(!IsRecoverySaveKey("unknown_save") && IsRecoverySaveKey(std::string(63, 'x')) &&
        !IsRecoverySaveKey(std::string(64, 'x')),
        "unknown and unrepresentable wire keys cannot qualify recovery");
    Check(IsRecoverySaveKey("0xAAAA_manual0"),
        "safe legacy label syntax is independent of mandatory account Host and timeline checks");
    std::string expectedRebound = payload(metadata);
    const std::string oldKey = "0x1234_campaign1_0xABCDEF_manual0";
    expectedRebound.replace(expectedRebound.find(oldKey), oldKey.size(), saveB);
    Check(RebindRecoveryJournalSaveKey(payload(metadata), saveB) == expectedRebound,
        "forward journal rebind changes only exact save-key bytes, retaining inventory and receipts");
    Check(RebindRecoveryJournalSaveKey(payload(metadata) + "recoverySaveKey=extra\n", saveB).empty(),
        "journal rebind rejects ambiguous duplicate save-key fields");
    Check(RebindRecoveryJournalSaveKey(payload("recoveryTimeline=a1\n"), saveB).empty(),
        "journal rebind rejects missing save-key field");

    Write(journal, payload(metadata));
    Check(ReadPlayerSidecarSnapshotRevision(journal.string()) == 7,
        "validated scoped canonical snapshot retains its durable revision floor");
    Write(journal, payload("recoveryRevision=9\n"));
    Check(ReadPlayerSidecarSnapshotRevision(journal.string()) == 9 &&
        !ParseRecoveryJournalMetadata(journal.string(), parsed),
        "offline canonical revision floor survives without authorizing unscoped recovery");
    Write(journal, payload("recoveryRevision=18446744073709551615\n"));
    Check(ReadPlayerSidecarSnapshotRevision(journal.string()) == UINT64_MAX,
        "maximum valid revision floor is parsed without truncation");
    for (const std::string value : {"0", "-1", "9tail", "18446744073709551616", ""})
    {
        Write(journal, payload("recoveryRevision=" + value + "\n"));
        Check(ReadPlayerSidecarSnapshotRevision(journal.string()) == 0,
            "zero, malformed or overflowing canonical revision floor is rejected");
    }
    Write(journal, payload("recoveryRevision=9\nrecoveryRevision=10\n"));
    Check(ReadPlayerSidecarSnapshotRevision(journal.string()) == 0,
        "duplicate revision floors cannot silently select the later value");
    Write(journal, payload("recoveryRevision=9\r\n"));
    Check(ReadPlayerSidecarSnapshotRevision(journal.string()) == 9,
        "canonical revision floor supports native Windows CRLF metadata");
    Write(journal, payload("inventory=QBeam:1\n"));
    Check(ReadPlayerSidecarSnapshotRevision(journal.string()) == 0,
        "missing canonical revision floor remains absent");
    Write(journal, "corrupt payload\nrecoveryRevision=99\n");
    Check(ReadPlayerSidecarSnapshotRevision(journal.string()) == 0,
        "unvalidated canonical bytes cannot raise the persistent revision floor");
}

void TestTimelineFence()
{
    using CoopPlayerStateStore::MatchesTimeline;
    Check(MatchesTimeline(0xa1, 0xa1, 7, 7, false), "current lineage and epoch admit normal upload");
    Check(!MatchesTimeline(0xa2, 0xa1, 7, 7, false), "rollback lineage rejects an offline client's old journal");
    Check(!MatchesTimeline(0xa1, 0xa1, 8, 7, false), "late upload after host load cannot cross world epoch");
    Check(!MatchesTimeline(0xa1, 0xa1, 7, 7, true), "load guard rejects upload even with matching old identities");
    Check(!MatchesTimeline(0, 0, 7, 7, false) && !MatchesTimeline(0xa1, 0, 7, 7, false),
        "missing journal lineage is not considered an exact match");
    Check(!MatchesTimeline(0xa1, 0xa1, 0, 0, false) && !MatchesTimeline(0xa1, 0xa1, 7, 0, false),
        "missing world epoch is not considered an exact match");
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    fixtureRoot = argv[1];
    fs::create_directories(fixtureRoot);
    TestLiveAndReconnect();
    TestDescendantAndSameSlot();
    TestMissingAndCorruptCheckpoints();
    TestAtomicFailures();
    TestAdvanceFailures();
    TestValidationAndSymlinks();
    TestLegacyAndInputs();
    TestRecoveryMetadata();
    TestTimelineFence();
    std::cout << checks << " production checkpoint-store filesystem checks passed\n";
}
