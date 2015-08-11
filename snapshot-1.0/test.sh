#!/bin/sh

rm -rf /var/tmp/snap/*
sudo chmod 777 snapshot-CMD
./snapchat init
./snapchat go
#./snapchat snap start=0 length=8388608 count=256 path=test
./snapchat snap start=0 length=2097152 count=1024 path=test
