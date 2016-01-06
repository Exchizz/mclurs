#!/bin/sh

echo MCLURS Hardware Probe Vn 0.8

# Step 1:  Find out what kind of processor we are running on

ARCH=`uname --machine`
NCPU=`grep processor /proc/cpuinfo | wc -l`
UUIDPI=`grep Serial /proc/cpuinfo | awk '{print $3;}'`

echo Detected a ${NCPU}-cpu $ARCH with serial number $UUIDPI

# Step 2:  Collect the network UUID (MAC address of eth0)

ETH0MAC=`ip link show eth0 | tail -1l | cut '-d ' -f 6 | tr -d ':'`
echo MAC of eth0 is $ETH0MAC

# Step 3:  Look for the USBDUXFAST hardware

USBDUXMODULE=/lib/modules/`uname --kernel-release`/kernel/drivers/staging/comedi/drivers/usbduxfast.ko
USBDUXFAST=''
COMEDIDEV=''
if dmesg | grep -q 'idVendor=13d8' > /dev/null 2>&1; then
    USBDUXFAST=1
    echo -n Found USBDUXFAST hardware ""
    if [ -r "$USBDUXMODULE" ]; then
	modprobe usbduxfast
	if [ $? = 0 ]; then
	    COMEDIDEV=`dmesg | grep 'comedi' | grep 'usbduxfast' | awk '{ print substr($4,7,1);}'`
	    echo attached to /dev/comedi$COMEDIDEV
	fi
    else
	echo but no driver module: check your kernel
    fi
else
    echo No USBDUXFAST hardware detected
fi

###
### Up to here we can do on any machine running in a MCLURS array
###

# From here, though, we assume we are using a Pi since we want to probe the analogue board...

if [ "x$ARCH" = "xarmv7l" ]; then
    echo "ARM Architecture, assuming RPi"

# Step 4:  Find out which I2C bus has the analogue board attached
#
# Look for the DS2482 and LTC2637, present on all board versions

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
    DS2482=1
    echo "DS2482   found" on I2C Bus $I2CBUS
    LTC2637=1
    echo "LTC2637  found" on I2C Bus $I2CBUS

# Step 5:  Got a Bus;  look for the other chips and figure out board version

    if grep -q ' 6f ' /tmp/i2c-$I2CBUS-detect && grep -q ' 57 ' /tmp/i2c-$I2CBUS-detect; then
	echo "MCP79410 found" on I2C Bus $I2CBUS
	Version=1
	MCP79410=1
    fi

    if grep -q ' 50 ' /tmp/i2c-$I2CBUS-detect; then
	echo "24AA64   found" on I2C Bus $I2CBUS
	EEPROM64=1
    fi

    if grep -q ' 27 ' /tmp/i2c-$I2CBUS-detect || grep -q ' UU ' /tmp/i2c-$I2CBUS-detect; then
	echo "PCA9534  found" on I2C Bus $I2CBUS
	Version=2
	PCA9534=1
    fi

    echo I2C Probe complete, Hardware Version $Version
    rm -f /tmp/i2c-$I2CBUS-detect

# Step 6:  Collect the analogue board UUID from the MCP79410 if present

    UUIDBOARD=`i2cdump -y 1 0x57 b | fgrep 'f0:' | awk '{ print $2$3$4$5$6$7$8$9; }'`
    if [ "x$UUIDBOARD" = "xffffffffffffffff" ]; then
	echo "MCLURS Hardware UUID unset"
	UUIDBOARD=''
    else
	echo UUID of analogue board is $UUIDBOARD
    fi

fi

###
###  We have now collected all the probe data we need to set up the /etc/mclurs directory
###


exit 0
