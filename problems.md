1. could not build qemu 
tomli isnt avaliable on python3.10 std (only avalibale from 3.11) - manuall installing using `pip install tomli` is required

2. Could not build the driver
Build the driver on the host or the guest VM?
on guest no make, on host buildroot cross compiler is not detected
Could only build on host after changing `CROSS_PREFIX := x86_64-buildroot-linux-gnu-` to `CROSS_PREFIX := x86_64-linux-`

3. Intelissense in vscode
Consider adding guide to installing the makefile and c/c++ extensions