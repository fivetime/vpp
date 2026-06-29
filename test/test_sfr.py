#!/usr/bin/env python3
"""Source-FIB Redirect (SFR) plugin functional tests.

SFR is a source-based redirect implemented as an input feature on the L3 arc
(nodes ``sfr-input-ip4`` / ``sfr-input-ip6``, run before ``ip4-lookup`` /
``ip6-lookup``). For every packet it looks the packet's *source* address up in
a per-interface bound source-FIB table using the ordinary forwarding lookup:

  * HIT with a live next-hop -> redirect via the resolved adjacency, bypassing
    the destination lookup (``SFR_ERROR_REDIRECTED``).
  * MISS (source hits the table drop default route) or a DEAD next-hop (drop
    dpo) -> FAIL OPEN, continue down the arc to the normal destination L3 path
    (``SFR_ERROR_PASSED``).
  * ECMP source route (>1 live next-hop) -> flow-hash pins a flow to one bucket.

These tests drive the plugin directly via its binary API
(``sfr_enable_disable`` / ``sfr_dump`` / ``sfr_plugin_get_version``) and crafted
scapy packets, mirroring the topology and idioms of ``test_abf.py`` (an input
feature / source-policy redirect) and ``test_ip_ecmp.py`` (the ECMP bucket
spray + flow-pin idiom).

Topology:
  pg0 = ingress (SFR feature bound here)
  pg1 = DESTINATION-route egress (the normal L3 path)
  pg2 = SOURCE-REDIRECT egress (and ECMP bucket A)
  pg3 = ECMP bucket B
"""

import unittest

from config import config
from framework import VppTestCase
from asfframework import VppTestRunner
from vpp_ip_route import (
    VppIpRoute,
    VppRoutePath,
    VppIpTable,
    FibPathType,
)

from scapy.packet import Raw
from scapy.layers.l2 import Ether
from scapy.layers.inet import IP, UDP
from scapy.layers.inet6 import IPv6

NUM_PKTS = 67

# vnet/error.h codes (not exposed as API enums); see src/vnet/error.h
VNET_API_ERROR_NO_SUCH_ENTRY = -6
VNET_API_ERROR_INSTANCE_IN_USE = -147

# The source-FIB table SFR binds to pg0.
SRC_FIB_TABLE_ID = 10
OTHER_FIB_TABLE_ID = 11

# Per-node error-counter paths, from src/plugins/sfr/sfr_error.def. The
# counter strings are "packets redirected via source-FIB" (REDIRECTED) and
# "packets passed through (non-member or fail-open)" (PASSED). We match on a
# stable keyword to be robust to any stat-segment name sanitising.
SFR4_REDIRECTED = ("sfr-input-ip4", "redirected")
SFR4_PASSED = ("sfr-input-ip4", "passed")
SFR6_REDIRECTED = ("sfr-input-ip6", "redirected")
SFR6_PASSED = ("sfr-input-ip6", "passed")


