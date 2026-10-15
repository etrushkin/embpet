"""Console line building (README 3.2) and @@ frame parsing (README 3.3)."""
import json

FRAME = "@@"
GROUPS = ("tg", "wifi", "io", "evt", "sys")


# Mirrors CONFIG_EMBPET_CMDLINE_MAX (main/Kconfig.projbuild); update both together.
# Bytes of the line without the newline.
CMDLINE_MAX = 4400


def quote(arg):
    """Quote one argument for the embpet console (README 3.2); newlines become \\n.

    Other control characters raise ValueError: the line reader ends the line at CR, erases
    at BS/DEL and drops the rest, so they would corrupt or split the command. TAB is fine
    inside quotes."""
    for ch in arg:
        if (ch < " " and ch not in "\n\t") or ch == "\x7f":
            raise ValueError("control character %r cannot be sent in a console argument" % ch)
    esc = arg.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")
    return '"%s"' % esc


def split_command(tokens):
    """Split argv-style tokens into (command words, remaining args).

    The first two tokens are the command when the first is a command group ("wifi set"),
    else only the first is."""
    tokens = list(tokens)
    n = 2 if len(tokens) >= 2 and tokens[0] in GROUPS else 1
    return tokens[:n], tokens[n:]


def build_line(words, args=(), text=(), rid=None, actor=None):
    """Console line: WORDS [-i RID] [-a ACTOR] "ARG"... [-- "TEXT"...].

    -i and -a go right after the command words (README 3.2). ARGs are quoted but are
    still parsed by argtable, so options such as "--older" work. TEXT words come after a
    bare `--`, so they are positional even when they start with '-'."""
    parts = list(words)
    if rid:
        parts += ["-i", rid]
    if actor:
        parts += ["-a", actor]
    parts += [quote(x) for x in args]
    if text:
        parts.append("--")
        parts += [quote(x) for x in text]
    line = " ".join(parts)
    n = len(line.encode("utf-8"))
    if n > CMDLINE_MAX:
        raise ValueError("command line is %d bytes, the firmware accepts at most %d"
                         % (n, CMDLINE_MAX))
    return line


def parse_frame(line):
    """Parse one console line as a frame: only lines starting with "@@{" count (the echoed
    command or log text may contain "@@{" further in). Returns the dict or None."""
    if not line.startswith(FRAME + "{"):
        return None
    try:
        f = json.loads(line[len(FRAME):])
    except ValueError:
        return None
    return f if isinstance(f, dict) else None


def _frame_obj(s):
    """Parse s as a JSON object holding t and seq, else None."""
    try:
        f = json.loads(s)
    except ValueError:
        return None
    if isinstance(f, dict) and "t" in f and "seq" in f:
        return f
    return None


class FrameChecker:
    """Line-by-line checker for --check-frames.

    A line starting with '@@' must be a frame (JSON object with int seq and a boot id).
    An '@@' later in a line is bad only if the rest parses as a frame (a real frame glued
    onto another line); otherwise it is log text and only counted in embedded_text.
    seq must be strictly increasing per boot id; any gap is counted and fails the check.
    Meant for plain streams: an `evt since` replay re-emits old seqs and would be
    reported as bad.
    """

    def __init__(self):
        self.frames = self.bad = self.gaps = self.embedded_text = 0
        self.problems = []
        self.last = {}          # boot id -> last seq

    def _bad(self, msg, text):
        self.bad += 1
        self.problems.append("%s: %r" % (msg, text[:120]))

    def feed(self, text):
        if "@@" not in text:
            return
        if not text.startswith("@@"):
            idx = text.find("@@")
            while idx >= 0:
                if _frame_obj(text[idx + 2:]) is not None:
                    self._bad("frame glued onto another line", text)
                    return
                idx = text.find("@@", idx + 2)
            self.embedded_text += 1
            return
        f = _frame_obj(text[2:])
        if f is None or type(f["seq"]) is not int or "boot" not in f:
            self._bad("bad frame", text)
            return
        self.frames += 1
        seq, boot = f["seq"], f["boot"]
        prev = self.last.get(boot)
        if prev is not None:
            if seq <= prev:
                self._bad("seq %d after %d (boot %s)" % (seq, prev, boot), text)
            elif seq > prev + 1:
                self.gaps += 1
        self.last[boot] = seq if prev is None else max(seq, prev)

    def ok(self):
        return self.bad == 0 and self.gaps == 0 and self.frames > 0

    def summary(self):
        return {"frames": self.frames, "bad": self.bad, "gaps": self.gaps,
                "embedded_text": self.embedded_text, "boots": len(self.last),
                "last_seq": self.last}
