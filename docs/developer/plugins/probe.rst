.. _probe_plugin:

probe: off-control-plane data-plane observation
===============================================

.. note::

   v1 (FIB-reachability observation) is implemented and lab-validated; the
   framework below reserves extension points for the remaining task types
   (still design-stage).

Introduction
------------

VPP has one **main (control) thread**. It serially services the binary API,
the debug CLI, control-plane feeding (e.g. route programming), linux-cp, and —
when no workers are configured — packet processing as well. Two failure modes
follow:

* **Blocking the control thread.** A control-plane operation that waits
  synchronously for an external event steals the thread for its whole
  duration. The classic offender is the ``ping`` debug CLI run over
  ``cli_inband``: it blocks the main thread for *seconds* waiting for echo
  replies, starving the API, route feeding and packet punt in the process — so
  a "liveness" probe built on it can *manufacture* the very forwarding stall it
  was meant to detect.

* **Loading the control thread.** Even non-blocking work — enumerating tables
  via ``*_dump``, polling counters — contends with the main thread. At scale
  (millions of entries / high route churn) this is a real throughput cost.

The **probe** plugin is an in-process, *off-control-plane* host for such
observation and probing work. It runs on its own execution context (a
cooperative main-thread process for light tasks, or worker threads for
heavy / parallel ones), reads — and optionally actively probes — data-plane
state directly, and publishes results **out of band** via the stats segment.
External agents then observe data-plane health by reading the stats segment,
**without ever traversing, contending with, or blocking VPP's control plane**.

It is deliberately application-agnostic: the plugin knows about FIB entries,
adjacencies, counters and probe packets — not about any consumer's semantics.
Consumers register the tasks they care about and read the published results.

Design invariant
----------------

*No task hosted by* ``probe`` *may block or heavily burden the control
(main) thread.* This is enforced by construction through the execution-model
layer, and bounded by one hard VPP rule:

* **Reads are lock-free and parallelizable.** VPP's forwarding structures
  (FIB mtrie/bihash, adjacencies) are read concurrently by workers on the
  data path; a probe task can perform the same reads from a worker safely.

* **Writes are barrier-serialized and *cannot* be parallelized.** Mutating
  shared state requires the global worker barrier (all workers paused, one
  writer at a time). A write task can *decouple* injection from the binary-API
  dispatch path and *batch* many mutations under fewer barriers, but it cannot
  make the mutation itself run in parallel. This boundary is intrinsic to VPP
  and is called out so future work does not assume otherwise.

Architecture
------------

Four layers, so new bottlenecks slot in as new tasks without touching the
framework:

1. **Task registry.** A pluggable set of task types. Each task declares its
   *class* (observation / active-probe / mutation), its *execution model*
   (process / worker) and its *I/O channels*. Tasks are registered and removed
   via the binary API / CLI — a configuration path, exercised at setup, not on
   the hot path.

2. **Execution models.**

   * *Main-thread process* — a ``vlib_process`` node scheduled cooperatively on
     the main thread. Suits **light** tasks (a handful of FIB lookups every few
     seconds cost microseconds and yield between wakes; this does not "block"
     the thread). Simplest; no worker configuration required.
   * *Worker* — a registered VPP worker thread, for **heavy / parallel** tasks
     (full-table inventory, large probe fleets) that must run off the main
     thread on a dedicated core.

3. **I/O channels (all bypass the binary-API / CLI control path).**

   * *Stats segment* — the primary output. Results are published as gauges /
     counters that any stats client (the VPP stats socket, Prometheus
     exporters, a co-located agent) reads directly from shared memory.
   * *Own socket* — optional, for tasks needing bulk input/output beyond the
     stats schema (e.g. a future bulk mutation-injection channel).

4. **Data-plane access.** Shared helpers for tasks: lock-free reads (FIB
   lookup, adjacency/DPO inspection, counter snapshot) and — for mutation
   tasks — the batched-under-barrier write convention. Encapsulates the
   read-safety and barrier rules so every task reuses them correctly.

Task classes
------------

.. list-table::
   :header-rows: 1
   :widths: 18 34 30 22 10

   * - Class
     - Examples
     - Offloadable?
     - Execution
     - Stage
   * - **Observation** (read-only)
     - FIB reachability, adjacency state, counters, table inventory / drift
     - Fully (lock-free reads, parallelizable)
     - process (light) / worker (heavy)
     - v1 / v2
   * - **Active probe**
     - inject a probe packet, verify it is forwarded (exercises the real FIB,
       not just its contents)
     - Fully (runs on a worker in the data path, off the control thread)
     - worker
     - v2
   * - **Bulk mutation**
     - batched FIB / table programming (decouple from per-message API dispatch)
     - Partially — write is barrier-serialized (not parallelizable); prep +
       batching only
     - worker prep + barrier-batched apply
     - v3+ (exploratory)

v1: FIB reachability
--------------------

The first task type answers, off the control plane: *"is prefix P in FIB table
T currently resolvable to a real forwarding adjacency, or is it black-holed /
unresolved?"* — the read-only equivalent of an active reachability check,
without sending a packet or dumping the table through the API.

Mechanism: the task's process node periodically resolves each registered
target via ``ip[46]_fib_forwarding_lookup`` → load-balance → **leaf DPO**, and
classifies the result. A recursive or ECMP route nests a per-path load-balance
in bucket 0 (the top forwarding load-balance points at per-path load-balances,
each pointing at the real adjacency), so the task follows bucket 0 **down
through the load-balance levels** to the leaf before classifying:

