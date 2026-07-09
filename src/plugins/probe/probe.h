/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 probe plugin contributors.
 *
 * probe: an in-process, off-control-plane host for read-only data-plane
 * observation. v1 = FIB reachability: a main-thread vlib_process periodically
 * resolves registered targets in the FIB and publishes a per-target verdict to
 * the stats segment, so agents observe forwarding health without traversing,
 * contending with, or blocking VPP's single control (main) thread.
 * See docs/developer/plugins/probe.rst.
 */

#ifndef __included_probe_h__
#define __included_probe_h__

#include <vlib/vlib.h>
#include <vnet/fib/fib_types.h>

/* Reachability verdict, published as /probe/fib/<name>/reachable. */
#define PROBE_UNREACHABLE 0 /* dpo-drop, incomplete/glean, or no FIB table */
#define PROBE_REACHABLE	  1 /* resolves to a complete forwarding adjacency */

/* One registered FIB-reachability target. */
typedef struct probe_fib_target_t_
{
  fib_prefix_t prefix; /* family + address (+ len, for display) */
  u32 table_id;	       /* VRF / FIB table id */
  u8 *name;	       /* stats-segment name (unique) */

  /* stats-segment gauge indices (created on add, removed on del) */
  u32 stat_reachable;
  u32 stat_dpo_type;
  u32 stat_changes;

  /* last published verdict, for change detection */
  u8 last_reachable;
  u8 have_result;
  u64 changes; /* number of reachable<->unreachable transitions */
} probe_fib_target_t;

typedef struct probe_main_t_
{
  u16 msg_id_base; /* binary API */

  probe_fib_target_t *targets;	/* pool */
  uword *target_by_name;	/* hash: name -> pool index */

  f64 scan_interval;	   /* seconds between scans */
  u32 process_node_index;  /* the vlib_process node */

  /* Liveness heartbeat: a monotonically increasing counter bumped once per
   * process scan, published as /probe/heartbeat. Because the process runs on a
   * timer (vlib_process_wait_for_event_or_clock), it advances even when VPP is
   * fully idle — unlike the worker vector/loop counters that freeze under
   * adaptive sleep (the §4.1 "can't tell idle from wedged" blind spot). An
   * agent reading the stats segment infers a STALLED main thread (wedge) when
   * this stops advancing while the segment is still mapped; a dead process
   * instead removes the stats socket, which the agent sees as
   * ErrStatsDisconnected. Together they cover both death modes off the main
   * thread, without a single binary-API call. */
  u32 stat_heartbeat;
  u64 beats;

  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;
} probe_main_t;

extern probe_main_t probe_main;

/* Default scan cadence (seconds). Lookups are microseconds; this bounds how
 * quickly a FIB change surfaces as a stat AND the heartbeat cadence (so the
 * agent's wedge-detection floor is a small multiple of this). 1s keeps both
 * responsive at negligible cost. */
#define PROBE_DEFAULT_SCAN_INTERVAL 1.0

/* add/del a target (shared by CLI + binary API). Returns 0 on success. */
int probe_fib_add_del (const fib_prefix_t *prefix, u32 table_id,
		       const u8 *name, u8 is_add);

/* wake the process node to scan now (e.g. right after a config change). */
void probe_scan_kick (void);

/* binary API plumbing (probe_api.c) */
clib_error_t *probe_api_hookup (vlib_main_t *vm);

#endif /* __included_probe_h__ */
