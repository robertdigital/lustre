#! /bin/bash
# Utility script for cleaning up snapshots and removing modules.
OBDDIR="`dirname $0`/.."
[ "$OBDDIR" = "" ] && OBDDIR=".."
. $OBDDIR/demos/config.sh

plog umount $MNTOBD
plog umount $MNTSNAP

plog log "CLEANUP /dev/obd2 /dev/obd1"
$OBDDIR/class/obdcontrol -f << EOF
device /dev/obd2
cleanup
detach
device /dev/obd1
cleanup
detach
quit
EOF

rmmod obdsnap

$OBDDIR/demos/obdclean.sh
