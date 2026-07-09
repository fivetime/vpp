/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 probe plugin contributors.
 *
 * probe: binary API. Registration/removal of FIB-reachability targets and a
 * dump of the current set + last verdicts. This is a configuration path only;
 * the periodic lookups run on the plugin's process node and results are read
 * out-of-band from the stats segment (see probe.c, docs/.../probe.rst).
 */

#include <stddef.h>

#include <vnet/vnet.h>
#include <vnet/plugin/plugin.h>
#include <probe/probe.h>
#include <vnet/fib/fib_table.h>
#include <vnet/ip/ip_types_api.h>
#include <vnet/classify/vnet_classify.h>

#include <vpp/app/version.h>

#include <vlibapi/api.h>
#include <vlibmemory/api.h>

/* define message IDs */
#include <vnet/format_fns.h>
#include <probe/probe.api_enum.h>
#include <probe/probe.api_types.h>

#define REPLY_MSG_ID_BASE (probe_main.msg_id_base)
#include <vlibapi/api_helper_macros.h>

static void
vl_api_probe_fib_add_del_t_handler (vl_api_probe_fib_add_del_t *mp)
{
  vl_api_probe_fib_add_del_reply_t *rmp;
  fib_prefix_t pfx;
  u8 *name;
  int rv;

  ip_prefix_decode (&mp->prefix, &pfx);

  /* mp->name is a fixed-size, null-terminated string field; copy it into a
   * vec so probe_fib_add_del() can own an independent, null-terminated key. */
  name = format (0, "%s", mp->name);

  rv = probe_fib_add_del (&pfx, ntohl (mp->table_id), name, mp->is_add);

  vec_free (name);

  REPLY_MACRO (VL_API_PROBE_FIB_ADD_DEL_REPLY);
}

static void
probe_fib_send_details (const probe_fib_target_t *t, vl_api_registration_t *rp,
			u32 context)
{
  vl_api_probe_fib_details_t *mp;

  mp = vl_msg_api_alloc (sizeof (*mp));
  clib_memset (mp, 0, sizeof (*mp));
  mp->_vl_msg_id = ntohs (VL_API_PROBE_FIB_DETAILS + probe_main.msg_id_base);
  mp->context = context;

  mp->table_id = htonl (t->table_id);
  ip_prefix_encode (&t->prefix, &mp->prefix);
  /* stat name is already null-terminated; clamp to the API field size. */
  strncpy ((char *) mp->name, (char *) t->name, ARRAY_LEN (mp->name) - 1);
  mp->reachable = t->have_result ? t->last_reachable : PROBE_UNREACHABLE;
  mp->dpo_type = 0;

  vl_api_send_msg (rp, (u8 *) mp);
}

static void
vl_api_probe_fib_dump_t_handler (vl_api_probe_fib_dump_t *mp)
{
  probe_main_t *pm = &probe_main;
  vl_api_registration_t *rp;
  probe_fib_target_t *t;

  rp = vl_api_client_index_to_registration (mp->client_index);
  if (rp == 0)
    return;

  pool_foreach (t, pm->targets)
    {
      probe_fib_send_details (t, rp, mp->context);
    }
}

/* Sanity ceiling on one batch, so a malformed match_len/key_len cannot make us
 * over-allocate the reply. The agent chunks well below this. */
#define PROBE_CLASSIFY_LOOKUP_MAX 65536

static void
vl_api_probe_classify_lookup_t_handler (vl_api_probe_classify_lookup_t *mp)
{
  vnet_classify_main_t *cm = &vnet_classify_main;
  vl_api_probe_classify_lookup_reply_t *rmp;
  u32 table_index = ntohl (mp->table_id);
  u32 key_len = ntohl (mp->key_len);
  u32 match_len = ntohl (mp->match_len);
  vnet_classify_table_t *t = 0;
  u32 count = 0;
  int rv = 0;
  u32 i;

  /* Derive the key count from the flat match buffer, validating against the
   * table's fixed match-buffer size. On any error we reply with count=0 (an
   * empty hits vector) and a non-zero retval; the loop below never runs (so t
   * is never dereferenced). */
  if (key_len == 0 || (match_len % key_len) != 0)
    {
      rv = VNET_API_ERROR_INVALID_VALUE;
      goto reply;
    }
  count = match_len / key_len;
  if (count > PROBE_CLASSIFY_LOOKUP_MAX)
    {
      rv = VNET_API_ERROR_INVALID_VALUE;
      count = 0;
      goto reply;
    }
  if (pool_is_free_index (cm->tables, table_index))
    {
      rv = VNET_API_ERROR_NO_SUCH_TABLE;
      count = 0;
      goto reply;
    }
  t = pool_elt_at_index (cm->tables, table_index);
  if (key_len != (t->skip_n_vectors + t->match_n_vectors) * sizeof (u32x4))
    {
      rv = VNET_API_ERROR_INVALID_VALUE;
      count = 0;
      goto reply;
    }

reply:
  REPLY_MACRO3 (VL_API_PROBE_CLASSIFY_LOOKUP_REPLY, count * sizeof (u32), ({
		  rmp->count = htonl (count);
		  for (i = 0; i < count; i++)
		    {
		      u8 *h = mp->match + (uword) i * key_len;
		      u32 hash = vnet_classify_hash_packet (t, h);
		      vnet_classify_entry_t *e =
			vnet_classify_find_entry (t, h, hash, 0.0);
		      rmp->hits[i] = htonl (e ? e->next_index : ~0);
		    }
		}));
}

#include <probe/probe.api.c>

clib_error_t *
probe_api_hookup (vlib_main_t *vm)
{
  /* Ask for a correctly-sized block of API message decode slots. */
  probe_main.msg_id_base = setup_message_id_table ();
  return 0;
}
