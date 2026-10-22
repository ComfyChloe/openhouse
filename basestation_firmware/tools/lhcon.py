#!/usr/bin/env python3
"""Send commands to the lhbs USB console and print the replies.
usage: lhcon.py [-d /dev/ttyACM1] "cmd1" ["cmd2" ...]"""
import os, sys, termios, time, select

def open_port(dev):
    fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0                      # raw
    a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    a[4] = a[5] = termios.B115200
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd

def read_until_prompt(fd, timeout=3.0):
    buf, end = b'', time.time() + timeout
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.1)
        if r:
            buf += os.read(fd, 4096)
            if buf.rstrip().endswith(b'lhbs>'):
                break
    return buf.decode('latin1')

def main(argv):
    dev = '/dev/lhbs'
    if len(argv) > 2 and argv[1] == '-d':
        dev, argv = argv[2], argv[2:]
    fd = open_port(dev)
    os.write(fd, b'\r')
    read_until_prompt(fd, 1.5)
    for cmd in argv[1:]:
        os.write(fd, cmd.encode() + b'\r')
        out = read_until_prompt(fd)
        print(out.replace('\r', '').rstrip().removesuffix('lhbs>').rstrip())
    os.close(fd)

if __name__ == '__main__':
    main(sys.argv)
