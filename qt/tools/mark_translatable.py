#!/usr/bin/env python3
"""Find user-facing string literals in dinero-qt and wrap them in tr().

DRY RUN BY DEFAULT. Nothing is written unless --apply is passed.

What this tool will and will not do, and why (see qt/MULTI-LANGUAGE-PLAN.md):

  WRAPS   a bare "..." literal passed to a known user-facing Qt setter, when
          the literal survives every deny rule below.

  SKIPS   and reports, with a reason:
            - glossary terms that must stay English in every language
              (product name, ticker, address prefixes, protocol vocabulary)
            - stylesheets, object names and other code-not-text arguments
            - RPC method names, hex, file paths, URLs
            - strings already inside tr(), QT_TR_NOOP or translate()
            - literals too short or with no letters to be prose

  REFUSES to touch string CONCATENATION building user text. Rewriting
          setText("Fee: " + x) as setText(tr("Fee: ") + x) is mechanically
          valid and produces an untranslatable fragment that LOOKS done, which
          is worse than leaving it English. Those are reported for manual
          conversion to .arg() and are never modified here.

The report is the point. Read what it skipped, not just what it would change.
"""

import argparse
import json
import os
import re
import sys

# Qt calls whose string argument is shown to a person.
USER_FACING_CALLS = (
    "setText", "setWindowTitle", "setToolTip", "setPlaceholderText",
    "setTitle", "setStatusTip", "setWhatsThis", "addTab", "addItem",
)

# Calls whose string argument is code, never prose. Never wrap these.
CODE_ONLY_CALLS = (
    "setStyleSheet", "setObjectName", "setProperty", "setAccessibleName",
    "setFont", "setIcon", "findChild", "setAttribute", "connect",
)

# Terms that must stay English in every language. A literal containing any of
# these is reported for human judgment rather than wrapped, so a translator is
# never handed the product's own vocabulary to localise.
GLOSSARY_TERMS = (
    "Dinero", "DIN", "din1", "dins1", "Taproot", "UTXO", "BIP39", "BIP32",
    "P2TR", "P2WPKH", "Utreexo", "AssumeUTXO", "Poseidon", "Spartan",
    "mempool", "testnet", "regtest", "mainnet", "RPC", "P2P", "SHA256",
    # wDIN has no word boundary before "DIN", so it needs its own entry.
    "wDIN", "Orchard", "Stratum", "Utreexo",
)

HEX_RE = re.compile(r"^[0-9a-fA-F]{6,}$")
URL_RE = re.compile(r"^[a-z][a-z0-9+.-]*://", re.I)
FORMAT_ONLY_RE = re.compile(r"^[\s%0-9.dsfx*+-]*$")
HAS_LETTERS_RE = re.compile(r"[A-Za-z]{2,}")
STYLESHEET_HINT_RE = re.compile(r"[{}]|^\s*(QLabel|QWidget|QPushButton|background|color|border|font-)")

# A call site: <call>( ... ) capturing the first argument region.
CALL_RE = re.compile(
    r"\b(" + "|".join(USER_FACING_CALLS) + r")\s*\(", re.M)

STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')


def find_matching_paren(text, open_index):
    """Return index just past the ) matching the ( at open_index, or -1."""
    depth = 0
    i = open_index
    in_string = False
    escape = False
    while i < len(text):
        ch = text[i]
        if in_string:
            if escape:
                escape = False
            elif ch == "\\":
                escape = True
            elif ch == '"':
                in_string = False
        else:
            if ch == '"':
                in_string = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    return i + 1
        i += 1
    return -1


def skip_reason(literal, arg_text, line_text):
    """Why this literal must not be auto-wrapped, or None to wrap it."""
    text = literal
    if not text.strip():
        return "empty"
    if len(text.strip()) < 2:
        return "too-short"
    if not HAS_LETTERS_RE.search(text):
        return "no-prose"          # icons, separators, punctuation, numbers
    if FORMAT_ONLY_RE.match(text):
        return "format-only"
    if HEX_RE.match(text.strip()):
        return "hex"
    if URL_RE.match(text.strip()):
        return "url"
    if text.strip().startswith("/") or text.strip().startswith("~/"):
        return "path"
    if STYLESHEET_HINT_RE.search(text):
        return "stylesheet"
    if "." in text and " " not in text and text.count(".") >= 1 and re.match(
            r"^[a-z][a-z0-9_]*\.[a-z][a-z0-9_]*$", text.strip()):
        return "rpc-method-name"
    for term in GLOSSARY_TERMS:
        if re.search(r"\b" + re.escape(term) + r"\b", text):
            return "glossary:" + term
    if "tr(" in line_text and literal in line_text.split("tr(", 1)[1]:
        return "already-marked"
    if "QT_TR_NOOP" in line_text or "translate(" in line_text:
        return "already-marked"
    return None


