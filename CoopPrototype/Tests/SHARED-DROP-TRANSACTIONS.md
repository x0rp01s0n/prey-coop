# Shared-Drop Transactions

Protocol 255 uses a prepare/ready/commit exchange for shared world drops. Personal
map loot stays on its vanilla pickup path.

1. The Host reserves the drop for one account. The Client prepares its baseline
   snapshot and replies ready, without adding the item to its inventory.
2. The Host persists the ownership assignment in that account's scoped player
   snapshot before publishing commit. The world copy is no longer eligible loot.
3. The winner delivers the committed item through native pickup and measures the
   actual inventory delta. Inventory and cumulative delivery receipt are captured
   together before reporting completion; partial acceptance retains the remainder.
4. Repeated packets or reconnect recovery use the receipt, not another full grant.
   A missing completion message cannot turn a committed item back into world loot.

Timeout or disconnect cancels only an uncommitted reservation. After commit,
failed or delayed delivery remains an account entitlement. Unknown native outcomes
or failed post-mutation persistence fail closed rather than retrying pickup blindly.

## Scope And Persistence

Packets are fenced by Host session, save, world epoch, area, transaction, and object
version. Persisted entries contain a stable drop identity and archetype/count, not
a native EntityId. They live in the exact Host/account/save snapshot; the Client
replaces its receipt list with the snapshot actually restored, rather than merging
unrelated timelines.

Explicit checkpoint replacement starts a new Host-session nonce, making old
pending grants inert. Within the current session, uploads cannot invent ownership,
regress an acquired receipt, or omit an acquired entry. Missing pending entries are
preserved by the Host. The receipt section belongs to the plain inventory in that
same hashed file and disables older native-capture inventory replacement.
The marker remains authoritative with zero entries. This uses the ordinary plain
snapshot restore path and overrides opt-in experimental native-preload captures;
compatibility with those experiments is not claimed.

This does **not** provide durable world-drop deduplication across Host crashes.
Host restart/world identity reconciliation is a separate unresolved boundary.

## Regression Suites

Run from the repository root with Python 3 and a C++17 compiler:

```sh
python3 CoopPrototype/Tests/run_shared_drop_transaction_tests.py
python3 CoopPrototype/Tests/run_shared_drop_delivery_tests.py
bash CoopPrototype/Tests/run_shared_drop_delivery_tests.sh
```

- Transaction checks cover contention, cancellation, delayed/repeated/reordered
  packets, lost commit/result, disconnect, scope rejection, and ownership storage
  failure. They compile selected unmodified production methods and record types.
- Native-delivery checks compile the production delivery helper against engine
  fakes: partial/full acceptance, overflow acceptance/cancellation, nested callbacks,
  unknown deltas, persistence failures, and receipt-based remainder delivery.
- Metadata checks compile the production parser and merge helpers: malformed or
  duplicate rows, integer bounds, receipt regression/omission, invented ownership,
  and preservation of pending assignments. The same command extracts the actual
  recovery selector and checks that precommit baselines retain Host assignments
  without separating acquired receipts from their inventory snapshot.

The C++ fixtures fake engine inventory, entity operations, persistence results, and
transport. They do not prove native inventory packing, disk atomicity under power
loss, engine hook behavior, or real reconnect restoration. The separate Windows
`Run-SharedDropEscrow.ps1` runner exercises a disposable native pair and records
phase boundaries, counts, probes, and snapshots; held-phase recovery requires an
actual Client restart while the Host remains unchanged.

Historical PR60 counterexamples and their original evidence are documented in
[PR60-VERIFICATION.md](PR60-VERIFICATION.md). Those baseline results are not claims
about the current implementation, and the old baseline-only runner option no
longer applies.
