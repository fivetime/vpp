# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Bulk-FIB batch route plugin contributors.

add_vpp_plugin(bfib
  SOURCES
  bfib_api.c

  API_FILES
  bfib.api
)
