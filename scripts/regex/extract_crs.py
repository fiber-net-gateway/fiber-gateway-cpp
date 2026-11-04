#!/usr/bin/env python3
"""Extracts what the regex tests need from an OWASP CRS checkout.

    scripts/regex/extract_crs.py <crs-checkout> <fixture-dir> [payload-count]

Writes into the fixture directory:
  rx_patterns.txt   every @rx pattern, as file:line<TAB>pattern
  rule_groups.txt   file:line<TAB>phase;targets;transforms, the rules that see the
                    same text and so can share a RegexSet
  payloads.txt      attack payloads from the CRS regression tests, hex encoded.
                    scripts/regex/gen_crs_golden.go adds the expected results.
  data/             the word lists behind @pmFromFile
"""
import glob
import os
import random
import re
import shutil
import sys

RX = re.compile(rb'"!?@rx\s+((?:[^"\\]|\\.)*)"')
RULE = re.compile(r'^\s*SecRule\s+(\S+)\s+"!?@rx\s')
PAYLOAD = re.compile(r'^\s*(?:data|uri|encoded_request|User-Agent|Cookie|Referer|Content-Type):\s*(.+?)\s*$')


def rule_files(root):
    return sorted(glob.glob(os.path.join(root, 'rules', '*.conf')))


def write_patterns(root, path):
    with open(path, 'wb') as out:
        for name in rule_files(root):
            with open(name, 'rb') as rules:
                for number, line in enumerate(rules, 1):
                    if line.lstrip().startswith(b'#'):
                        continue
                    for match in RX.finditer(line):
                        location = f'{os.path.basename(name)}:{number}'.encode()
                        out.write(location + b'\t' + match.group(1).replace(b'\\"', b'"') + b'\n')


def write_groups(root, path):
    with open(path, 'w', encoding='utf-8') as out:
        for name in rule_files(root):
            with open(name, encoding='utf-8', errors='replace') as rules:
                lines = rules.read().split('\n')
            phase = None
            i = 0
            while i < len(lines):
                first = i
                statement = lines[i]
                while statement.endswith('\\') and i + 1 < len(lines):
                    i += 1
                    statement = statement[:-1] + ' ' + lines[i]
                i += 1
                if not statement.lstrip().startswith('SecRule'):
                    continue
                # A chained rule inherits the phase of the rule it follows.
                found = re.search(r'phase:(\d)', statement)
                if found:
                    phase = found.group(1)
                rule = RULE.match(statement)
                if rule is None:
                    continue
                transforms = [t for t in re.findall(r'[ ,"\']t:([A-Za-z0-9]+)', statement) if t != 'none']
                targets = sorted(t for t in rule.group(1).split('|') if not t.startswith('!'))
                out.write(f'{os.path.basename(name)}:{first + 1}\t{phase};{"|".join(targets)};{",".join(transforms)}\n')


def write_payloads(root, path, count):
    seen = set()
    for name in glob.glob(os.path.join(root, 'tests/regression/tests/**/*.yaml'), recursive=True):
        with open(name, encoding='utf-8', errors='surrogateescape') as tests:
            for line in tests:
                match = PAYLOAD.match(line)
                if match is None:
                    continue
                value = match.group(1)
                if len(value) >= 2 and value[0] == value[-1] and value[0] in '\'"':
                    value = value[1:-1]
                payload = value.encode('utf-8', 'surrogateescape')
                if 0 < len(payload) <= 4096:
                    seen.add(payload)
    payloads = sorted(seen)
    random.Random(7).shuffle(payloads)
    with open(path, 'w', encoding='ascii') as out:
        for payload in payloads[:count]:
            out.write(payload.hex() + '\n')


def main():
    root, fixture = sys.argv[1], sys.argv[2]
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 1500
    os.makedirs(os.path.join(fixture, 'data'), exist_ok=True)
    write_patterns(root, os.path.join(fixture, 'rx_patterns.txt'))
    write_groups(root, os.path.join(fixture, 'rule_groups.txt'))
    write_payloads(root, os.path.join(fixture, 'payloads.txt'), count)
    for name in glob.glob(os.path.join(root, 'rules', '*.data')):
        shutil.copy(name, os.path.join(fixture, 'data'))


if __name__ == '__main__':
    main()
