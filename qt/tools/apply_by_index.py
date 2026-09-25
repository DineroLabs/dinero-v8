#!/usr/bin/env python3
"""Apply translations to a catalog from an index-keyed JSON map.

The six catalogs seeded together share an identical ordered list of
untranslated entries, so a fixed index is a stable address for the same source
string in every one of them. That lets a translator work through the list in
ranges and lets a reviewer compare the same index across languages.

Usage: apply_by_index.py <catalog.ts> <master.json> <translations.json>
  master.json       [[context, source], ...] in the catalog's own order
  translations.json {"<index>": "<translation>", ...}
"""
import json, sys, xml.etree.ElementTree as ET

def main(ts_path, master_path, tr_path):
    master = json.load(open(master_path, encoding='utf-8'))
    table = {int(k): v for k, v in json.load(open(tr_path, encoding='utf-8')).items()}
    bad = [i for i in table if i < 0 or i >= len(master)]
    if bad:
        print(f"index out of range: {bad[:5]}", file=sys.stderr)
        return 2

    # Build (context, source) -> translation, refusing a collision where the
    # same source in the same context would get two different translations.
    want = {}
    for i, t in table.items():
        key = (master[i][0], master[i][1])
        if key in want and want[key] != t:
            print(f"conflicting translations for index {i}: {key[1]!r}", file=sys.stderr)
            return 2
        want[key] = t

    tree = ET.parse(ts_path); root = tree.getroot()
    applied = skipped = 0
    for context in root.findall('context'):
        name = context.findtext('name')
        for message in context.findall('message'):
            source = message.findtext('source')
            translation = message.find('translation')
            if translation is None or (name, source) not in want:
                continue
            if translation.get('type') != 'unfinished':
                skipped += 1
                continue
            translation.text = want[(name, source)]
            del translation.attrib['type']
            applied += 1

    head = []
    with open(ts_path, encoding='utf-8') as f:
        for line in f:
            if line.lstrip().startswith('<TS'):
                break
            head.append(line)
    body = ET.tostring(root, encoding='unicode')
    with open(ts_path, 'w', encoding='utf-8') as f:
        f.write(''.join(head)); f.write(body)
        if not body.endswith('\n'):
            f.write('\n')
    print(f"applied={applied} already_translated={skipped} requested={len(table)}")
    return 0

if __name__ == '__main__':
    sys.exit(main(*sys.argv[1:4]))
