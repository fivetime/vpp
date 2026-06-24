/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Source-FIB Redirect plugin contributors.
 */

#include <sfr/sfr.h>

#include <vnet/fib/fib_table.h>
#include <vnet/fib/ip4_fib.h>
#include <vnet/fib/ip6_fib.h>
#include <vnet/dpo/load_balance.h>
#include <vnet/dpo/drop_dpo.h>
#include <vnet/feature/feature.h>
#include <vnet/ip/ip4.h>
#include <vnet/ip/ip6.h>
#include <vnet/adj/adj.h>		/* IP4/IP6_LOOKUP_NEXT_NODES + IP_LOOKUP_NEXT_* */

sfr_main_t sfr_main;

/**
 * Forward declarations
 */
extern vlib_node_registration_t sfr_ip4_node;
extern vlib_node_registration_t sfr_ip6_node;

/**
 * The FIB source SFR uses to lock the source-FIB tables it binds.
 */
#define SFR_FIB_SOURCE FIB_SOURCE_API

int
sfr_enable_disable (fib_protocol_t fproto, u32 sw_if_index, u32 table_id,
		    u8 is_enable)
{
  sfr_main_t *sm = &sfr_main;
  u32 fib_index;

  if (FIB_PROTOCOL_IP4 != fproto && FIB_PROTOCOL_IP6 != fproto)
    return (VNET_API_ERROR_INVALID_PROTOCOL);

  vec_validate_init_empty (sm->fib_index_by_sw_if_index[fproto], sw_if_index,
			   ~0);

  if (is_enable)
    {
      if (~0 != sm->fib_index_by_sw_if_index[fproto][sw_if_index])
	{
	  /*
	   * Already bound. Re-assert the data-plane feature rather than no-op:
	   * an earlier enable may have raced the interface coming up (e.g. an
	   * lcp tap still being set up) so the feature, though recorded, never
	   * made it into the interface's compiled feature config — left listed
	   * by 'show interface features' yet skipped in the data path. A
	   * disable+enable forces a recompile; the feature count nets back to 1
	   * so this stays idempotent. Lets a control-plane reconcile (which
	   * re-enables periodically) self-heal a stuck binding.
	   */
	  vnet_feature_enable_disable (
	    (FIB_PROTOCOL_IP4 == fproto ? "ip4-unicast" : "ip6-unicast"),
	    (FIB_PROTOCOL_IP4 == fproto ? "sfr-input-ip4" : "sfr-input-ip6"),
	    sw_if_index, 0, NULL, 0);
	  vnet_feature_enable_disable (
	    (FIB_PROTOCOL_IP4 == fproto ? "ip4-unicast" : "ip6-unicast"),
	    (FIB_PROTOCOL_IP4 == fproto ? "sfr-input-ip4" : "sfr-input-ip6"),
	    sw_if_index, 1, NULL, 0);
	  return (0);
	}

      /*
       * find or create the source-FIB table and hold a lock on it for as
       * long as the feature is enabled on this interface.
       */
      fib_index =
	fib_table_find_or_create_and_lock (fproto, table_id, SFR_FIB_SOURCE);

      sm->fib_index_by_sw_if_index[fproto][sw_if_index] = fib_index;

      vnet_feature_enable_disable (
	(FIB_PROTOCOL_IP4 == fproto ? "ip4-unicast" : "ip6-unicast"),
	(FIB_PROTOCOL_IP4 == fproto ? "sfr-input-ip4" : "sfr-input-ip6"),
	sw_if_index, 1, NULL, 0);
    }
  else
    {
      if (~0 == sm->fib_index_by_sw_if_index[fproto][sw_if_index])
	return (VNET_API_ERROR_NO_SUCH_ENTRY);

      fib_index = sm->fib_index_by_sw_if_index[fproto][sw_if_index];

      vnet_feature_enable_disable (
	(FIB_PROTOCOL_IP4 == fproto ? "ip4-unicast" : "ip6-unicast"),
	(FIB_PROTOCOL_IP4 == fproto ? "sfr-input-ip4" : "sfr-input-ip6"),
	sw_if_index, 0, NULL, 0);

      sm->fib_index_by_sw_if_index[fproto][sw_if_index] = ~0;

      /*
       * release the lock on the source-FIB taken at enable time.
       */
      fib_table_unlock (fib_index, fproto, SFR_FIB_SOURCE);
    }

  return (0);
}

