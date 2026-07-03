/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 probe plugin contributors.
 *
 * probe: off-control-plane data-plane observation (v1 = FIB reachability).
 *
 * A main-thread vlib_process periodically resolves each registered target in
 * the FIB and publishes the verdict to the stats segment. It reads the same
 * forwarding structures the data path reads; running on the main thread means
 * the reads are serialized with the main thread's own FIB writes (safe, no
 * barrier). Each lookup is microseconds and the process sleeps between scans,
 * so it does not burden the control thread — unlike a blocking `ping` CLI.
 * See docs/developer/plugins/probe.rst.
 */

#include <probe/probe.h>

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <vpp/app/version.h>

#include <vlib/stats/stats.h>

#include <vnet/fib/fib_table.h>
#include <vnet/fib/ip4_fib.h>
#include <vnet/fib/ip6_fib.h>
#include <vnet/dpo/load_balance.h>
#include <vnet/dpo/drop_dpo.h>
#include <vnet/dpo/dpo.h>
#include <vnet/ip/ip_types.h>
#include <vnet/ip/format.h>

probe_main_t probe_main;

#define PROBE_EVENT_SCAN 1

/* Descend the forwarding chain from an initial DPO to a leaf. A recursive or
 * ECMP route nests a load-balance in bucket 0 (the top forwarding load-balance
 * points at per-path load-balances, each of which points at the real
 * adjacency), so the first bucket is often itself a DPO_LOAD_BALANCE, not the
 * leaf. Follow bucket 0 down through load-balance levels until a non-LB DPO is
 * reached; cap the depth to avoid a pathological loop. */
static_always_inline const dpo_id_t *
probe_dpo_leaf (const dpo_id_t *dpo)
{
  for (int depth = 0; depth < 8; depth++)
    {
      if (dpo->dpoi_type != DPO_LOAD_BALANCE)
	return dpo;
      const load_balance_t *lb = load_balance_get (dpo->dpoi_index);
      if (lb->lb_n_buckets == 0)
	return dpo; /* empty load-balance: no path, leave as-is (unreachable) */
      dpo = load_balance_get_bucket_i (lb, 0);
    }
  return dpo; /* depth exceeded: treat as unreachable */
}

/* Map a resolved leaf forwarding DPO to a reachability verdict. A complete
 * adjacency (direct or midchain) can forward to a next-hop; drop / incomplete
 * / glean / receive cannot (black-hole or unresolved). Call probe_dpo_leaf()
 * first so recursive/ECMP routes are followed to their leaf adjacency. */
static_always_inline u8
probe_dpo_reachable (const dpo_id_t *dpo)
{
  switch (dpo->dpoi_type)
    {
    case DPO_ADJACENCY:
    case DPO_ADJACENCY_MIDCHAIN:
      return PROBE_REACHABLE;
    default:
      return PROBE_UNREACHABLE;
    }
}

/* Resolve one target in its FIB and publish the verdict. Runs on the main
 * thread (from the process node), so FIB reads are safe without a barrier. */
static void
probe_scan_one (probe_fib_target_t *t)
{
  u8 reachable = PROBE_UNREACHABLE;
  u32 dpo_type = DPO_DROP; /* default when the target's FIB table is absent */
  u32 fib_index = fib_table_find (t->prefix.fp_proto, t->table_id);

  if (fib_index != (u32) ~0)
    {
      index_t lbi;
      const load_balance_t *lb;
      const dpo_id_t *dpo;

      if (FIB_PROTOCOL_IP4 == t->prefix.fp_proto)
	lbi = ip4_fib_forwarding_lookup (fib_index, &t->prefix.fp_addr.ip4);
      else
	lbi = ip6_fib_table_fwding_lookup (fib_index, &t->prefix.fp_addr.ip6);

      lb = load_balance_get (lbi);
      /* bucket 0 is representative; descend nested load-balances (recursive /
       * ECMP routes) to the leaf, then classify. A live path resolves to an
       * adjacency, a pure miss/drop route to a drop DPO. */
      dpo = load_balance_get_bucket_i (lb, 0);
      dpo = probe_dpo_leaf (dpo);
      dpo_type = dpo->dpoi_type;
      reachable = probe_dpo_reachable (dpo);
    }

  vlib_stats_set_gauge (t->stat_reachable, reachable);
  vlib_stats_set_gauge (t->stat_dpo_type, dpo_type);

  if (!t->have_result || reachable != t->last_reachable)
    {
      if (t->have_result)
	{
	  t->changes++;
	  vlib_stats_set_gauge (t->stat_changes, t->changes);
	}
      t->last_reachable = reachable;
      t->have_result = 1;
    }
}

static uword
probe_process (vlib_main_t *vm, vlib_node_runtime_t *rt, vlib_frame_t *f)
{
  probe_main_t *pm = &probe_main;
  uword *event_data = 0;

  while (1)
    {
      vlib_process_wait_for_event_or_clock (vm, pm->scan_interval);
      (void) vlib_process_get_events (vm, &event_data);
      vec_reset_length (event_data);

      probe_fib_target_t *t;
      pool_foreach (t, pm->targets)
	{
	  probe_scan_one (t);
	}
    }
  return 0;
}

VLIB_REGISTER_NODE (probe_process_node) = {
  .function = probe_process,
  .type = VLIB_NODE_TYPE_PROCESS,
  .name = "probe-process",
};

void
probe_scan_kick (void)
{
  probe_main_t *pm = &probe_main;
  if (pm->process_node_index != (u32) ~0)
    vlib_process_signal_event (pm->vlib_main, pm->process_node_index,
			       PROBE_EVENT_SCAN, 0);
}

