#!/bin/bash
# Simulate two machines behind separate NATs on one box, with a mock tracker
# and STUN server on the "internet". Needs no root: everything lives in
# unprivileged user+network namespaces and vanishes when this script exits.
#
#   tools/netsim/netsim.sh [-g 1|2] [-n cone|symmetric|none] [-d MS] [-l PCT]
#                          [-f] [-b BINARY]
#
#   -n  NAT type on both sides (default cone = port-restricted). symmetric
#       randomizes the port per destination; none just routes.
#   -d  one-way delay in ms added on each peer's uplink; -l loss percent.
#   -f  forward the game UDP port to the host's machine (control: should
#       work even when hole punching doesn't).
#
# Starts game windows for "host" (10.0.1.2 behind NAT 198.51.100.2) and
# "client" (10.0.2.2 behind NAT 203.0.113.2). Tracker is 192.0.2.10:42420.
# Logs: $SIM/{tracker,host,client}.log, per-machine HOME under $SIM/.
set -eu

GAME=1 NAT=cone DELAY=0 LOSS=0 FORWARD=0 BIN=
while getopts g:n:d:l:fb: o; do
	case $o in
	g) GAME=$OPTARG ;; n) NAT=$OPTARG ;; d) DELAY=$OPTARG ;; l) LOSS=$OPTARG ;;
	f) FORWARD=1 ;; b) BIN=$OPTARG ;;
	*) sed -n '2,/^set -eu/p' "$0" | sed '$d'; exit 2 ;;
	esac
done

ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[ -n "$BIN" ] || BIN=$ROOT/d$GAME/out/build/linux-debug-local/main/d${GAME}x-redux-sng
DATA=$HOME/.d${GAME}x-redux/data
UDP_PORT=42424
TRACKER_IP=192.0.2.10

if [ -z "${NETSIM_INNER:-}" ]; then
	[ -x "$BIN" ] || { echo "no binary at $BIN (build first, or pass -b)"; exit 1; }
	export NETSIM_INNER=1 SIM=$(mktemp -d "${TMPDIR:-/tmp}/netsim.XXXXXX")
	export GAME NAT DELAY LOSS FORWARD BIN DATA ROOT UDP_PORT TRACKER_IP
	echo "sandbox dir: $SIM"
	exec unshare -Urn --kill-child "$0" "$@"
fi

ns_spawn() { unshare -n sleep infinity >/dev/null 2>&1 & echo $!; }
in_ns()    { local pid=$1; shift; nsenter -t "$pid" -n -- "$@"; }

ip link set lo up
ip addr add $TRACKER_IP/32 dev lo
sysctl -qw net.ipv4.ip_forward=1

NAT_A=$(ns_spawn) NAT_B=$(ns_spawn) HOST_A=$(ns_spawn) HOST_B=$(ns_spawn)
trap 'kill $NAT_A $NAT_B $HOST_A $HOST_B $(jobs -p) 2>/dev/null || true' EXIT
sleep 0.3

# wire <wan-side-pid> <wan-ip> <lan-pid> <lan-prefix> <tag>
wire() {
	local nat=$1 wan_ip=$2 host=$3 lan=$4 tag=$5
	ip link add "w$tag" type veth peer name "u$tag"
	ip link set "u$tag" netns "$nat"
	ip addr add "$wan_ip.1/24" dev "w$tag"; ip link set "w$tag" up
	ip link add "l$tag" netns "$nat" type veth peer name "h$tag" netns "$host"
	in_ns "$nat" sh -c "
		ip link set lo up
		ip addr add $wan_ip.2/24 dev u$tag; ip link set u$tag up
		ip addr add $lan.1/24 dev l$tag;    ip link set l$tag up
		ip route add default via $wan_ip.1
		sysctl -qw net.ipv4.ip_forward=1"
	in_ns "$host" sh -c "
		ip link set lo up
		ip addr add $lan.2/24 dev h$tag; ip link set h$tag up
		ip route add default via $lan.1"
	[ "$DELAY$LOSS" = 00 ] || tc qdisc add dev "w$tag" root netem delay "${DELAY}ms" loss "${LOSS}%"
}
wire "$NAT_A" 198.51.100 "$HOST_A" 10.0.1 a
wire "$NAT_B" 203.0.113  "$HOST_B" 10.0.2 b

nat_rules() { # <pid> <wan-if> <lan-if> <host-ip>
	[ "$NAT" = none ] && return
	local masq=masquerade; [ "$NAT" = symmetric ] && masq="masquerade random"
	in_ns "$1" sh -c "
		nft add table ip nat
		nft add chain ip nat post '{ type nat hook postrouting priority 100; }'
		nft add rule ip nat post oifname $2 $masq"
	[ "$FORWARD" = 1 ] && [ "$1" = "$NAT_A" ] && in_ns "$1" sh -c "
		nft add chain ip nat pre '{ type nat hook prerouting priority -100; }'
		nft add rule ip nat pre iifname $2 udp dport $UDP_PORT dnat to $4"
	return 0
}
nat_rules "$NAT_A" ua la 10.0.1.2
nat_rules "$NAT_B" ub lb 10.0.2.2
if [ "$NAT" = none ]; then
	ip route add 10.0.1.0/24 via 198.51.100.2
	ip route add 10.0.2.0/24 via 203.0.113.2
fi

python3 -I "$ROOT/tools/netsim/mock_tracker.py" $TRACKER_IP >"$SIM/tracker.log" 2>&1 &
sleep 0.5

launch() { # <name> <pid> <pilot>
	local name=$1 pid=$2 pilot=$3 home=$SIM/$1
	mkdir -p "$home/.d${GAME}x-redux"
	ln -sfn "$DATA" "$home/.d${GAME}x-redux/data"
	in_ns "$pid" env HOME="$home" DXX_STUN_SERVERS=$TRACKER_IP:3478 \
		"$BIN" -hogdir "$DATA" -pilot "$pilot" \
		-tracker_hostaddr $TRACKER_IP -tracker_hostport 42420 \
		>"$SIM/$name.log" 2>&1 &
}
launch host   "$HOST_A" SimHost
launch client "$HOST_B" SimClient

cat <<MSG
host   : 10.0.1.2 behind NAT 198.51.100.2   (log: $SIM/host.log)
client : 10.0.2.2 behind NAT 203.0.113.2    (log: $SIM/client.log)
NAT=$NAT delay=${DELAY}ms loss=${LOSS}% forward=$FORWARD
Host a netgame in the SimHost window, then Join (tracker list) in SimClient.
Tracker traffic: tail -f $SIM/tracker.log      Ctrl-C to tear down.
MSG
wait