void
sfr_walk (sfr_walk_cb_t cb, void *ctx)
{
  sfr_main_t *sm = &sfr_main;
  fib_protocol_t fproto;
  u32 sw_if_index;

  FOR_EACH_FIB_IP_PROTOCOL (fproto)
  {
    vec_foreach_index (sw_if_index, sm->fib_index_by_sw_if_index[fproto])
    {
      u32 fib_index = sm->fib_index_by_sw_if_index[fproto][sw_if_index];
      if (~0 == fib_index)
	continue;
      if (!cb (fproto, sw_if_index, fib_index, ctx))
	return;
    }
  }
}

static clib_error_t *
sfr_enable_cmd (vlib_main_t * vm,
		unformat_input_t * input, vlib_cli_command_t * cmd)
{
  u32 sw_if_index, table_id;
  fib_protocol_t fproto;
  vnet_main_t *vnm;
  u32 is_disable;

  is_disable = 0;
  sw_if_index = table_id = ~0;
  vnm = vnet_get_main ();
  fproto = FIB_PROTOCOL_MAX;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "disable"))
	is_disable = 1;
      else if (unformat (input, "enable"))
	is_disable = 0;
      else if (unformat (input, "ip4"))
	fproto = FIB_PROTOCOL_IP4;
      else if (unformat (input, "ip6"))
	fproto = FIB_PROTOCOL_IP6;
      else if (unformat (input, "table %d", &table_id))
	;
      else if (unformat (input, "%U",
			 unformat_vnet_sw_interface, vnm, &sw_if_index))
	;
      else
	return (clib_error_return (0, "unknown input '%U'",
				   format_unformat_error, input));
    }

  if (~0 == sw_if_index)
    return (clib_error_return (0, "invalid interface name"));
  if (FIB_PROTOCOL_MAX == fproto)
    return (clib_error_return (0, "Specify either ip4 or ip6"));
  if (!is_disable && ~0 == table_id)
    return (clib_error_return (0, "Specify a source-FIB table id"));

  sfr_enable_disable (fproto, sw_if_index, table_id, !is_disable);

  return (NULL);
}

/**
 * Enable/disable source-FIB redirect on an interface.
 */
VLIB_CLI_COMMAND (sfr_enable_cmd_node, static) = {
  .path = "sfr",
  .function = sfr_enable_cmd,
  .short_help = "sfr <ip4|ip6> [disable] table <id> <interface>",
  // this is not MP safe
};

static clib_error_t *
sfr_show_cmd (vlib_main_t * vm,
	      unformat_input_t * input, vlib_cli_command_t * cmd)
{
  sfr_main_t *sm = &sfr_main;
  fib_protocol_t fproto;
  u32 sw_if_index;
  vnet_main_t *vnm;

  vnm = vnet_get_main ();

  FOR_EACH_FIB_IP_PROTOCOL (fproto)
  {
    vec_foreach_index (sw_if_index, sm->fib_index_by_sw_if_index[fproto])
    {
      u32 fib_index = sm->fib_index_by_sw_if_index[fproto][sw_if_index];
      if (~0 == fib_index)
	continue;
      vlib_cli_output (vm, "%U: %U source-FIB table %d (fib-index %d)",
		       format_vnet_sw_if_index_name, vnm, sw_if_index,
		       format_fib_protocol, fproto,
		       fib_table_get_table_id (fib_index, fproto), fib_index);
    }
  }
  return (NULL);
}

VLIB_CLI_COMMAND (sfr_show_cmd_node, static) = {
  .path = "show sfr",
  .function = sfr_show_cmd,
  .short_help = "show sfr",
  .is_mp_safe = 1,
};

