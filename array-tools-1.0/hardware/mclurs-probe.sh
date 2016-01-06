#!/bin/sh

echo MCLURS Hardware Probe Vn 0.5

NCPU=`grep processor /proc/cpuinfo | wc -l`
UUIDPI=`grep serial /proc/cpuinfo | awk '{print $3;}'`

echo Detected a ${NCPU}-cpu Pi with serial number $UUIDPI

I2CBUS=`i2cdetect -l | awk '{print substr($1,4); }'`

echo Using I2C Bus $I2CBUS ...

exit 0
