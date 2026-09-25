#!/usr/bin/env python3
"""Apply translations to a Qt .ts catalog from a JSON map.

The map is {context_name: {source_text: translation_text}}. Only entries that
are currently unfinished are filled, so re-running is safe and an existing
human-reviewed translation is never overwritten.

Qt .ts is XML, so this parses rather than pattern-matches. The DOCTYPE is
preserved by hand because ElementTree drops it.
"""
import json, sys, xml.etree.ElementTree as ET

def main(ts_path, map_path):
    with open(map_path, encoding='utf-8') as f:
        mapping = json.load(f)
    tree = ET.parse(ts_path)
    root = tree.getroot()

    applied = skipped_done = missing = 0
    seen_sources = {}
    for context in root.findall('context'):
        name = context.findtext('name')
        table = mapping.get(name)
        if not table:
            continue
        for message in context.findall('message'):
            source = message.findtext('source')
            if source is None:
                continue
            seen_sources.setdefault(name, set()).add(source)
            if source not in table:
                continue
            translation = message.find('translation')
            if translation is None:
                continue
            if translation.get('type') != 'unfinished':
                skipped_done += 1
                continue
            translation.text = table[source]
            del translation.attrib['type']
            applied += 1

    for name, table in mapping.items():
        for source in table:
            if source not in seen_sources.get(name, ()):
                missing += 1
                print(f"  NOT FOUND in {name}: {source!r}", file=sys.stderr)

    # Preserve the XML declaration and DOCTYPE that ElementTree discards.
    with open(ts_path, encoding='utf-8') as f:
        head = []
        for line in f:
            if line.lstrip().startswith('<TS'):
                break
            head.append(line)
    body = ET.tostring(root, encoding='unicode')
    with open(ts_path, 'w', encoding='utf-8') as f:
        f.write(''.join(head))
        f.write(body)
        if not body.endswith('\n'):
            f.write('\n')

    print(f"applied={applied} already_translated={skipped_done} not_found={missing}")
    return 1 if missing else 0

if __name__ == '__main__':
    if len(sys.argv) != 3:
        print("usage: apply_translations.py <catalog.ts> <map.json>", file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
