# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms under GPLv3 section 7: see LICENSE.
#
# What the off-device shell tests (tests/test_*.sh) share: each had its own
# copy. A test starts with
#     set -u
#     . "$(dirname "$0")/lib.sh"
# and ends with `finish`. For sh and dash alike. It gives:
#   here    the source tree
#   SRC     the H2's own files in it (buildroot's overlay), or the caller's SRC
#   T       a scratch folder made for this run (the test removes it when done)
#   R       the root a test makes there for the scripts it runs: $T/root
#   SLEEP   the real sleep, for a test that puts a stand-in first in PATH
#   check NAME 'CONDITION'    "ok   NAME" or "FAIL NAME", and counted
#   until_ 'CONDITION'        waits until it holds
#   count FILE                how many lines it has (0: there's none)
#   lines FILE                its lines on one, a space between them
#   rooted FROM TO [...]      the script FROM with its paths inside $R, as TO
#   finish                    the last line: the counts, and the exit status
here=$(cd "$(dirname "$0")/.." && pwd)
SRC=${SRC:-$here/buildroot/external/board/h2/rootfs-overlay}
T=$(mktemp -d)
R=$T/root
SLEEP=$(command -v sleep)

ok=0; bad=0
check() { if eval "$2"; then echo "ok   $1"; ok=$((ok+1)); else echo "FAIL $1"; bad=$((bad+1)); fi; }
count() { if [ -f "$1" ]; then echo $(($(wc -l < "$1") + 0)); else echo 0; fi; }
lines() { [ -f "$1" ] && tr '\n' ' ' < "$1" | sed 's/ $//'; }

# Looked at every 0.02 s, UNTIL_TRIES times at most (6 s; a test may set another)
UNTIL_TRIES=300
until_() { i=0; while ! eval "$1" && [ $i -lt $UNTIL_TRIES ]; do $SLEEP 0.02; i=$((i+1)); done; }

# /usr/..., /mnt/... and the rest of ROOTED (a test may add to it) become
# $R/usr/..., $R/mnt/...; /dev/null stays the machine's. Any more arguments
# are sed's, applied after those: FILE_FOR_DEVICE is one, for a script that
# asks whether the card is a block device (-b: nobody here may make one, a
# plain file stands for it).
ROOTED='usr|mnt|sys|tmp|dev|proc|var'
FILE_FOR_DEVICE='s#\[ -b #[ -e #g'
rooted() {
    rooted_from=$1; rooted_to=$2; shift 2
    sed -E -e "s#(^|[ =\"(<>])/($ROOTED)/#\\1$R/\\2/#g" -e "s#$R/dev/null#/dev/null#g" "$@" "$rooted_from" > "$rooted_to"
}

finish() { echo "$ok passed, $bad failed"; [ $bad -eq 0 ]; }
