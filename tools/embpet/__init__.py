"""Host-side library for the embpet board: serial open without reset, @@ frame parser,
console quoting, Board.cmd() / Board.events(). Used by tools/embpet_cli.py and test/hil."""
from .board import Board, CmdTimeout, NoBoard, new_rid
from .frames import FRAME, FrameChecker, build_line, parse_frame, quote, split_command
from .serialport import find_port, list_ports, open_port, reset_chip

__all__ = ["Board", "CmdTimeout", "NoBoard", "new_rid", "FRAME", "FrameChecker",
           "build_line", "parse_frame", "quote", "split_command", "find_port",
           "list_ports", "open_port", "reset_chip"]