@unittest.skipIf("sfr" in config.excluded_plugins, "Exclude SFR plugin tests")
class TestSfr(VppTestCase):
    """Source-FIB Redirect (SFR) Test Case"""

    @classmethod
    def setUpClass(cls):
        super(TestSfr, cls).setUpClass()

    @classmethod
    def tearDownClass(cls):
        super(TestSfr, cls).tearDownClass()

    def setUp(self):
        super(TestSfr, self).setUp()

        # pg0 ingress, pg1 dest path, pg2 source-redirect path / ECMP-A,
        # pg3 ECMP-B.
        self.create_pg_interfaces(range(4))

        for i in self.pg_interfaces:
            i.admin_up()
            i.config_ip4()
            i.resolve_arp()
            i.config_ip6()
            i.resolve_ndp()

        # The source-FIB table bound on pg0. SFR locks it on enable; it lives
        # for the duration of the binding. Register so the framework tears it
        # down if a test leaves it bound.
        self.src_table4 = VppIpTable(self, SRC_FIB_TABLE_ID)
        self.src_table4.add_vpp_config()
        self.src_table6 = VppIpTable(self, SRC_FIB_TABLE_ID, is_ip6=1)
        self.src_table6.add_vpp_config()

    def tearDown(self):
        # Best-effort disable of any binding a test left in place, so the
        # source-FIB lock is released before the table is removed.
        for is_ipv6 in (False, True):
            try:
                self.vapi.sfr_enable_disable(
                    sw_if_index=self.pg0.sw_if_index,
                    table_id=SRC_FIB_TABLE_ID,
                    is_ipv6=is_ipv6,
                    is_enable=False,
                )
            except Exception:
                pass

        for i in self.pg_interfaces:
            i.unconfig_ip4()
            i.unconfig_ip6()
            i.admin_down()
        super(TestSfr, self).tearDown()

    # ------------------------------------------------------------------ #
    # helpers
    # ------------------------------------------------------------------ #
    def enable_sfr(self, sw_if_index, table_id, is_ipv6):
        self.vapi.sfr_enable_disable(
            sw_if_index=sw_if_index,
            table_id=table_id,
            is_ipv6=is_ipv6,
            is_enable=True,
        )

    def disable_sfr(self, sw_if_index, table_id, is_ipv6):
        self.vapi.sfr_enable_disable(
            sw_if_index=sw_if_index,
            table_id=table_id,
            is_ipv6=is_ipv6,
            is_enable=False,
        )

    def get_sfr_counter(self, spec):
        """Sum the SFR per-node error counter identified by (node, keyword).

        Resolves the exact stat-segment name under /err/<node>/ that contains
        the keyword, so we don't depend on whether the stat segment preserves
        spaces/parentheses in the error string verbatim.
        """
        node, keyword = spec
        prefix = "/err/%s/" % node
        try:
            names = self.statistics.ls([prefix])
        except Exception:
            names = []
        total = 0
        found = False
        for name in names:
            if name.startswith(prefix) and keyword in name:
                total += self.statistics.get_err_counter(name)
                found = True
        if found:
            return total
        # counter not yet materialised (node never executed) -> 0
        return 0

    # ------------------------------------------------------------------ #
    # 1. REDIRECT-ON-HIT
    # ------------------------------------------------------------------ #
    def test_sfr4_redirect_on_hit(self):
        """IPv4 redirect on source-FIB hit"""
        self._redirect_on_hit(is_ipv6=False)

    def test_sfr6_redirect_on_hit(self):
        """IPv6 redirect on source-FIB hit"""
        self._redirect_on_hit(is_ipv6=True)

    def _redirect_on_hit(self, is_ipv6):
        ip_l = IPv6 if is_ipv6 else IP
        dst = "2001:db8:1::1" if is_ipv6 else "203.0.113.1"
        src = "2001:db8:2::1" if is_ipv6 else "192.0.2.1"
        plen = 128 if is_ipv6 else 32
        nh1 = self.pg1.remote_ip6 if is_ipv6 else self.pg1.remote_ip4
        nh2 = self.pg2.remote_ip6 if is_ipv6 else self.pg2.remote_ip4
        redirected = SFR6_REDIRECTED if is_ipv6 else SFR4_REDIRECTED

        # main-table destination route -> via pg1 (the normal L3 path)
        dst_route = VppIpRoute(
            self, dst, plen, [VppRoutePath(nh1, self.pg1.sw_if_index)]
        )
        dst_route.add_vpp_config()

        # source-FIB route for the test SOURCE -> via pg2 (the redirect path)
        src_route = VppIpRoute(
            self,
            src,
            plen,
            [VppRoutePath(nh2, self.pg2.sw_if_index)],
            table_id=SRC_FIB_TABLE_ID,
        )
        src_route.add_vpp_config()

        self.enable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, is_ipv6)

        c0 = self.get_sfr_counter(redirected)

        p = (
            Ether(src=self.pg0.remote_mac, dst=self.pg0.local_mac)
            / ip_l(src=src, dst=dst)
            / UDP(sport=1234, dport=1234)
            / Raw(b"\xa5" * 100)
        )

        # the source is in the source-FIB -> egress pg2 (NOT pg1)
        rx = self.send_and_expect(self.pg0, p * NUM_PKTS, self.pg2)
        self.pg1.assert_nothing_captured(remark="redirected away from dest path")
        for rxp in rx:
            self.assertEqual(rxp[ip_l].dst, dst)

        c1 = self.get_sfr_counter(redirected)
        self.assertEqual(c1 - c0, NUM_PKTS)

        self.disable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, is_ipv6)

    # ------------------------------------------------------------------ #
    # 2. FAIL-OPEN-ON-MISS
    # ------------------------------------------------------------------ #
    def test_sfr4_fail_open_on_miss(self):
        """IPv4 fail open when source not in source-FIB"""
        self._fail_open_on_miss(is_ipv6=False)

    def test_sfr6_fail_open_on_miss(self):
        """IPv6 fail open when source not in source-FIB"""
        self._fail_open_on_miss(is_ipv6=True)

    def _fail_open_on_miss(self, is_ipv6):
        ip_l = IPv6 if is_ipv6 else IP
        dst = "2001:db8:1::1" if is_ipv6 else "203.0.113.1"
        # a source NOT present in the source-FIB
        src = "2001:db8:dead::1" if is_ipv6 else "198.51.100.7"
        plen = 128 if is_ipv6 else 32
        nh1 = self.pg1.remote_ip6 if is_ipv6 else self.pg1.remote_ip4
        passed = SFR6_PASSED if is_ipv6 else SFR4_PASSED

        dst_route = VppIpRoute(
            self, dst, plen, [VppRoutePath(nh1, self.pg1.sw_if_index)]
        )
        dst_route.add_vpp_config()

        # bind an (empty-of-this-source) source-FIB on pg0
        self.enable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, is_ipv6)

        c0 = self.get_sfr_counter(passed)

        p = (
            Ether(src=self.pg0.remote_mac, dst=self.pg0.local_mac)
            / ip_l(src=src, dst=dst)
            / UDP(sport=1234, dport=1234)
            / Raw(b"\xa5" * 100)
        )

        # source misses the source-FIB -> fail open to the dest path on pg1
        self.send_and_expect(self.pg0, p * NUM_PKTS, self.pg1)
        self.pg2.assert_nothing_captured(remark="no redirect on miss")

        c1 = self.get_sfr_counter(passed)
        self.assertEqual(c1 - c0, NUM_PKTS)

        self.disable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, is_ipv6)

    # ------------------------------------------------------------------ #
    # 3. FAIL-OPEN-ON-DEAD-NEXTHOP
    # ------------------------------------------------------------------ #
    def test_sfr4_fail_open_on_dead_nexthop(self):
        """IPv4 fail open when source-FIB next-hop is a drop"""
        self._fail_open_on_dead_nexthop(is_ipv6=False)

    def test_sfr6_fail_open_on_dead_nexthop(self):
        """IPv6 fail open when source-FIB next-hop is a drop"""
        self._fail_open_on_dead_nexthop(is_ipv6=True)

    def _fail_open_on_dead_nexthop(self, is_ipv6):
        ip_l = IPv6 if is_ipv6 else IP
        dst = "2001:db8:1::1" if is_ipv6 else "203.0.113.1"
        # A source distinct from the redirect-on-hit test's source. Both tests
        # bind table 10 on pg0 and register a /32/128 for their source; sharing
        # one address makes the two VppIpRoute objects collide on the same
        # object-registry key (table-10-<src>), so the dead test's DROP route is
        # "Skipping removal ... not present" at teardown and leaks into the
        # later redirect test, which then fails open instead of redirecting.
        src = "2001:db8:2::66" if is_ipv6 else "192.0.2.66"
        plen = 128 if is_ipv6 else 32
        nh1 = self.pg1.remote_ip6 if is_ipv6 else self.pg1.remote_ip4
        passed = SFR6_PASSED if is_ipv6 else SFR4_PASSED

        dst_route = VppIpRoute(
            self, dst, plen, [VppRoutePath(nh1, self.pg1.sw_if_index)]
        )
        dst_route.add_vpp_config()

        # source-FIB route whose next-hop is an explicit DROP -> the lookup
        # resolves to a drop dpo, the same as a BFD-down collapse. SFR must
        # fail open rather than blackhole the packet.
        src_route = VppIpRoute(
            self,
            src,
            plen,
            [
                VppRoutePath(
                    "::" if is_ipv6 else "0.0.0.0",
                    0xFFFFFFFF,
                    type=FibPathType.FIB_PATH_TYPE_DROP,
                )
            ],
            table_id=SRC_FIB_TABLE_ID,
        )
        src_route.add_vpp_config()

        self.enable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, is_ipv6)

        c0 = self.get_sfr_counter(passed)

        p = (
            Ether(src=self.pg0.remote_mac, dst=self.pg0.local_mac)
            / ip_l(src=src, dst=dst)
            / UDP(sport=1234, dport=1234)
            / Raw(b"\xa5" * 100)
        )

        # dead source next-hop -> fail open to dest path on pg1
        self.send_and_expect(self.pg0, p * NUM_PKTS, self.pg1)
        self.pg2.assert_nothing_captured(remark="dead next-hop must not redirect")

        c1 = self.get_sfr_counter(passed)
        self.assertEqual(c1 - c0, NUM_PKTS)

        self.disable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, is_ipv6)

    # ------------------------------------------------------------------ #
    # 4. ECMP source route
    # ------------------------------------------------------------------ #
    def test_sfr4_ecmp(self):
        """IPv4 ECMP source route sprays buckets, pins a flow"""
        self._ecmp(is_ipv6=False)

    def test_sfr6_ecmp(self):
        """IPv6 ECMP source route sprays buckets, pins a flow"""
        self._ecmp(is_ipv6=True)

    def _ecmp(self, is_ipv6):
        ip_l = IPv6 if is_ipv6 else IP
        dst = "2001:db8:1::1" if is_ipv6 else "203.0.113.1"
        # the ECMP source prefix: a /24 (v4) or /64 (v6) so many sources match
        src_net = "2001:db8:5::" if is_ipv6 else "10.10.10.0"
        src_plen = 64 if is_ipv6 else 24
        dst_plen = 128 if is_ipv6 else 32

        dst_route = VppIpRoute(
            self,
            dst,
            dst_plen,
            [
                VppRoutePath(
                    self.pg1.remote_ip6 if is_ipv6 else self.pg1.remote_ip4,
                    self.pg1.sw_if_index,
                )
            ],
        )
        dst_route.add_vpp_config()

        # source-FIB ECMP route with TWO live next-hops: pg2 + pg3
        nh2 = self.pg2.remote_ip6 if is_ipv6 else self.pg2.remote_ip4
        nh3 = self.pg3.remote_ip6 if is_ipv6 else self.pg3.remote_ip4
        src_route = VppIpRoute(
            self,
            src_net,
            src_plen,
            [
                VppRoutePath(nh2, self.pg2.sw_if_index),
                VppRoutePath(nh3, self.pg3.sw_if_index),
            ],
            table_id=SRC_FIB_TABLE_ID,
        )
        src_route.add_vpp_config()

        self.enable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, is_ipv6)

        # spray: vary the source within the prefix so the flow-hash lands in
        # both buckets (the test_ip_ecmp idiom). Expect packets on BOTH pg2
        # and pg3, none on pg1.
        base = 1
        pkts = []
        for i in range(NUM_PKTS):
            if is_ipv6:
                src = "2001:db8:5::%x" % (base + i)
            else:
                src = "10.10.10.%d" % (base + i)
            pkts.append(
                Ether(src=self.pg0.remote_mac, dst=self.pg0.local_mac)
                / ip_l(src=src, dst=dst)
                / UDP(sport=1234, dport=1234 + i)
                / Raw(b"\xa5" * 100)
            )

        self.pg0.add_stream(pkts)
        self.pg_enable_capture(self.pg_interfaces)
        self.pg_start()

        # split is data-dependent, so grab whatever each bucket captured
        # (test_ip_ecmp idiom) rather than asserting an exact per-if count
        rx2 = self.pg2._get_capture()
        rx3 = self.pg3._get_capture()
        self.pg1.assert_nothing_captured(remark="ECMP redirect, not dest path")

        n2 = 0 if rx2 is None else len(rx2)
        n3 = 0 if rx3 is None else len(rx3)
        self.assertGreater(n2, 0, "no traffic in ECMP bucket pg2")
        self.assertGreater(n3, 0, "no traffic in ECMP bucket pg3")
        self.assertEqual(n2 + n3, NUM_PKTS)

        # a SINGLE flow (one fixed src + sport) must pin to exactly one bucket
        flow_src = "2001:db8:5::abc" if is_ipv6 else "10.10.10.200"
        flow = (
            Ether(src=self.pg0.remote_mac, dst=self.pg0.local_mac)
            / ip_l(src=flow_src, dst=dst)
            / UDP(sport=4242, dport=4242)
            / Raw(b"\xa5" * 100)
        )
        self.pg_enable_capture(self.pg_interfaces)
        self.pg0.add_stream(flow * NUM_PKTS)
        self.pg_start()

        f2 = self.pg2._get_capture()
        f3 = self.pg3._get_capture()
        n2 = 0 if f2 is None else len(f2)
        n3 = 0 if f3 is None else len(f3)
        # all of one flow on one interface, nothing on the other
        self.assertIn(
            (n2, n3),
            [(NUM_PKTS, 0), (0, NUM_PKTS)],
            "single flow not pinned to one ECMP bucket (pg2=%d pg3=%d)" % (n2, n3),
        )
        self.pg1.assert_nothing_captured(remark="ECMP flow, not dest path")

        self.disable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, is_ipv6)

    # ------------------------------------------------------------------ #
    # 5. API: sfr_dump reflects the bindings
    # ------------------------------------------------------------------ #
    def test_sfr_dump(self):
        """sfr_dump returns bindings after enable, empty after disable"""
        # version handshake
        ver = self.vapi.sfr_plugin_get_version()
        self.assertEqual(ver.major, 1)

        # nothing bound yet
        self.assertEqual(len(self.vapi.sfr_dump()), 0)

        # enable v4 on pg0 + v6 on pg1
        self.enable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, False)
        self.enable_sfr(self.pg1.sw_if_index, SRC_FIB_TABLE_ID, True)

        details = self.vapi.sfr_dump()
        self.assertEqual(len(details), 2)

        by_if = {(d.sw_if_index, d.is_ipv6): d for d in details}
        self.assertIn((self.pg0.sw_if_index, False), by_if)
        self.assertIn((self.pg1.sw_if_index, True), by_if)
        self.assertEqual(by_if[(self.pg0.sw_if_index, False)].table_id, SRC_FIB_TABLE_ID)
        self.assertEqual(by_if[(self.pg1.sw_if_index, True)].table_id, SRC_FIB_TABLE_ID)

        # disable both -> empty
        self.disable_sfr(self.pg0.sw_if_index, SRC_FIB_TABLE_ID, False)
        self.disable_sfr(self.pg1.sw_if_index, SRC_FIB_TABLE_ID, True)
        self.assertEqual(len(self.vapi.sfr_dump()), 0)

    # ------------------------------------------------------------------ #
    # 6. CONTROL-PLANE rv semantics
    # ------------------------------------------------------------------ #
    def test_sfr_control_plane_rv(self):
        """enable/disable rv semantics (in-use, idempotent, no-such-entry)"""
        self._control_plane_rv(is_ipv6=False)
        self._control_plane_rv(is_ipv6=True)

    def _control_plane_rv(self, is_ipv6):
        swif = self.pg0.sw_if_index

        # enable table 10 -> rv 0
        r = self.vapi.sfr_enable_disable(
            sw_if_index=swif,
            table_id=SRC_FIB_TABLE_ID,
            is_ipv6=is_ipv6,
            is_enable=True,
        )
        self.assertEqual(r.retval, 0)

        # enable a DIFFERENT table on same if/proto -> INSTANCE_IN_USE
        with self.vapi.assert_negative_api_retval():
            r = self.vapi.sfr_enable_disable(
                sw_if_index=swif,
                table_id=OTHER_FIB_TABLE_ID,
                is_ipv6=is_ipv6,
                is_enable=True,
            )
        self.assertEqual(r.retval, VNET_API_ERROR_INSTANCE_IN_USE)

        # re-enable the SAME table -> idempotent re-assert, rv 0
        r = self.vapi.sfr_enable_disable(
            sw_if_index=swif,
            table_id=SRC_FIB_TABLE_ID,
            is_ipv6=is_ipv6,
            is_enable=True,
        )
        self.assertEqual(r.retval, 0)

        # binding is still on table 10, still exactly one
        details = [
            d
            for d in self.vapi.sfr_dump()
            if d.sw_if_index == swif and d.is_ipv6 == is_ipv6
        ]
        self.assertEqual(len(details), 1)
        self.assertEqual(details[0].table_id, SRC_FIB_TABLE_ID)

        # disable -> rv 0
        r = self.vapi.sfr_enable_disable(
            sw_if_index=swif,
            table_id=SRC_FIB_TABLE_ID,
            is_ipv6=is_ipv6,
            is_enable=False,
        )
        self.assertEqual(r.retval, 0)

        # disable again (not bound) -> NO_SUCH_ENTRY
        with self.vapi.assert_negative_api_retval():
            r = self.vapi.sfr_enable_disable(
                sw_if_index=swif,
                table_id=SRC_FIB_TABLE_ID,
                is_ipv6=is_ipv6,
                is_enable=False,
            )
        self.assertEqual(r.retval, VNET_API_ERROR_NO_SUCH_ENTRY)


if __name__ == "__main__":
    unittest.main(testRunner=VppTestRunner)
