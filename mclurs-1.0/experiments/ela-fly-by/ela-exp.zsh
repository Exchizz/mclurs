#!/bin/zsh

read expname duration

sudo grab | 
sox -c8 -r312500 -t s16 - -t wavpcm /home/pi/ela-data/$expname.wav trim 0 $duration
