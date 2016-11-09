function reply = start_sampling(expname, duration)
    rpi2 = udp('169.254.255.2', 2469);
    fopen(rpi2);
    fprintf(rpi2, '%s\n', [expname, ' ', num2str(duration)]')
end

