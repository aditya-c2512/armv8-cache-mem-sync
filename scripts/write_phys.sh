#!/usr/bin/env bash
# Write a single byte (0x55) to the physical address backing a process virtual
# address. Usage: scripts/write_phys.sh <PID> <VADDR> [--yes]

set -euo pipefail

if [ "$#" -lt 2 ]; then
    cat <<EOF
Usage: $0 <PID> <VADDR> [--yes]

Example:
  $0 1234 0x7ffdeadbeef --yes

This script reads /proc/<PID>/pagemap to compute the physical address for
the provided virtual address, then writes a single byte 0x55 to /dev/mem at
that physical address. Running this requires root and may be dangerous.
EOF
    exit 1
fi

PID=$1
VADDR=$2
FORCE=no
if [ "${3-}" = "--yes" ]; then
    FORCE=yes
fi

if ! ps -p "$PID" > /dev/null 2>&1; then
    echo "error: PID $PID not found" >&2
    exit 2
fi

# normalize VADDR (accept hex like 0x7fff...) - bash handles 0x in arithmetic
VADDR_DEC=$((VADDR))
PAGE=$(getconf PAGESIZE)
VPN=$(( VADDR_DEC / PAGE ))

# read 8-byte pagemap entry
ENTRY=$(sudo dd if="/proc/$PID/pagemap" bs=8 skip=$VPN count=1 2>/dev/null | od -An -t u8 | awk '{print $1}')
if [ -z "$ENTRY" ]; then
    echo "error: failed to read pagemap for pid $PID" >&2
    exit 3
fi

# check present bit (bit 63)
if [ $(( ENTRY & (1<<63) )) -eq 0 ]; then
    echo "error: page not present in memory" >&2
    exit 4
fi

MASK=$(( (1<<55) - 1 ))
PFN=$(( ENTRY & MASK ))
PHYS=$(( PFN * PAGE + (VADDR_DEC % PAGE) ))

echo "PID=$PID VADDR=$VADDR (dec $VADDR_DEC) PFN=$PFN PHYS=0x$(printf '%x' $PHYS)"

if [ "$FORCE" != "yes" ]; then
    read -p "About to write 0x55 to /dev/mem at 0x$(printf '%x' $PHYS). Continue? [y/N] " yn
    case "$yn" in
        [Yy]*) ;;
        *) echo "aborted"; exit 0 ;;
    esac
fi

TMP=/tmp/write_phys_byte_$$
printf '\x55' > "$TMP"
sudo dd if="$TMP" of=/dev/mem bs=1 seek=$PHYS conv=notrunc status=none || {
    echo "error: dd failed" >&2
    rm -f "$TMP"
    exit 5
}
sync
rm -f "$TMP"
echo "wrote 0x55 to physical 0x$(printf '%x' $PHYS)"
