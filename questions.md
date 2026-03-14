1. No devm_ api usage? why? - Probably cause of the cfg where we can reallocate the buffers
2. part 1 - what is allocation messages in dmesg?
3. `pfpga_start_streaming` guide tells us to set the desc_head to desc_tail which is 0 - shouldnt the ring buffer head and tail should be far away when it is empty? or maybe empty means the head and tail are the same?
4. How does the device knows the next descriptor if we set the next address to zero? - Maybe it uses the pointer of desc_ring and run over it untill head  