"""Tiny interpreter for the subset of CMake used by ZMK's app/CMakeLists.txt and
this repo's shield CMakeLists.txt files, so the simulator compiles exactly the
source files (and in the same link order) that the real firmware build would
for a given autoconf.h.

Supported: if/elseif/else/endif with AND/OR/NOT/parentheses on CONFIG_* (and
literal) operands, target_sources(app PRIVATE ...), target_sources_ifdef,
zephyr_library_sources(_ifdef), add_subdirectory(_ifdef).  Everything else is
ignored.  Sources are returned as (kind, abs_path) with kind "app" or "zephyr"
(the Zephyr library the file would end up in, which determines link order).
"""
import os
import re

_CMD = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.S)


def _tokenize_args(s):
    return re.findall(r'"[^"]*"|\(|\)|[^\s()]+', s)


def _commands(text):
    text = re.sub(r"#[^\n]*", "", text)
    i = 0
    while True:
        m = _CMD.search(text, i)
        if not m:
            return
        depth, j = 1, m.end()
        while depth:
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
            j += 1
        yield m.group(1).lower(), text[m.end():j - 1]
        i = j


class _Cond:
    def __init__(self, cfg):
        self.cfg = cfg

    def truthy(self, tok):
        if tok.startswith("CONFIG_"):
            v = self.cfg.get(tok)
            return v not in (None, "", "0", "n", "N")
        return tok.upper() not in ("", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND")

    def eval(self, toks):
        self.t, self.i = toks, 0
        return self._or()

    def _peek(self):
        return self.t[self.i] if self.i < len(self.t) else None

    def _or(self):
        v = self._and()
        while self._peek() == "OR":
            self.i += 1
            v = self._and() or v
        return v

    def _and(self):
        v = self._not()
        while self._peek() == "AND":
            self.i += 1
            v = self._not() and v
        return v

    def _not(self):
        if self._peek() == "NOT":
            self.i += 1
            return not self._not()
        return self._atom()

    def _atom(self):
        tok = self._peek()
        self.i += 1
        if tok == "(":
            v = self._or()
            self.i += 1  # ')'
            return v
        return self.truthy(tok)


def collect(cmake_file, cfg, out=None, base_kind="zephyr", variables=None):
    """Return list of (kind, path) in declaration order."""
    out = [] if out is None else out
    variables = {} if variables is None else variables
    d = os.path.dirname(os.path.abspath(cmake_file))
    cond = _Cond(cfg)
    stack = []  # entries: [active_now, any_taken, parent_active]

    def active():
        return all(s[0] for s in stack)

    def expand(tok):
        return re.sub(r"\$\{(\w+)\}", lambda m: variables.get(m.group(1), m.group(0)), tok)

    for cmd, args in _commands(open(cmake_file, encoding="utf-8").read()):
        a = [expand(t) for t in _tokenize_args(args)]
        if cmd == "set" and len(a) >= 2 and active():
            variables[a[0]] = a[1]
            continue
        if cmd == "if":
            par = active()
            v = par and cond.eval(a)
            stack.append([v, v, par])
            continue
        if cmd == "elseif":
            s = stack[-1]
            v = s[2] and not s[1] and cond.eval(a)
            s[0] = v
            s[1] = s[1] or v
            continue
        if cmd == "else":
            s = stack[-1]
            s[0] = s[2] and not s[1]
            continue
        if cmd == "endif":
            stack.pop()
            continue
        if not active():
            continue
        if cmd == "target_sources" and a and a[0] == "app":
            out += [("app", os.path.join(d, f)) for f in a[2:] if f.endswith(".c")]
        elif cmd == "target_sources_ifdef" and len(a) > 3 and a[1] == "app":
            if cond.truthy(a[0]):
                out += [("app", os.path.join(d, f)) for f in a[3:] if f.endswith(".c")]
        elif cmd == "zephyr_library_sources":
            out += [(base_kind, os.path.join(d, f)) for f in a if f.endswith(".c")]
        elif cmd == "zephyr_library_sources_ifdef":
            if cond.truthy(a[0]):
                out += [(base_kind, os.path.join(d, f)) for f in a[1:] if f.endswith(".c")]
        elif cmd in ("add_subdirectory", "add_subdirectory_ifdef"):
            if cmd == "add_subdirectory_ifdef":
                if not cond.truthy(a[0]):
                    continue
                a = a[1:]
            sub = os.path.join(d, a[0])
            f = os.path.join(sub, "CMakeLists.txt")
            if os.path.exists(f):
                collect(f, cfg, out, base_kind, variables)
    return out


def parse_autoconf(path):
    cfg = {}
    for line in open(path, encoding="utf-8"):
        m = re.match(r"#define (CONFIG_\w+) (.*)", line)
        if m:
            cfg[m.group(1)] = m.group(2).strip().strip('"')
    return cfg
