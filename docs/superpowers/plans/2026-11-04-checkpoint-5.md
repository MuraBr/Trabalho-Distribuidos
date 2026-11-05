# Checkpoint 5 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement C5 for both students: LFU caching on Peer chunk reads, durable 2PC for Super Peer ANNOUNCE, and batched IST for missing committed log versions.

**Architecture:** Add a bounded compressed-chunk cache to the existing `Storage` and surface its statistics through a copy-out API. Extend the existing C4 log with a sidecar transaction journal and connect its prepare/decision states to C4 messages; add bounded IST batches to the same cluster protocol while retaining C4 entry encoding.

**Tech Stack:** C2x, POSIX threads/files/TCP, existing CRC32 framing, existing LZ4 chunks, Python 3 integration runner, Make.

**Spec:** `docs/superpowers/specs/2026-11-04-checkpoint-5-design.md`

## Global Constraints

- Build with `-std=c2x -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Werror`.
- Preserve the existing 98-byte TCP header, transport CRC32, C2 payloads, and on-disk `C4LG` record format.
- Use `M_PREPARE=10`, `M_COMMIT=11`, `M_ABORT=12`; allocate IST message values 25 and 26 and update protocol type validation.
- Keep function signatures, calls, and multi-condition expressions on one line in C files.
- Keep explanatory comments in Portuguese in Aluno 2 C sources/tests; do not edit `requisitos_aluno_1.md`.
- Report local API tests separately from TCP integration and retain limitations of classic 2PC under coordinator loss/partitions.

## Review Focus

- Cache eviction must not delete a durable chunk; pin this in `tests/c5/test_cache_lfu.c` by reading after eviction.
- Concurrent reads and cache reconfiguration must not expose freed cache data; cover with concurrent cache reads and lock-safe copy-out in the cache test.
- A repeated TransactionID with changed bytes must be rejected; cover in the transaction journal test.
- A failed participant vote must not publish metadata or committed log entry; cover via `ABORT` state and absence from `c4_log_get_transaction`.
- IST must reject gaps, corrupt records, divergent versions, and malformed/oversized batches; cover valid paging plus malformed inputs in C5 protocol integration.

---

### Task 1: Bounded LFU cache in Peer storage

**Files:**
- Modify: `storage.h`, `storage.c`, `peer.c`
- Create: `tests/c5/test_cache_lfu.c`
- Modify: `Makefile`

**Interfaces:**
- Produce `StorageCacheStats` with `size_t capacity_bytes`, `used_bytes`, `entries` and `uint64_t hits`, `misses`, `evictions`.
- Produce `int storage_set_cache_capacity(Storage *storage, size_t capacity_bytes)` and `int storage_get_cache_stats(Storage *storage, StorageCacheStats *output)`.
- `storage_create` defaults to 16 MiB; `peer.c` reads `PEER_CACHE_BYTES` before serving, with zero disabling cache.
- Cache key is `(ObjectID, chunk index)`; cache value is an owned copy of the compressed chunk and descriptor. Lowest frequency is evicted first; least-recent access breaks ties; frequency saturates at `UINT64_MAX`.

- [ ] **Step 1: Write failing LFU tests** for first-read miss, repeated-read hit/frequency, deterministic LFU/tie eviction, zero capacity, and successful reread from durable storage after eviction.
- [ ] **Step 2: Run `make bin/test_cache_lfu`** and confirm the expected compile/API failure before implementation.
- [ ] **Step 3: Implement cache APIs and `storage_read_chunk` lookup/fill/eviction** with the existing Storage mutex protecting entries and counters; return caller-owned chunk bytes on both disk and cache hits.
- [ ] **Step 4: Run `bin/test_cache_lfu`** and confirm every statistic and durable reread assertion passes.
- [ ] **Step 5: Configure capacity from `PEER_CACHE_BYTES` in `peer.c` and log concise hit/miss/eviction events on actual `M_DOWNLOAD_REQ` reads.**
- [ ] **Step 6: Run `make test-c2`** to verify the storage and transfer contract is unchanged.