int
probe_fib_add_del (const fib_prefix_t *prefix, u32 table_id, const u8 *name,
		   u8 is_add)
{
  probe_main_t *pm = &probe_main;
  u8 *key;
  uword *p;

  if (name == 0 || vec_len (name) == 0)
    return VNET_API_ERROR_INVALID_VALUE;

  /* null-terminated copy of the name (used as hash key + in stat paths) */
  key = 0;
  vec_add (key, name, vec_len (name));
  vec_add1 (key, 0);

  p = hash_get_mem (pm->target_by_name, key);

  if (is_add)
    {
      probe_fib_target_t *t;
      if (p != 0)
	{
	  vec_free (key);
	  return VNET_API_ERROR_VALUE_EXIST;
	}
      pool_get_zero (pm->targets, t);
      t->prefix = *prefix;
      t->table_id = table_id;
      t->name = key; /* owns the null-terminated key */

      /* Publish as SCALAR_INDEX (not the newer GAUGE dir-type): the value is
       * identical, but SCALAR_INDEX is understood by every stats client,
       * including older ones (e.g. govpp v0.13.0) that do not yet decode
       * STAT_DIR_TYPE_GAUGE and would otherwise read a nil value. */
      t->stat_reachable =
	vlib_stats_add_scalar ("/probe/fib/%s/reachable", t->name);
      t->stat_dpo_type =
	vlib_stats_add_scalar ("/probe/fib/%s/dpo_type", t->name);
      t->stat_changes =
	vlib_stats_add_scalar ("/probe/fib/%s/changes", t->name);

      hash_set_mem (pm->target_by_name, t->name, t - pm->targets);
      probe_scan_kick ();
      return 0;
    }
  else
    {
      probe_fib_target_t *t;
      if (p == 0)
	{
	  vec_free (key);
	  return VNET_API_ERROR_NO_SUCH_ENTRY;
	}
      t = pool_elt_at_index (pm->targets, p[0]);
      hash_unset_mem (pm->target_by_name, t->name);
      vlib_stats_remove_entry (t->stat_reachable);
      vlib_stats_remove_entry (t->stat_dpo_type);
      vlib_stats_remove_entry (t->stat_changes);
      vec_free (t->name);
      pool_put (pm->targets, t);
      vec_free (key);
      return 0;
    }
}

/* ---- CLI ---------------------------------------------------------------- */

static clib_error_t *
probe_fib_add_del_cmd (vlib_main_t *vm, unformat_input_t *input,
		       vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  clib_error_t *err = 0;
  ip_prefix_t ipp;
  fib_prefix_t pfx;
  u32 table_id = 0;
  u8 *name = 0;
  u8 is_add = 1;
  u8 got_prefix = 0;
  int rv;

  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "add"))
	is_add = 1;
      else if (unformat (line_input, "del"))
	is_add = 0;
      else if (unformat (line_input, "table %u", &table_id))
	;
      else if (unformat (line_input, "name %s", &name))
	;
      else if (unformat (line_input, "%U", unformat_ip_prefix, &ipp))
	got_prefix = 1;
      else
	{
	  err = clib_error_return (0, "unknown input `%U'",
				   format_unformat_error, line_input);
	  goto done;
	}
    }

  if (!got_prefix)
    {
      err = clib_error_return (0, "a target prefix is required");
      goto done;
    }
  if (name == 0)
    {
      err = clib_error_return (0, "a `name <name>' is required");
      goto done;
    }

  ip_prefix_to_fib_prefix (&ipp, &pfx);
  rv = probe_fib_add_del (&pfx, table_id, name, is_add);
  if (rv)
    err = clib_error_return (0, "probe_fib_add_del failed: %d", rv);

done:
  vec_free (name);
  unformat_free (line_input);
  return err;
}

VLIB_CLI_COMMAND (probe_fib_add_del_command, static) = {
  .path = "probe fib",
  .short_help = "probe fib [add|del] <prefix> table <id> name <name>",
  .function = probe_fib_add_del_cmd,
};

static clib_error_t *
probe_fib_show_cmd (vlib_main_t *vm, unformat_input_t *input,
		    vlib_cli_command_t *cmd)
{
  probe_main_t *pm = &probe_main;
  probe_fib_target_t *t;

  vlib_cli_output (vm, "scan interval: %.1fs, targets: %u", pm->scan_interval,
		   pool_elts (pm->targets));
  pool_foreach (t, pm->targets)
    {
      vlib_cli_output (
	vm, "  %s: %U table %u -> %s (changes %llu)", t->name,
	format_fib_prefix, &t->prefix, t->table_id,
	(t->have_result
	   ? (t->last_reachable ? "reachable" : "UNREACHABLE")
	   : "(pending)"),
	t->changes);
    }
  return 0;
}

VLIB_CLI_COMMAND (probe_fib_show_command, static) = {
  .path = "show probe fib",
  .short_help = "show probe fib",
  .function = probe_fib_show_cmd,
};

/* ---- init --------------------------------------------------------------- */

static clib_error_t *
probe_init (vlib_main_t *vm)
{
  probe_main_t *pm = &probe_main;
  clib_error_t *err;

  pm->vlib_main = vm;
  pm->vnet_main = vnet_get_main ();
  pm->targets = 0;
  pm->target_by_name = hash_create_string (0, sizeof (uword));
  pm->scan_interval = PROBE_DEFAULT_SCAN_INTERVAL;
  pm->process_node_index = probe_process_node.index;

  err = probe_api_hookup (vm);
  return err;
}

VLIB_INIT_FUNCTION (probe_init);

VLIB_PLUGIN_REGISTER () = {
  .version = VPP_BUILD_VER,
  .description = "off-control-plane data-plane observation (FIB reachability)",
};
