#!/usr/bin/env python3
# Add a `skb-mode` flag to VPP's af_xdp create CLI.
#
# VPP's af_xdp never sets xsk_socket_config.xdp_flags, so libxdp attempts a
# native (driver) XDP attach. NICs without native XDP — Broadcom bnx2x and most
# SR-IOV VFs — reject it ("XDP mode not supported; try using SKB mode"). This
# adds an opt-in `skb-mode` create flag that sets XDP_FLAGS_SKB_MODE on both the
# xsk socket config (default-program path) and the custom-prog bpf_xdp_attach
# path, enabling generic/SKB-mode XDP so af_xdp works on any kernel driver.
#
# Applied at package-build time against the release tag source (which predates
# the upstream master af_xdp changes), so it targets that exact layout. Idempotent.
import sys

B = "src/plugins/af_xdp/"


def patch(path, marker, subs):
    s = open(path, encoding="utf-8").read()
    if marker in s:
        print(f"{path}: already patched ({marker})")
        return
    for old, new in subs:
        if old not in s:
            sys.exit(f"{path}: anchor not found:\n{old}")
        s = s.replace(old, new, 1)
    open(path, "w", encoding="utf-8", newline="\n").write(s)
    print(f"{path}: patched")


patch(
    B + "af_xdp.h",
    "AF_XDP_CREATE_FLAGS_SKB_MODE",
    [(
        "  AF_XDP_CREATE_FLAGS_NO_SYSCALL_LOCK = 1,\n} af_xdp_create_flag_t;",
        "  AF_XDP_CREATE_FLAGS_NO_SYSCALL_LOCK = 1,\n"
        "  AF_XDP_CREATE_FLAGS_SKB_MODE = 4,\n} af_xdp_create_flag_t;",
    )],
)

patch(
    B + "unformat.c",
    "skb-mode",
    [(
        '      else if (unformat (line_input, "no-syscall-lock"))\n'
        "\targs->flags |= AF_XDP_CREATE_FLAGS_NO_SYSCALL_LOCK;\n",
        '      else if (unformat (line_input, "no-syscall-lock"))\n'
        "\targs->flags |= AF_XDP_CREATE_FLAGS_NO_SYSCALL_LOCK;\n"
        '      else if (unformat (line_input, "skb-mode"))\n'
        "\targs->flags |= AF_XDP_CREATE_FLAGS_SKB_MODE;\n",
    )],
)

patch(
    B + "device.c",
    "AF_XDP_CREATE_FLAGS_SKB_MODE",
    [
        (
            "      sock_config.bind_flags |= XDP_ZEROCOPY;\n      break;\n    }\n  if (args->prog)",
            "      sock_config.bind_flags |= XDP_ZEROCOPY;\n      break;\n    }\n"
            "  if (args->flags & AF_XDP_CREATE_FLAGS_SKB_MODE)\n"
            "    sock_config.xdp_flags |= XDP_FLAGS_SKB_MODE;\n  if (args->prog)",
        ),
        (
            "  if (bpf_xdp_attach (ad->linux_ifindex, fd, XDP_FLAGS_UPDATE_IF_NOEXIST,\n\t\t      NULL))",
            "  if (bpf_xdp_attach (ad->linux_ifindex, fd,\n"
            "\t\t      XDP_FLAGS_UPDATE_IF_NOEXIST |\n"
            "\t\t\t(args->flags & AF_XDP_CREATE_FLAGS_SKB_MODE ? XDP_FLAGS_SKB_MODE :\n"
            "\t\t\t\t\t\t\t\t      0),\n"
            "\t\t      NULL))",
        ),
    ],
)

patch(
    B + "cli.c",
    "[skb-mode]",
    [(
        "[zero-copy|no-zero-copy] [no-syscall-lock]\",",
        "[zero-copy|no-zero-copy] [no-syscall-lock] [skb-mode]\",",
    )],
)

print("af_xdp skb-mode patch complete")
