/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Source-FIB Redirect plugin contributors.
 */

#ifndef __SFR_H__
#define __SFR_H__

#include <vlib/vlib.h>
#include <vnet/fib/fib_types.h>

#define SFR_PLUGIN_VERSION_MAJOR 1
#define SFR_PLUGIN_VERSION_MINOR 0

/**
 * Source-FIB Redirect (SFR) plugin main.
 *
 * SFR is the egress/source-side sibling of plain destination forwarding:
 * an input feature that looks the packet's SOURCE address up in a dedicated
 * FIB table and, on a hit, redirects the packet via the resolved next-hop.
 * On a miss (source not present) or a dead next-hop the packet fails open
 * and continues down the input feature arc on the normal L3 path.
 */
typedef struct sfr_main_t_
{
  /**
   * Per-protocol, per-interface source-FIB index. Used in the data-plane.
   * Indexed by [fib_protocol_t][sw_if_index]. ~0 => not enabled.
   */
  u32 *fib_index_by_sw_if_index[FIB_PROTOCOL_MAX];

  /**
   * Base message ID for the API
   */
  u16 msg_id_base;
} sfr_main_t;

extern sfr_main_t sfr_main;

/**
 * Enable/disable source-FIB redirect on an interface, bound to the
 * source-FIB identified by table_id. The table is created (and locked) if
 * it does not yet exist.
 */
extern int sfr_enable_disable (fib_protocol_t fproto, u32 sw_if_index,
			       u32 table_id, u8 is_enable);

typedef int (*sfr_walk_cb_t) (fib_protocol_t fproto, u32 sw_if_index,
			      u32 fib_index, void *ctx);

extern void sfr_walk (sfr_walk_cb_t cb, void *ctx);

#endif /* __SFR_H__ */
