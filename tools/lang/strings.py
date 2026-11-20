#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""
List the UI's texts to translate, as a language file to fill in
(app/core/lang.h describes the format):

    python3 tools/lang/strings.py > Español.txt
    python3 tools/lang/strings.py --update Español.txt   # adds the texts it lacks

It takes the string literals in the UI's sources that read as words for
people: not printf formats, settings keys, paths or single symbols. A text
with %s ("%s songs", "Downloading %s") has a number or name put in its
place: keep each %s, in any order of the words around it. Any text a file
doesn't translate stays in English.
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SOURCES = ['app/ui/ui.c', 'app/net/radiodir.c', 'device/main.c', 'device/bt.c', 'device/wifi.c',
           'device/share.c', 'device/ssh.c']
# Sources whose other strings are SQL, logs or file formats: only the texts on
# a line that translates (or, in the updater, says why an update failed)
TR_ONLY = ['app/library/library.c', 'app/net/update.c', 'device/updater.c', 'app/sys/hibyupt.c', 'app/sys/uninstall.c']
TRANSLATES = re.compile(r'\b(tr|tr_fill|tr_num|N_|fail_locked)\(')
PRINTF = re.compile(r'%[-+ #0]*\d*(\.\d+)?(hh|h|ll|l|z)?[diouxXeEfgGcp%\[]')
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


def for_people(s, translated=False):
    """A text for people (translated: on a line that calls tr(), so short
    lower-case words such as "now" or "muted" count too)."""
    if 'abcdefghijklmnopqrstuvwxyz' in s or 'abcdef' in s:      # the text-entry wheel's letters, hex digits
        return False
    if any(x in s for x in ('2>', '/dev/', 'LD_LIBRARY', "sed -n", ' 2>&1')):   # shell commands
        return False
    if ' ' not in s.strip() and any(c in s for c in '_.'):    # S16_LE, host names
        return False
    if not translated and not re.search(r'[a-z]', s) and len(s) <= 6:   # PUT, GET, PK: protocol words
        return False
    if PRINTF.search(s):                                        # printf formats (only %s is filled in)
        return False
    if not re.search(r'[A-Za-z]', s.replace('%s', '')) or (not translated and (
            len(s) < 2 or not re.search(r'[A-Za-z]{2}', s.replace('%s', '')))):
        return False
    if '%s' in s:                                               # query strings, paths, "%s%s", a name
        rest = s.replace('%s', '').strip()                      # ("ES9018K2M %s"); "Downloading %s" is text
        if any(x in s for x in ('=', '&', '/', '%s%s')) or (' ' not in rest and not rest.endswith(':')
                                                            and not rest.isalpha()):
            return False
    if re.fullmatch(r'[a-z0-9_]+(\.[a-z0-9_]+)+', s):          # settings keys
        return False
    if '/' in s and ' ' not in s:                               # paths, MIME types
        return False
    if not translated and re.fullmatch(r'[a-z0-9_\-]+', s):    # identifiers, ids
        return False
    if s.startswith(('http', '#', '-', '.')) or s.isupper() and ' ' not in s and len(s) > 12:
        return False
    return True


def code_of(line):
    """The line without a // comment (one outside the string literals)."""
    q = None
    i = 0
    while i < len(line):
        c = line[i]
        if q:
            if c == '\\':
                i += 1
            elif c == q:
                q = None
        elif c in '"\'':
            q = c
        elif line.startswith('//', i):
            return line[:i]
        i += 1
    return line


def texts():
    seen = []
    for rel in SOURCES + TR_ONLY:
        path = os.path.join(ROOT, rel)
        if not os.path.exists(path):
            continue
        for line in open(path, encoding='utf-8'):
            code = code_of(line)
            if code.lstrip().startswith(('#include', '/*', '*', 'fprintf', 'perror')) or 'fprintf(' in code:
                continue
            translated = bool(TRANSLATES.search(code))
            if rel in TR_ONLY and not translated:
                continue
            for m in LITERAL.finditer(code):
                s = decode(m.group(1))
                if for_people(s, translated) and s not in seen:
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
    print('# Lines left empty after " = " stay in English. Keep each %s: a number or a name')
    print('# goes there. The file\'s name is the one shown in Settings > Interface > Language;')
    print('# put it in the card\'s fathom/lang folder.')
    for s in found:
        print('%s = ' % escape(s))


if __name__ == '__main__':
    main()
