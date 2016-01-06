#!/bin/sh

echo MCLURS Hardware Probe Vn 0.5

NCPU=`grep processor /proc/cpuinfo | wc -l`
UUIDPI=`grep serial /proc/cpuinfo | awk '{print $3;}'

echo Detected a ${NCPU}cpu Pi with serial number $UUIDPI

exit 0