typedef enum sfr_next_t_
{
  SFR_NEXT_DROP,
  SFR_N_NEXT,
} sfr_next_t;

typedef struct sfr_input_trace_t_
{
  u32 next;
  u32 fib_index;
  index_t lbi;
} sfr_input_trace_t;

typedef enum
{
#define sfr_error(n,s) SFR_ERROR_##n,
#include "sfr_error.def"
#undef sfr_error
  SFR_N_ERROR,
} sfr_error_t;

always_inline uword
sfr_input_inline (vlib_main_t * vm,
		  vlib_node_runtime_t * node,
		  vlib_frame_t * frame, fib_protocol_t fproto)
{
  sfr_main_t *sm = &sfr_main;
  u32 n_left_from, *from, *to_next, next_index, redirects, passes;

  from = vlib_frame_vector_args (frame);
  n_left_from = frame->n_vectors;
  next_index = node->cached_next_index;
  redirects = passes = 0;

  while (n_left_from > 0)
    {
      u32 n_left_to_next;

      vlib_get_next_frame (vm, node, next_index, to_next, n_left_to_next);

      while (n_left_from > 0 && n_left_to_next > 0)
	{
	  u32 next0 = IP_LOOKUP_NEXT_DROP;
	  const load_balance_t *lb0;
	  const dpo_id_t *dpo0;
	  vlib_buffer_t *b0;
	  u32 bi0, sw_if_index0, fib_index0;
	  index_t lbi0;

	  bi0 = from[0];
	  to_next[0] = bi0;
	  from += 1;
	  to_next += 1;
	  n_left_from -= 1;
	  n_left_to_next -= 1;

	  b0 = vlib_get_buffer (vm, bi0);
	  sw_if_index0 = vnet_buffer (b0)->sw_if_index[VLIB_RX];

	  ASSERT (vec_len (sm->fib_index_by_sw_if_index[fproto]) >
		  sw_if_index0);
	  fib_index0 = sm->fib_index_by_sw_if_index[fproto][sw_if_index0];

	  /*
	   * look the packet's SOURCE address up in the bound source-FIB,
	   * using the same forwarding lookup the L3 path uses for the
	   * destination. The lookup is address-agnostic.
	   */
	  if (FIB_PROTOCOL_IP4 == fproto)
	    {
	      const ip4_header_t *ip0 = vlib_buffer_get_current (b0);
	      lbi0 = ip4_fib_forwarding_lookup (fib_index0, &ip0->src_address);
	    }
	  else
	    {
	      const ip6_header_t *ip0 = vlib_buffer_get_current (b0);
	      lbi0 = ip6_fib_table_fwding_lookup (fib_index0,
						  &ip0->src_address);
	    }

	  lb0 = load_balance_get (lbi0);
	  dpo0 = load_balance_get_bucket_i (lb0, 0);

	  if (dpo_is_drop (dpo0))
	    {
	      /*
	       * miss (source not a member, hits the table's drop default
	       * route) or a dead next-hop (e.g. BFD down collapsed the
	       * load-balance to drop): fail open and continue down the
	       * input feature arc on the normal destination L3 path.
	       */
	      vnet_feature_next (&next0, b0);
	      passes++;
	    }
	  else
	    {
	      /*
	       * hit with a live next-hop: redirect via the resolved
	       * adjacency, bypassing the destination lookup.
	       */
	      vnet_buffer (b0)->ip.adj_index[VLIB_TX] = dpo0->dpoi_index;
	      next0 = dpo0->dpoi_next_node;
	      redirects++;
	    }

	  if (PREDICT_FALSE (b0->flags & VLIB_BUFFER_IS_TRACED))
	    {
	      sfr_input_trace_t *tr;

	      tr = vlib_add_trace (vm, node, b0, sizeof (*tr));
	      tr->next = next0;
	      tr->fib_index = fib_index0;
	      tr->lbi = lbi0;
	    }

	  /* verify speculative enqueue, maybe switch current next frame */
	  vlib_validate_buffer_enqueue_x1 (vm, node, next_index,
					   to_next, n_left_to_next, bi0,
					   next0);
	}

      vlib_put_next_frame (vm, node, next_index, n_left_to_next);
    }

  vlib_node_increment_counter (
    vm, (fproto == FIB_PROTOCOL_IP6 ? sfr_ip6_node.index : sfr_ip4_node.index),
    SFR_ERROR_REDIRECTED, redirects);
  vlib_node_increment_counter (
    vm, (fproto == FIB_PROTOCOL_IP6 ? sfr_ip6_node.index : sfr_ip4_node.index),
    SFR_ERROR_PASSED, passes);

  return frame->n_vectors;
}

