#!/usr/bin/env python3
"""Wrap user-facing string literals of ONE source file in tr().

Written for screens whose prose lives in multi-line adjacent string literals,
which the broader scanner deliberately leaves alone:

    label->setText(
        "<b>Your seed phrase works across wallets.</b><br><br>"
        "Write it down and keep it offline.");

C++ concatenates those into one string, so the WHOLE run must go inside a
single tr(), not just the first fragment. Wrapping only the first piece would
split one sentence into several catalog entries and make it untranslatable in
any language whose word order differs.

DRY RUN BY DEFAULT; pass --apply to write.

Never wrapped:
  * arguments to setStyleSheet / setObjectName and friends (code, not prose)
  * literals with no letters, or shorter than two characters
  * anything already inside tr()
  * a call whose argument mixes literals with variables (a concatenation),
    which must become .arg() by hand instead

This tool only ever touches compile-time literals. Runtime data — notably a
BIP39 seed phrase, which arrives from the daemon over RPC — cannot be reached
by tr() at all and is therefore untouchable by construction.
"""

import argparse
import re
import sys

SINGLE_ARG_CALLS = (
    "setText", "setTitle", "setSubTitle", "setPlaceholderText",
    "setToolTip", "setWindowTitle", "setWhatsThis", "setStatusTip",
)
CTOR_CALLS = ("QLabel", "QPushButton", "QCheckBox", "QRadioButton", "QGroupBox")
MSGBOX_RE = re.compile(r"QMessageBox::(warning|critical|information|question)\s*\(")

CODE_ONLY = ("setStyleSheet", "setObjectName", "setProperty", "setAccessibleName")

STRING_LITERAL = re.compile(r'"(?:[^"\\]|\\.)*"')
HAS_LETTERS = re.compile(r"[A-Za-z]{2,}")


def find_close(text, open_index):
    depth, i, in_str, esc = 0, open_index, False, False
    while i < len(text):
        c = text[i]
        if in_str:
            if esc: esc = False
            elif c == "\\": esc = True
            elif c == '"': in_str = False
        else:
            if c == '"': in_str = True
            elif c == "(": depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0: return i
        i += 1
    return -1


def literal_run_only(arg):
    """True when arg is nothing but adjacent string literals and whitespace."""
    stripped = STRING_LITERAL.sub("", arg).strip()
    return stripped == "" and STRING_LITERAL.search(arg) is not None


def worth_translating(arg):
    text = "".join(m.group(0)[1:-1] for m in STRING_LITERAL.finditer(arg))
    if len(text.strip()) < 2:
        return False
    return bool(HAS_LETTERS.search(text))


def split_top_level_args(arg_text):
    parts, depth, cur, in_str, esc = [], 0, "", False, False
    for c in arg_text:
        if in_str:
            cur += c
            if esc: esc = False
            elif c == "\\": esc = True
            elif c == '"': in_str = False
            continue
        if c == '"': in_str = True; cur += c; continue
        if c in "([{": depth += 1
        elif c in ")]}": depth -= 1
        if c == "," and depth == 0:
            parts.append(cur); cur = ""
        else:
            cur += c
    parts.append(cur)
    return parts


def process(src):
    out, changes, skipped = src, [], []
    # Work from the end so earlier offsets stay valid.
    calls = []
    for name in SINGLE_ARG_CALLS:
        for m in re.finditer(r"\b" + name + r"\s*\(", out):
            calls.append((m.start(), m.end() - 1, name, "single"))
    for name in CTOR_CALLS:
        for m in re.finditer(r"\bnew\s+" + name + r"\s*\(", out):
            calls.append((m.start(), m.end() - 1, name, "single"))
    for m in MSGBOX_RE.finditer(out):
        calls.append((m.start(), m.end() - 1, "QMessageBox", "multi"))
    calls.sort(key=lambda c: -c[0])

    for start, open_paren, name, kind in calls:
        close = find_close(out, open_paren)
        if close < 0:
            continue
        arg_text = out[open_paren + 1:close]
        line_start = out.rfind("\n", 0, start) + 1
        line = out[line_start:out.find("\n", start)]
        if any(c in line for c in CODE_ONLY) or "tr(" in arg_text:
            continue

        if kind == "single":
            if not literal_run_only(arg_text):
                if STRING_LITERAL.search(arg_text):
                    skipped.append((name, "mixed-with-code"))
                continue
            if not worth_translating(arg_text):
                skipped.append((name, "no-prose"))
                continue
            out = out[:open_paren + 1] + "tr(" + arg_text + ")" + out[close:]
            changes.append(name)
        else:
            parts = split_top_level_args(arg_text)
            new_parts, touched = [], False
            for p in parts:
                if literal_run_only(p) and worth_translating(p):
                    lead = len(p) - len(p.lstrip())
                    new_parts.append(p[:lead] + "tr(" + p.lstrip() + ")")
                    touched = True
                else:
                    new_parts.append(p)
            if touched:
                out = out[:open_paren + 1] + ",".join(new_parts) + out[close:]
                changes.append(name)
    return out, changes, skipped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--apply", action="store_true")
    a = ap.parse_args()
    src = open(a.file, encoding="utf-8").read()
    out, changes, skipped = process(src)
    from collections import Counter
    print("would wrap: %d" % len(changes))
    for k, v in Counter(changes).most_common():
        print("    %-22s %d" % (k, v))
    if skipped:
        print("skipped:")
        for k, v in Counter(skipped).most_common():
            print("    %-22s %s" % (k[0], k[1]))
    if a.apply:
        open(a.file, "w", encoding="utf-8").write(out)
        print("APPLIED to", a.file)
    else:
        print("(dry run; pass --apply to write)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
