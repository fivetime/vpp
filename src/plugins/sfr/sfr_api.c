/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Source-FIB Redirect plugin contributors.
 */

#include <stddef.h>

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <sfr/sfr.h>
#include <vnet/fib/fib_table.h>

#include <vpp/app/version.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

/* define message IDs */
#include <vnet/format_fns.h>
#include <sfr/sfr.api_enum.h>
#include <sfr/sfr.api_types.h>

/**
 * Base message ID for the plugin
 */
static u32 sfr_base_msg_id;

#define REPLY_MSG_ID_BASE (sfr_base_msg_id)
#include <vlibapi/api_helper_macros.h>

static void
vl_api_sfr_plugin_get_version_t_handler (vl_api_sfr_plugin_get_version_t * mp)
{
  vl_api_sfr_plugin_get_version_reply_t *rmp;
  vl_api_registration_t *rp;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  rmp = vl_msg_api_alloc (sizeof (*rmp));
  rmp->_vl_msg_id =
    ntohs (VL_API_SFR_PLUGIN_GET_VERSION_REPLY + sfr_base_msg_id);
  rmp->context = mp->context;
  rmp->major = htonl (SFR_PLUGIN_VERSION_MAJOR);
  rmp->minor = htonl (SFR_PLUGIN_VERSION_MINOR);

  vl_api_send_msg (rp, (u8 *) rmp);
}

static void
vl_api_sfr_enable_disable_t_handler (vl_api_sfr_enable_disable_t * mp)
{
  vl_api_sfr_enable_disable_reply_t *rmp;
  fib_protocol_t fproto = (mp->is_ipv6 ?
			   FIB_PROTOCOL_IP6 : FIB_PROTOCOL_IP4);
  int rv;

  /*
   * Bounds-check the interface index before it reaches the data structures:
   * sfr_enable_disable() feeds sw_if_index straight into vec_validate, so an
   * out-of-range value (e.g. 0xFFFFFFFE) would otherwise trigger a huge
   * allocation / OOM. Rejects ~0 too.
   */
  VALIDATE_SW_IF_INDEX (mp);

  rv = sfr_enable_disable (fproto, ntohl (mp->sw_if_index),
			   ntohl (mp->table_id), mp->is_enable);

  BAD_SW_IF_INDEX_LABEL;

  REPLY_MACRO (VL_API_SFR_ENABLE_DISABLE_REPLY);
}

typedef struct sfr_dump_walk_ctx_t_
{
  vl_api_registration_t *rp;
  u32 context;
} sfr_dump_walk_ctx_t;

static int
sfr_send_details (fib_protocol_t fproto, u32 sw_if_index, u32 fib_index,
		  void *args)
{
  vl_api_sfr_details_t *mp;
  sfr_dump_walk_ctx_t *ctx;

  ctx = args;

  mp = vl_msg_api_alloc (sizeof (*mp));
  mp->_vl_msg_id = ntohs (VL_API_SFR_DETAILS + sfr_base_msg_id);

  mp->context = ctx->context;
  mp->sw_if_index = htonl (sw_if_index);
  mp->table_id = htonl (fib_table_get_table_id (fib_index, fproto));
  mp->is_ipv6 = (fproto == FIB_PROTOCOL_IP6);

  vl_api_send_msg (ctx->rp, (u8 *) mp);

  return (1);
}

static void
vl_api_sfr_dump_t_handler (vl_api_sfr_dump_t * mp)
{
  vl_api_registration_t *rp;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  sfr_dump_walk_ctx_t ctx = {
    .rp = rp,
    .context = mp->context,
  };

  sfr_walk (sfr_send_details, &ctx);
}

#include <sfr/sfr.api.c>

static clib_error_t *
sfr_api_init (vlib_main_t * vm)
{
  /* Ask for a correctly-sized block of API message decode slots */
  sfr_base_msg_id = setup_message_id_table ();
  sfr_main.msg_id_base = sfr_base_msg_id;

  return 0;
}

VLIB_INIT_FUNCTION (sfr_api_init);

VLIB_PLUGIN_REGISTER () = {
  .version = VPP_BUILD_VER,
  .description = "Source-FIB Redirect",
};