* **reachable** — a real interface adjacency (``dpo-adjacency`` /
  ``dpo-adjacency-midchain`` with a resolved next-hop / rewrite);
* **black-holed** — a ``dpo-drop`` (e.g. a ``via drop`` route, or a
  fail-open drop next-hop);
* **unresolved** — ``dpo-adjacency-incomplete`` / glean (next-hop not yet
  resolved).

Each verdict is published as a stats gauge (see below). Because a single mtrie
/ bihash lookup is microseconds, a handful of targets on the main-thread
process node is negligible; large target sets move to a worker.

Configuration
-------------

Registration is a control-plane setup action (infrequent), so it may use the
binary API / CLI:

::

  probe fib [add|del] <prefix> table <table-id> name <name>
  show probe fib

The ``<prefix>`` must carry a length (``/32``, ``/128``); ``name`` is the
unique stats-segment key for the target. Binary API: ``probe_fib_add_del`` /
``probe_fib_dump``.

Classify session point-lookup
-----------------------------

``probe_classify_lookup`` resolves classify sessions by match key **without**
dumping the whole table. VPP core exposes only ``classify_session_dump`` (whole
table, ``table_id`` only — no match filter), so a control-plane client that
wants the hit-next index of a *specific* session must otherwise stream the
entire table across the main thread. For an edge-wide shared mask table that can
be hundreds of thousands of sessions, paid on **every** membership change — the
control plane only ever wanted a handful of keys.

This message does the same O(1) hash lookup the data path does per packet
(``vnet_classify_hash_packet`` + ``vnet_classify_find_entry``, with ``now = 0``
so it never mutates the entry's ``hits`` / ``last_heard``), in a batch:

* Request: ``table_id``, ``key_len`` (bytes per key =
  ``(skip_n_vectors + match_n_vectors) * 16`` for that table — the same full
  match buffer ``classify_add_del_session`` takes), and ``match`` = the keys
  concatenated (count = ``match_len / key_len``). All keys in one request share
  one table (hence one ``key_len``); a caller with multiple masks issues one
  request per table.
* Reply: ``hits[count]``, positionally aligned to the request keys — each is the
  session's ``next_index`` (the policer index in policer-classify mode), or
  ``~0`` (``0xffffffff``) if no session matches.

It runs on the main thread like every binary-API handler, so it is serialized
with ``classify_add_del_session`` writes and can never observe a torn write.
Cost is O(1) per key versus O(table) per dump: the win is algorithmic (read the
K keys you care about, not the whole table), not a thread change — a point
lookup is a hash probe regardless of which thread runs it.

Output (stats segment)
----------------------

Results appear under a ``/probe`` stats tree, read via the stats segment
(``/run/vpp/stats.sock``) by any client — no binary-API round-trip, no main
thread involvement:

::

  /probe/fib/<name>/reachable      1 = reachable, 0 = not
  /probe/fib/<name>/dpo_type       leaf DPO class (adjacency / drop / incomplete)
  /probe/fib/<name>/changes        count of reachable<->unreachable transitions

These are published as ``STAT_DIR_TYPE_SCALAR_INDEX`` entries (via
``vlib_stats_add_scalar`` + ``vlib_stats_set_gauge``), **not** the newer
``STAT_DIR_TYPE_GAUGE``: the value is identical, but SCALAR_INDEX is decoded by
every stats client, including older ones (e.g. govpp v0.13.0) that do not yet
map the GAUGE dir-type and would otherwise read a nil value.

Threading and read-safety
-------------------------

* **Main-thread process (default).** The process node runs on the main thread,
  so its reads are serialized with the main thread's own FIB writes — safe by
  construction, no barrier needed. It performs only microsecond lookups and
  sleeps between intervals, so it does not burden the thread.
* **Worker (heavy tasks).** A worker performs lock-free reads exactly as the
  data path does; it must use the read-safe lookup helpers and must not retain
  references across a barrier epoch. It never writes shared state (observation
  tasks) — or, for mutation tasks, follows the batched-under-barrier
  convention.

The plugin runs in-process, so a fault in a task can affect VPP; tasks are
therefore held to strict read-only, defensive discipline (bounds-checked,
no shared-state writes outside the mutation convention).

Roadmap
-------

* **v1** — FIB reachability observation (main-thread process → stats gauges).
* **v2** — table inventory / drift observation and active packet probes, on
  workers (offload heavy enumeration and real-forwarding checks off the main
  thread).
* **v3+** — exploratory bulk-mutation offload (decouple bulk programming from
  the per-message API dispatch; batch under fewer barriers). Bounded by the
  barrier rule: this relieves the control thread of API-dispatch overhead and
  improves batching, but does not parallelize the mutation itself.

Relation to existing facilities
-------------------------------

* **stats segment** — ``probe`` *produces* stats (reachability, inventory) that
  do not otherwise exist as counters; it composes with all existing stats
  tooling on the read side.
* **flowprobe / sflow** — those export *observed traffic* (passive); ``probe``
  reports *state / active reachability* and does not depend on live traffic
  being present.
* **ping** — the debug ``ping`` tests forwarding but runs on, and blocks, the
  main thread; ``probe`` provides the same intent (v1 read-only, v2 active) off
  the control plane.
