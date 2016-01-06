#!/bin/sh

echo MCLURS Hardware Probe Vn 0.5

NCPU=`grep processor /proc/cpuinfo | wc -l`
UUIDPI=`grep serial /proc/cpuinfo | awk '{print $3;}'`

echo Detected a ${NCPU}-cpu Pi with serial number $UUIDPI

I2CBUS=''
for BUS in `i2cdetect -l | awk '{print substr($1,5); }'`; do
    echo -n Probing using I2C Bus $BUS ...
    if i2cdetect -y $BUS > /tmp/i2c-$BUS-detect; then
	if grep -q ' 18 ' /tmp/i2c-$BUS-detect && grep -q ' 41 ' /tmp/i2c-$BUS-detect; then
	    I2CBUS=$BUS
	    echo " accepted $BUS"
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

Version=0
echo DS2482  found on I2C Bus $I2CBUS
echo LTC2637 found on I2C Bus $I2CBUS

if grep -q ' 6f ' /tmp/i2c-$I2CBUS-detect && grep -q ' 57 ' /tmp/i2c-$I2CBUS-detect; then
    echo MCP79410 found on I2C Bus $I2CBUS
    Version=1
fi

if grep -q ' 50 ' /tmp/i2c-$I2CBUS-detect; then
    echo Found 24AA64 on I2C Bus $I2CBUS
fi

if grep -q ' 27 ' /tmp/i2c-$I2CBUS-detect || grep -q ' UU ' /tmp/i2c-$I2CBUS-detect; then
    echo Found PCA9534 on I2C Bus $I2CBUS
    Version=2
fi

echo I2C Probe complete, Hardware Version $Version
rm -f /tmp/i2c-$I2CBUS-detect

exit 0
