#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
List the UI's texts to translate, as a language file to fill in
(app/core/lang.h describes the format):

    python3 tools/lang/strings.py > Español.txt
    python3 tools/lang/strings.py --update Español.txt   # adds the texts it lacks

It takes the string literals in the UI's sources that read as words for
people: not format strings (with %), settings keys, paths or single symbols.
Texts the player builds from parts ("3 dB left") aren't in the list and stay
in English; so does any text a file doesn't translate.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCES = ['app/ui/ui.c', 'app/net/radiodir.c', 'device/main.c', 'device/bt.c', 'device/wifi.c', 'device/share.c', 'device/ssh.c']
LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')


def decode(lit):
    """A C literal's bytes (\\x escapes are UTF-8 bytes) as text."""
    raw = bytearray()
    i = 0
    while i < len(lit):
        c = lit[i]
        if c == '\\' and i + 1 < len(lit):
            n = lit[i + 1]
            if n == 'x':
                m = re.match(r'[0-9a-fA-F]{1,2}', lit[i + 2:])
                raw.append(int(m.group(0), 16))
                i += 2 + len(m.group(0))
                continue
            raw += {'n': b'\\n', 't': b'\t', '"': b'"', '\\': b'\\', "'": b"'"}.get(n, n.encode())
            i += 2
            continue
        raw += c.encode('utf-8')
        i += 1
    return raw.decode('utf-8', errors='replace')


def for_people(s):
    if 'abcdefghijklmnopqrstuvwxyz' in s or 'abcdef' in s:      # the text-entry wheel's letters, hex digits
        return False
    if any(x in s for x in ('2>', '/dev/', 'LD_LIBRARY', "sed -n", ' 2>&1')):   # shell commands
        return False
    if ' ' not in s.strip() and any(c in s for c in '_.'):    # S16_LE, host names
        return False
    if not re.search(r'[a-z]', s) and len(s) <= 6:             # PUT, GET, PK: protocol words
        return False
    if '%' in s or len(s) < 2 or not re.search(r'[A-Za-z]{2}', s):
        return False
    if re.fullmatch(r'[a-z0-9_]+(\.[a-z0-9_]+)+', s):          # settings keys
        return False
    if '/' in s and ' ' not in s:                               # paths, MIME types
        return False
    if re.fullmatch(r'[a-z0-9_\-]+', s):                        # identifiers, ids
        return False
    if s.startswith(('http', '#', '-', '.')) or s.isupper() and ' ' not in s and len(s) > 12:
        return False
    return True


def texts():
    seen = []
    for rel in SOURCES:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        for line in open(path, encoding='utf-8'):
            code = line.split('//')[0]
            if code.lstrip().startswith(('#include', '/*', '*', 'fprintf', 'perror')) or 'fprintf(' in code:
                continue
            for m in LITERAL.finditer(code):
                s = decode(m.group(1))
                if for_people(s) and s not in seen:
                    seen.append(s)
    return seen


def escape(s):
    return s.replace('\n', '\\n')


def main():
    found = texts()
    if len(sys.argv) == 3 and sys.argv[1] == '--update':
        have = set()
        with open(sys.argv[2], encoding='utf-8') as f:
            for line in f:
                if ' = ' in line and not line.lstrip().startswith('#'):
                    have.add(line.split(' = ', 1)[0].strip())
        new = [s for s in found if escape(s) not in have]
        with open(sys.argv[2], 'a', encoding='utf-8') as f:
            for s in new:
                f.write('%s = \n' % escape(s))
        print('%d texts added' % len(new), file=sys.stderr)
        return
    print('# Fathom: a translation. One line per text: the English, " = ", the translation.')
    print('# Lines left empty after " = " stay in English. The file\'s name is the one shown')
    print('# in Settings > Display > Language; put it in the card\'s fathom/lang folder.')
    for s in found:
        print('%s = ' % escape(s))


if __name__ == '__main__':
    main()