def analyse_file(path):
    src = open(path, encoding="utf-8", errors="replace").read()
    lines = src.split("\n")
    results = {"wrap": [], "skip": [], "concat": []}

    for match in CALL_RE.finditer(src):
        call = match.group(1)
        open_paren = match.end() - 1
        close = find_matching_paren(src, open_paren)
        if close < 0:
            continue
        arg_text = src[open_paren + 1:close - 1]
        line_no = src.count("\n", 0, match.start()) + 1
        line_text = lines[line_no - 1] if line_no - 1 < len(lines) else ""

        # Code-only call on the same statement: leave it entirely alone.
        if any(c in line_text for c in CODE_ONLY_CALLS):
            continue

        strings = STRING_RE.findall(arg_text)
        if not strings:
            continue

        # Concatenation into user-visible text: report, never rewrite.
        if "+" in arg_text and len(arg_text.split("+")) > 1:
            results["concat"].append(
                {"file": path, "line": line_no, "call": call,
                 "sample": strings[0][:60]})
            continue

        for literal in strings:
            reason = skip_reason(literal, arg_text, line_text)
            entry = {"file": path, "line": line_no, "call": call,
                     "text": literal[:80]}
            if reason:
                entry["reason"] = reason
                results["skip"].append(entry)
            else:
                results["wrap"].append(entry)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", default="src", help="directory to scan")
    parser.add_argument("--report", help="write the full JSON report here")
    parser.add_argument("--apply", action="store_true",
                        help="actually rewrite files (default is dry run)")
    args = parser.parse_args()

    if args.apply:
        print("REFUSING --apply: this increment introduces the tool and its "
              "report only.\nReview the report, agree the deny-list, then "
              "enable apply in a separate change.", file=sys.stderr)
        return 2

    totals = {"wrap": [], "skip": [], "concat": []}
    files = []
    for root, _dirs, names in os.walk(args.src):
        if "build" in root.split(os.sep):
            continue
        for name in sorted(names):
            if name.endswith(".cpp"):
                files.append(os.path.join(root, name))

    for path in files:
        res = analyse_file(path)
        for key in totals:
            totals[key].extend(res[key])

    reasons = {}
    for entry in totals["skip"]:
        key = entry["reason"].split(":")[0]
        reasons[key] = reasons.get(key, 0) + 1

    print("dinero-qt translatable-string scan (DRY RUN, nothing written)")
    print("files scanned: %d\n" % len(files))
    print("WOULD WRAP in tr():      %d" % len(totals["wrap"]))
    print("SKIPPED (with reason):   %d" % len(totals["skip"]))
    for reason in sorted(reasons, key=lambda r: -reasons[r]):
        print("    %-18s %d" % (reason, reasons[reason]))
    print("REFUSED, concatenation:  %d   <- convert to .arg() BY HAND"
          % len(totals["concat"]))

    glossary_hits = [e for e in totals["skip"] if e["reason"].startswith("glossary:")]
    if glossary_hits:
        print("\nglossary protections that fired -- these need a HUMAN decision:")
        print("  (the sentence may well be translatable; the TERM inside it must not be)")
        for entry in glossary_hits[:5]:
            print("    %s:%d  %s  [%s]"
                  % (os.path.basename(entry["file"]), entry["line"],
                     entry["text"][:42], entry["reason"]))

    print("\nwould-wrap sample:")
    for entry in totals["wrap"][:5]:
        print("    %s:%d  %s(\"%s\")"
              % (os.path.basename(entry["file"]), entry["line"],
                 entry["call"], entry["text"][:46]))

    if args.report:
        with open(args.report, "w", encoding="utf-8") as stream:
            json.dump(totals, stream, indent=1, ensure_ascii=False)
        print("\nfull report: %s" % args.report)
    return 0


if __name__ == "__main__":
    sys.exit(main())
