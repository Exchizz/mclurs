#!/bin/sh

echo MCLURS Hardware Probe Vn 0.5

NCPU=`grep processor /proc/cpuinfo | wc -l`
UUIDPI=`grep serial /proc/cpuinfo | awk '{print $3;}'`

echo Detected a ${NCPU}-cpu Pi with serial number $UUIDPI

I2CBUS=''
for BUS in `i2cdetect -l | awk '{print substr($1,5); }'`; do
    echo -n Probing using I2C Bus $BUS ...
    if i2cdetect -y $BUS > /tmp/i2c-$BUS-detect; then
	if grep -q 18 /tmp/i2c-$BUS-detect && grep -q 41 /tmp/i2c-$BUS-detect; then
	    I2CBUS=$BUS
	    break
	fi
    else
	echo " failed"
	rm -f /tmp/i2c-$BUS-detect
    fi
done

if [ "x$I2CBUS" = "x" ]; then
    echo Unable to determine I2C Bus to talk to hardware
    exit 1
fi



exit 0
