#!/usr/bin/env python3
"""Opt-in Linux receive-bandwidth test. See docs/network-link-testing.md."""

import argparse
import ipaddress
import json
import os
import shlex
import shutil
import subprocess
import sys


IFB = "ml-link-test"
OWNER = "moonlight-link-test:"


def run(*args):
    return subprocess.run(args, check=True, text=True, stdout=subprocess.PIPE).stdout


def links():
    return json.loads(run("ip", "-j", "link", "show"))


def owned_link():
    link = next((item for item in links() if item["ifname"] == IFB), None)
    if link and not link.get("ifalias", "").startswith(OWNER):
        raise RuntimeError(f"{IFB} already exists and is not owned by this helper")
    return link


def require_root():
    if os.geteuid() != 0:
        raise RuntimeError("start/stop require sudo (CAP_NET_ADMIN)")


def start(args):
    if not args.interface:
        raise RuntimeError("start requires --interface")
    address = ipaddress.ip_address(args.host) if args.host else None
    protocols = ["ip", "ipv6"] if address is None else ["ip" if address.version == 4 else "ipv6"]
    if not 1 <= args.rate_mbps <= 10000 or not 1 <= args.queue_ms <= 100:
        raise RuntimeError("rate must be 1..10000 Mbps; queue must be 1..100 ms")
    # A finite FIFO introduces backlog and drops when the source outruns it.
    limit = args.rate_mbps * 125 * args.queue_ms
    burst = min(65536, max(4096, args.rate_mbps * 125 // 2))
    commands = [
        ("ip", "link", "add", "name", IFB, "type", "ifb"),
        ("ip", "link", "set", "dev", IFB, "alias", OWNER + args.interface),
        ("ip", "link", "set", "dev", IFB, "up"),
        ("tc", "qdisc", "add", "dev", IFB, "root", "handle", "1:", "tbf",
         "rate", f"{args.rate_mbps}mbit", "burst", str(burst), "limit", str(limit)),
        ("tc", "qdisc", "add", "dev", args.interface, "handle", "ffff:", "ingress"),
    ]
    for offset, protocol in enumerate(protocols):
        match = ("src_ip", str(address), "ip_proto", "udp") if address else ()
        commands.append(("tc", "filter", "add", "dev", args.interface, "parent", "ffff:",
                         "protocol", protocol, "pref", str(49152 + offset), "handle", "1", "flower", "skip_hw",
                         *match, "action", "mirred", "egress", "redirect", "dev", IFB))
    if args.dry_run:
        for command in commands:
            print(shlex.join(command))
        return
    require_root()
    if owned_link():
        raise RuntimeError("A test is already active; stop it before starting another")
    if not any(item["ifname"] == args.interface for item in links()):
        raise RuntimeError(f"No interface named {args.interface}")
    qdiscs = json.loads(run("tc", "-j", "qdisc", "show", "dev", args.interface))
    if any(item["kind"] in ("ingress", "clsact") for item in qdiscs):
        raise RuntimeError("Interface already has ingress/clsact rules; refusing to replace them")
    created_link = created_ingress = False
    try:
        for index, command in enumerate(commands):
            run(*command)
            if index == 0:
                created_link = True
            if index == 4:
                created_ingress = True
    except BaseException:
        # Remove the redirection before deleting its target. Keep the IFB if
        # removing the ingress hook fails, so stop can retry the cleanup.
        if created_ingress:
            run("tc", "qdisc", "del", "dev", args.interface, "ingress")
        if created_link:
            run("ip", "link", "del", "dev", IFB)
        raise
    scope = f"UDP from {address}" if address else "IPv4/IPv6 traffic"
    print(f"Limiting incoming {scope} on {args.interface} to "
          f"{args.rate_mbps} Mbps; FIFO {limit} bytes ({args.queue_ms} ms of service).")
    print("Use 'status' for backlog/drop counters and 'sudo ... stop' to remove the test.")


def stop():
    require_root()
    link = owned_link()
    if not link:
        print("No active test.")
        return
    interface = link["ifalias"][len(OWNER):]
    if any(item["ifname"] == interface for item in links()):
        filters = json.loads(run("tc", "-j", "filter", "show", "dev", interface,
                                 "parent", "ffff:"))
        # Never remove rules installed by somebody else after our start.
        for item in filters:
            # tc emits a classifier header as well as each concrete filter.
            if "options" not in item:
                continue
            actions = item.get("options", {}).get("actions", [])
            if (item.get("pref") not in (49152, 49153) or item.get("kind") != "flower"
                    or not any(action.get("to_dev") == IFB for action in actions)):
                raise RuntimeError("Ingress rules changed; refusing automatic cleanup")
        qdiscs = json.loads(run("tc", "-j", "qdisc", "show", "dev", interface))
        hook = next((item for item in qdiscs if item["kind"] in ("ingress", "clsact")), None)
        if hook:
            if hook["kind"] != "ingress" or hook.get("handle") != "ffff:":
                raise RuntimeError("Ingress hook changed; refusing automatic cleanup")
            run("tc", "qdisc", "del", "dev", interface, "ingress")
    run("ip", "link", "del", "dev", IFB)
    print("Test removed.")


def status():
    link = owned_link()
    if not link:
        print("No active test.")
        return
    interface = link["ifalias"][len(OWNER):]
    print(f"Incoming test on {interface}:")
    print(run("tc", "-s", "qdisc", "show", "dev", IFB), end="")
    print(run("tc", "-s", "filter", "show", "dev", interface,
              "parent", "ffff:"), end="")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    begin = sub.add_parser("start", help="limit incoming IP traffic; requires sudo")
    begin.add_argument("--interface", help="interface receiving the stream")
    begin.add_argument("--host", help="optional: restrict the limit to UDP from this host IP")
    begin.add_argument("--rate-mbps", type=int, default=1000)
    begin.add_argument("--queue-ms", type=int, default=2,
                       help="FIFO size in milliseconds of service (default: 2)")
    begin.add_argument("--dry-run", action="store_true", help="print commands without applying them")
    sub.add_parser("stop", help="remove this helper's test; requires sudo")
    sub.add_parser("status", help="show rate, backlog and drop counters")
    args = parser.parse_args()
    try:
        if not (args.action == "start" and args.dry_run):
            for command in ("ip", "tc"):
                if not shutil.which(command):
                    raise RuntimeError(f"Missing {command}; install iproute2 first")
        if args.action == "start":
            start(args)
        elif args.action == "stop":
            stop()
        else:
            status()
    except (RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
