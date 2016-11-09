function stop_sampling(amp)
    rpi2 = udp('169.254.255.2', 2470);
    fopen(rpi2);
    fprintf(rpi2, '\n')
end