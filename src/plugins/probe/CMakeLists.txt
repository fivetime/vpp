# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 probe plugin contributors.

add_vpp_plugin(probe
  SOURCES
  probe.c
  probe_api.c

  API_FILES
  probe.api
)
