# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 Source-FIB Redirect plugin contributors.

add_vpp_plugin(sfr
  SOURCES
  sfr_api.c
  sfr.c

  API_FILES
  sfr.api
)
