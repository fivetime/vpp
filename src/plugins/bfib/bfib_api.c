/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Bulk-FIB batch route plugin contributors.
 *
 * bfib: batch ip_route_add_del. Each item is handled through the SAME
 * vnet pipeline as vl_api_ip_route_add_del (fib_api_path_decode ->
 * fib_api_route_add_del, is_multipath=0), so per-route semantics are
 * byte-identical to the stock message; only the per-message overhead
 * is amortized. The reply carries a per-item (cookie, retval) pair in
 * request order, letting a pipelined client confirm each route with no
 * client-side batch state.
 */

#include <stddef.h>

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <vnet/fib/fib_table.h>
#include <vnet/fib/fib_api.h>
#include <vnet/ip/ip_types_api.h>

#include <vpp/app/version.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

/* define message IDs */
#include <vnet/format_fns.h>
#include <bfib/bfib.api_enum.h>
#include <bfib/bfib.api_types.h>

#define BFIB_PLUGIN_VERSION_MAJOR 1
#define BFIB_PLUGIN_VERSION_MINOR 0

/* Max paths per item — must match the fixed array in bfib.api. */
#define BFIB_ITEM_MAX_PATHS 4

static u32 bfib_base_msg_id;

#define REPLY_MSG_ID_BASE (bfib_base_msg_id)
#include <vlibapi/api_helper_macros.h>

static void
vl_api_bfib_plugin_get_version_t_handler (vl_api_bfib_plugin_get_version_t *
					  mp)
{
  vl_api_bfib_plugin_get_version_reply_t *rmp;
  vl_api_registration_t *rp;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  rmp->_vl_msg_id =
    ntohs (VL_API_BFIB_PLUGIN_GET_VERSION_REPLY + bfib_base_msg_id);
  rmp->context = mp->context;
  rmp->major = htonl (BFIB_PLUGIN_VERSION_MAJOR);
  rmp->minor = htonl (BFIB_PLUGIN_VERSION_MINOR);

  vl_api_send_msg (rp, (u8 *) rmp);
}

/* One item through the stock single-route pipeline (mirrors
 * ip_route_add_del_t_handler's prologue, minus stats_index). */
static int
bfib_item_add_del (u32 fib_index, fib_protocol_t fproto,
		   const vl_api_bfib_route_t * item)
{
  fib_route_path_t *rpaths = NULL, *rpath;
  fib_entry_flag_t entry_flags = FIB_ENTRY_FLAG_NONE;
  const vl_api_fib_path_t *apath;
  fib_prefix_t pfx;
  int rv = 0, ii;
  u8 n_paths = item->n_paths;

  if (n_paths > BFIB_ITEM_MAX_PATHS)
    return (VNET_API_ERROR_NO_PATHS_IN_ROUTE);

  ip_prefix_decode (&item->prefix, &pfx);
  if (pfx.fp_proto != fproto)
    return (VNET_API_ERROR_INVALID_ADDRESS_FAMILY);

  if (0 != n_paths)
    vec_validate (rpaths, n_paths - 1);

  for (ii = 0; ii < n_paths; ii++)
    {
      apath = &item->paths[ii];
      rpath = &rpaths[ii];

      rv = fib_api_path_decode ((vl_api_fib_path_t *) apath, rpath);

      if ((rpath->frp_flags & FIB_ROUTE_PATH_LOCAL) &&
	  (~0 == rpath->frp_sw_if_index))
	entry_flags |= (FIB_ENTRY_FLAG_CONNECTED | FIB_ENTRY_FLAG_LOCAL);

      if (0 != rv)
	goto out;
    }

  rv = fib_api_route_add_del (item->is_add, 0 /* is_multipath */, fib_index,
			      &pfx, FIB_SOURCE_API, entry_flags, rpaths);

out:
  vec_free (rpaths);
  return (rv);
}

static void
vl_api_bfib_route_batch_t_handler (vl_api_bfib_route_batch_t * mp)
{
  vl_api_bfib_route_batch_reply_t *rmp;
  vl_api_registration_t *rp;
  u32 fib_index4 = ~0, fib_index6 = ~0;
  u16 n_items = ntohs (mp->n_items);
  int overall = 0;
  u16 i;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  rmp = vl_msg_api_alloc (sizeof (*rmp) +
			  (uword) n_items * sizeof (rmp->results[0]));
  clib_memset (rmp, 0,
	       sizeof (*rmp) + (uword) n_items * sizeof (rmp->results[0]));
  rmp->_vl_msg_id = ntohs (VL_API_BFIB_ROUTE_BATCH_REPLY + bfib_base_msg_id);
  rmp->context = mp->context;

  for (i = 0; i < n_items; i++)
    {
      const vl_api_bfib_route_t *item = &mp->items[i];
      fib_prefix_t pfx;
      u32 *fib_indexp;
      int rv;

      rmp->results[i].cookie = item->cookie;	/* echo verbatim (opaque) */

      /* Resolve the table per address family lazily, once per batch;
       * both families may appear in one batch (client convenience). */
      ip_prefix_decode (&item->prefix, &pfx);
      fib_indexp =
	(FIB_PROTOCOL_IP6 == pfx.fp_proto) ? &fib_index6 : &fib_index4;
      if (~0 == *fib_indexp)
	{
	  rv = fib_api_table_id_decode (pfx.fp_proto, ntohl (mp->table_id),
					fib_indexp);
	  if (0 != rv)
	    {
	      rmp->results[i].retval = htonl (rv);
	      continue;
	    }
	}

      rv = bfib_item_add_del (*fib_indexp, pfx.fp_proto, item);
      rmp->results[i].retval = htonl (rv);
    }

  rmp->retval = htonl (overall);
  rmp->n_items = htons (n_items);

  vl_api_send_msg (rp, (u8 *) rmp);
}

#include <bfib/bfib.api.c>

static clib_error_t *
bfib_api_init (vlib_main_t * vm)
{
  bfib_base_msg_id = setup_message_id_table ();

  return 0;
}

VLIB_INIT_FUNCTION (bfib_api_init);

VLIB_PLUGIN_REGISTER () = {
  .version = VPP_BUILD_VER,
  .description = "Bulk-FIB batch route add/del",
};
