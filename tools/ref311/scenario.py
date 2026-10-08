"""One test scenario, two drivers: real 3.11 in DOSBox-X (AUTOTYPE buttons) and a libw16 port
(W16_SCRIPT). Keeping both generated from one file guarantees identical input on both sides.

Scenario lines (blank lines and # comments ignored):
    type TEXT     types TEXT key by key (letters, digits, space, tab as \\t, . , ; / - = ' [ ] \\ `)
    key NAME      one key: enter esc tab space alt up down left right home end pageup pagedown
                  insert del bs f1..f12, or a single character
    shot          the screen has settled; both sides record a frame here

  scenario.py autotype FILE [PAUSE_COMMAS]   -> AUTOTYPE buttons on one line
  scenario.py w16 FILE                       -> W16_SCRIPT for the port (frames: shots/NN.png)
"""
import sys

AT_KEYS = {'enter': 'enter', 'esc': 'esc', 'tab': 'tab', 'space': 'space', 'alt': 'lalt', 'up': 'up',
           'down': 'down', 'left': 'left', 'right': 'right', 'home': 'home', 'end': 'end',
           'pageup': 'pageup', 'pagedown': 'pagedown', 'insert': 'insert', 'del': 'delete', 'bs': 'bspace'}
AT_KEYS.update({'f%d' % i: 'f%d' % i for i in range(1, 13)})
PUNCT = {' ': 'space', '\t': 'tab', '.': 'period', ',': 'comma', ';': 'semicolon', '/': 'slash',
         '-': 'minus', '=': 'equals', "'": 'quote', '[': 'lbracket', ']': 'rbracket', '\\': 'backslash',
         '`': 'grave'}


def steps(path):
    for n, line in enumerate(open(path, encoding='ascii'), 1):
        line = line.rstrip('\r\n')
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        cmd, _, arg = line.strip(' ').partition(' ')
        if cmd == 'type':
            arg = arg.replace('\\t', '\t')
            for c in arg:
                if not (c.isalnum() or c in PUNCT):
                    raise SystemExit('%s:%d: cannot type %r' % (path, n, c))
            yield 'type', arg
        elif cmd in ('key', 'shot'):
            yield cmd, arg.strip().lower()
        else:
            raise SystemExit('%s:%d: unknown command %s' % (path, n, cmd))


def char_button(c):
    """AUTOTYPE button / w16 key name for one typed character"""
    if c in PUNCT:
        return PUNCT[c], PUNCT[c]
    if c.isupper():
        return c, 'shift+' + c.lower()
    return c, c


def autotype(path, pause=12):
    out = []
    for cmd, arg in steps(path):
        if cmd == 'type':
            out += [char_button(c)[0] for c in arg]
        elif cmd == 'key':
            out.append(AT_KEYS.get(arg, arg))
        else:
            out += [','] * pause
    return ' '.join(out)


def w16(path):
    out, n = [], 0
    for cmd, arg in steps(path):
        if cmd == 'type':
            out += ['key ' + char_button(c)[1] for c in arg]
        elif cmd == 'key':
            out.append('key ' + arg)
        else:
            n += 1
            out += ['sleep 400', 'shot shots/%02d.png' % n]
    return '\n'.join(['sleep 400'] + out + ['quit', ''])


if __name__ == '__main__':
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    if sys.argv[1] == 'autotype':
        print(autotype(sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 12))
    elif sys.argv[1] == 'w16':
        sys.stdout.write(w16(sys.argv[2]))
    else:
        raise SystemExit(__doc__)