### Task 2: Durable transaction journal in the C4 log module

**Files:**
- Modify: `c4_log.h`, `c4_log.c`, `Makefile`
- Create: `tests/c5/test_transaction_journal.c`

**Interfaces:**
- Produce `C4TransactionState` values `C4_TXN_UNKNOWN`, `C4_TXN_PREPARED`, `C4_TXN_COMMITTED`, and `C4_TXN_ABORTED`.
- Produce `int c4_log_prepare(C4Log *log, const C4LogEntry *entry)`, `int c4_log_commit(C4Log *log, const uint8_t transaction_id[16], C4LogEntry *output)`, `int c4_log_abort(C4Log *log, const uint8_t transaction_id[16])`, `int c4_log_transaction_state(C4Log *log, const uint8_t transaction_id[16], C4TransactionState *output)`, and `int c4_log_get_prepared(C4Log *log, const uint8_t transaction_id[16], C4LogEntry *output)`.
- Store prepared bytes and decisions in a checksummed, fsynced sidecar journal; do not change `C4LG` bytes. Reopen must finish a durable COMMIT missing from the operation log and retain ABORT tombstones for idempotency.

- [ ] **Step 1: Write failing journal tests** for prepare visibility, duplicate prepare, conflicting TransactionID, commit, abort, no visibility after abort, commit replay after reopen, and corrupt journal rejection.
- [ ] **Step 2: Run `make bin/test_transaction_journal`** and confirm the missing APIs make it fail.
- [ ] **Step 3: Implement the sidecar journal and the five APIs** with write-ahead decisions, CRC validation, fsync, monotonic index/version checks, and idempotent repeated decisions.
- [ ] **Step 4: Run `bin/test_transaction_journal`** and verify all lifecycle/reopen assertions.
- [ ] **Step 5: Re-run the recovered C4 log test when available; keep old `C4LG` fixtures readable.**

### Task 3: 2PC messages and ANNOUNCE transaction flow

**Files:**
- Modify: `protocol.h`, `protocol.c`, `c4_cluster.c`, `c4_cluster.h`, `superpeer.c`, `Makefile`
- Create: `tests/c5/transaction_integration.py`

**Interfaces and protocol:**
- `M_PREPARE` carries one encoded `C4LogEntry`; its TransactionID must match the entry. `M_COMMIT` and `M_ABORT` carry no payload and use the header TransactionID.
- Produce `c4_cluster_handle` branches that accept phases only from the registered active leader, make repeated phases idempotent, and never apply prepared data.
- `c4_cluster_submit` prepares locally and on every member captured as active; any negative vote/timeout persists ABORT and sends it to every participant sent PREPARE, including peers whose vote timed out. Only unanimous prepare persists COMMIT, appends/applies the committed entry, then sends COMMIT to participants. Return ACK to the originating Peer only after the captured participants acknowledge COMMIT.
- On restart, committed journal entries replay through the existing `apply_c4_entry`; prepared/aborted records remain invisible.

- [ ] **Step 1: Write a TCP integration assertion** that an uploaded ANNOUNCE emits PREPARE then COMMIT and produces the same committed C4 record/metadata on every configured active Super Peer.
- [ ] **Step 2: Run the integration test** and confirm it fails because the current server rejects `M_PREPARE`.
- [ ] **Step 3: Implement phase validation/handlers and replace direct `M_STATE_TRANSFER` submission in `c4_cluster_submit` with the 2PC flow.**
- [ ] **Step 4: Run the integration test** and verify the committed record, metadata visibility, transaction idempotency, and that no prepared state is visible before commit.
- [ ] **Step 5: Add an abort-path case** that obtains a negative vote, then verify durable ABORT and absence from committed log/catalog on all nodes.
- [ ] **Step 6: Build `make all` with strict CFLAGS** and run C2/C4 regression targets available in this checkout.

### Task 4: Versioned, bounded Incremental State Transfer

**Files:**
- Modify: `protocol.h`, `protocol.c`, `c4_cluster.c`, `c4_cluster.h`
- Modify: `tests/c5/transaction_integration.py`, `Makefile`

