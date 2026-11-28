#!/usr/bin/env python3
"""Talk to units connected to the remote console relay (relay.py), on the relay's machine.

  rscon.py list                          connected units
  rscon.py run <unit> <secs> <command>   send a console command, print what comes back for secs
  rscon.py send <unit> <command>         send without waiting
  rscon.py follow <unit>                 print the console live (Ctrl+C to stop)
  rscon.py recent <unit> [n]             the last n lines (default 100)

<unit> is a name (TDP-0E34), a name prefix, or the start of the address. From the PC:
  ssh debian-build python3 rscon/rscon.py run TDP-0E34 3 g
"""
import os
import socket
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    sock = os.path.expanduser("~/rscon/ctl.sock")
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock)
    s.sendall((" ".join(sys.argv[1:]) + "\n").encode())
    f = s.makefile("rb")
    try:
        for raw in f:
            sys.stdout.write(raw.decode("utf-8", "replace"))
            sys.stdout.flush()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
