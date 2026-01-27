# ~/.bashrc for PhantomFPGA Training Environment

# Source global definitions
if [ -f /etc/profile ]; then
    . /etc/profile
fi

# History settings
export HISTSIZE=1000
export HISTFILESIZE=2000
export HISTCONTROL=ignoredups:erasedups

# Shell options
shopt -s histappend 2>/dev/null
shopt -s checkwinsize 2>/dev/null

# Color prompt
if [ "$(id -u)" = "0" ]; then
    PS1='\[\033[01;31m\]phantomfpga\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]# '
else
    PS1='\[\033[01;32m\]phantomfpga\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]$ '
fi

# Enable color support
alias ls='ls --color=auto'
alias grep='grep --color=auto'
alias dmesg='dmesg --color=auto'

# Common aliases
alias ll='ls -la'
alias la='ls -A'
alias l='ls -CF'
alias ..='cd ..'
alias ...='cd ../..'

# Development aliases
alias kmsg='dmesg -w'           # Watch kernel messages
alias kclr='dmesg -C'           # Clear kernel ring buffer
alias mods='lsmod'              # List modules
alias pci='lspci -vvv'          # Verbose PCI listing
alias hex='xxd'                 # Hex dump

# Memory/register viewing helpers
alias devmem='busybox devmem'

# Quick directory jumps
alias cdd='cd /mnt/driver'      # Go to driver source
alias cda='cd /mnt/app'         # Go to app source

# If the kernel module exists, show a reminder
if [ -f "${PHANTOMFPGA_DRIVER_DIR}/phantom_fpga.ko" ]; then
    echo "Driver module ready at: ${PHANTOMFPGA_DRIVER_DIR}/phantom_fpga.ko"
    echo "Use 'pf-load' to load it, or 'pf-test' to rebuild and test"
fi