**Interfaces and protocol:**
- Add `M_IST_REQUEST=25` and `M_IST_BATCH=26`.
- `M_IST_REQUEST` payload is 16 bytes: last applied index u64 and version u64. `M_IST_BATCH` payload is u32 entry count followed by repeated u32 encoded-entry-size and encoded `C4LG` bytes.
- Each response contains the largest contiguous batch fitting `MAX_PAYLOAD_SIZE`; an empty batch means current. The caller requests again after its last accepted entry.
- The receiver validates count, lengths, checksum, exact next index/version and transaction identity before durable append/apply. A gap or divergence returns `M_ERROR` without advancing the cursor.
- On coordinator announcement, followers request missing entries from the coordinator; a new coordinator requests missing entries from active replicas before serving ANNOUNCE.

- [ ] **Step 1: Add failing TCP integration cases** for a late Super Peer catching up in multiple requests, already-current response, gap rejection, and corrupted entry rejection.
- [ ] **Step 2: Run the integration cases** and confirm IST message values are rejected.
- [ ] **Step 3: Implement batch encoding/decoding and request serving** in the existing C4 cluster path; keep C4 `M_SNAPSHOT` behavior available for compatibility.
- [ ] **Step 4: Trigger IST at coordinator/follower recovery seams** and ensure synchronization errors remain visible and do not advertise synchronized state.
- [ ] **Step 5: Run the IST integration cases** and compare byte-identical committed logs and metadata after recovery.

### Task 5: Real C5 runner and end-to-end scenarios

**Files:**
- Modify: `tests/c5/run.sh`, `Makefile`
- Create: `tests/c5/integration.py`
- Reuse: `tests/config/c5.conf`, `tests/data/documento_a.pdf`

- [ ] **Step 1: Replace the placeholder runner** with fail-fast shell orchestration that builds strict binaries, runs local LFU/journal tests, then invokes the TCP integration against the supplied C5 roster and temporary per-process data directories.
- [ ] **Step 2: Exercise real Peer upload and repeated download** using `--local-peer-port`; compare downloaded bytes and assert miss, hit, frequency increment and eviction evidence.
- [ ] **Step 3: Exercise 2PC commit and abort**; assert protocol order and exact committed/aborted catalog/log states instead of accepting regular-expression matches alone.
- [ ] **Step 4: Start a Super Peer after committed versions exist** and assert it catches up via multiple IST batches with matching log bytes and version.
- [ ] **Step 5: Run `bash tests/c5/run.sh` with `tests/config/c5.conf`** and require a nonzero result for any missing event, client failure, byte mismatch, or process crash; retain logs/evidence under a unique `/tmp` directory.
- [ ] **Step 6: Run `make test-c2`, `test-c3-local`, `test-c3-membership`, available C3 network regressions, and the C5 TCP suite** to check interactions with existing checkpoints. The published checkout has no `tests/c4/test_log.c` or `tests/c4/integration.py`, so `make test`/`make test-c4` are not usable unless replacement tests are added.

### Task 6: Update status and explanations for both students

**Files:**
- Modify: `requisitos_aluno_2.md`, `explicacao_codigo_aluno_2.md`, `documentacao/guia_aluno_1.md`
- Create: `documentacao/checkpoint5_implementacao.md`

- [ ] **Step 1: Document Aluno 1's cache path and Aluno 2's transaction/IST wire and disk formats**, roles, CLI/configuration, test commands, observed evidence, and known limits.
- [ ] **Step 2: Record only actually executed checks** in `requisitos_aluno_2.md`; leave `requisitos_aluno_1.md` untouched.
- [ ] **Step 3: Review `git diff --check`, strict build output, C5 runner output and scoped regression output** before reporting completion.

---

## Execution Notes

Implement in this session (native execution); do not delegate. Do not commit or push unless separately requested. The published baseline has no `tests/c4/integration.py` or `tests/c4/test_log.c`, and `tests/c5/c5.conf` is locally deleted; preserve that state. The required C5 configuration is `tests/config/c5.conf`.
