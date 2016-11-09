function setgain(amp)
    rpi2 = udp('169.254.255.2', 2468);
    fopen(rpi2);
    fprintf(rpi2, 'scan')
    fprintf(rpi2, ['gain all ', num2str(amp), ' hp'])
end