static uword
sfr_input_ip4 (vlib_main_t * vm,
	       vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  return sfr_input_inline (vm, node, frame, FIB_PROTOCOL_IP4);
}

static uword
sfr_input_ip6 (vlib_main_t * vm,
	       vlib_node_runtime_t * node, vlib_frame_t * frame)
{
  return sfr_input_inline (vm, node, frame, FIB_PROTOCOL_IP6);
}

static u8 *
format_sfr_input_trace (u8 * s, va_list * args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  sfr_input_trace_t *t = va_arg (*args, sfr_input_trace_t *);

  s = format (s, " next %d source-fib-index %d lb-index %d",
	      t->next, t->fib_index, t->lbi);
  return s;
}

static char *sfr_error_strings[] = {
#define sfr_error(n,s) s,
#include "sfr_error.def"
#undef sfr_error
};

VLIB_REGISTER_NODE (sfr_ip4_node) =
{
  .function = sfr_input_ip4,
  .name = "sfr-input-ip4",
  .vector_size = sizeof (u32),
  .format_trace = format_sfr_input_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = SFR_N_ERROR,
  .error_strings = sfr_error_strings,
  /* SFR-4x:注册整套标准 ip4 lookup next-node。redirect 时 next0 = 邻接 dpo 的
     dpoi_next_node(IP_LOOKUP_NEXT_REWRITE=5 等标准值),必须能在本节点 next 表里映射到
     ip4-rewrite,否则越界落 error-punt、包被丢不转发。ip4-lookup 同款做法。fail-open 的
     vnet_feature_next 用 feature 框架追加的 arc-next 槽(>IP_LOOKUP_N_NEXT),与此并存。 */
  .n_next_nodes = IP4_LOOKUP_N_NEXT,
  .next_nodes = IP4_LOOKUP_NEXT_NODES,
};

VLIB_REGISTER_NODE (sfr_ip6_node) =
{
  .function = sfr_input_ip6,
  .name = "sfr-input-ip6",
  .vector_size = sizeof (u32),
  .format_trace = format_sfr_input_trace,
  .type = VLIB_NODE_TYPE_INTERNAL,
  .n_errors = SFR_N_ERROR,
  .error_strings = sfr_error_strings,
  /* SFR-4x:同 ip4 —— 整套标准 ip6 lookup next-node,redirect 的 dpoi_next_node 映射到
     ip6-rewrite。 */
  .n_next_nodes = IP6_LOOKUP_N_NEXT,
  .next_nodes = IP6_LOOKUP_NEXT_NODES,
};

VNET_FEATURE_INIT (sfr_ip4_feat, static) = {
  .arc_name = "ip4-unicast",
  .node_name = "sfr-input-ip4",
  .runs_after = VNET_FEATURES ("ip4-full-reassembly-feature",
			       "ip4-sv-reassembly-feature"),
  .runs_before = VNET_FEATURES ("ip4-lookup"),
};

VNET_FEATURE_INIT (sfr_ip6_feat, static) = {
  .arc_name = "ip6-unicast",
  .node_name = "sfr-input-ip6",
  .runs_after = VNET_FEATURES ("ip6-full-reassembly-feature",
			       "ip6-sv-reassembly-feature"),
  .runs_before = VNET_FEATURES ("ip6-lookup"),
};
