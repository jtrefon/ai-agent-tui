#!/usr/bin/env python3
"""Read ANSI 256-color art from stdin, output C++ colour-index table."""
import sys

data = sys.stdin.buffer.read()
rows = []
cur = []
i = 0
n = len(data)

while i < n:
    if data[i] == 0x1b and i + 1 < n and data[i+1] == ord('['):
        # Find the terminating 'm'
        end = data.find(ord('m'), i + 2)
        if end == -1:
            break
        seq = data[i+2:end]
        parts = seq.split(b';')
        # Sequence is "48;5;NNN" for background colour
        is_color = (len(parts) >= 3 and parts[0] == b'48' and parts[1] == b'5')
        if is_color:
            try:
                cur.append(int(parts[2]))
            except ValueError:
                pass
        i = end + 1
        # Only color-setting escapes are followed by a cell character (space).
        if is_color and i < n and data[i:i+1] != b'\n':
            i += 1
    elif data[i] == ord('\n'):
        if cur:
            rows.append(cur)
            cur = []
        i += 1
    else:
        # Regular char without a preceding escape — shouldn't happen in clean art
        i += 1

if cur:
    rows.append(cur)

if not rows:
    print('// No data parsed — check input format', file=sys.stderr)
    print('namespace tui { namespace welcome { namespace {')
    print('const int kArtRows = 0; const int kArtCols = 0; }}}')
    sys.exit(1)

maxc = max(len(r) for r in rows)
print(f'// Generated: {len(rows)} rows x {maxc} cols')
print('#include "welcome.h"')
print('#include "widgets.h"')
print('namespace tui { namespace welcome { namespace {')
print(f'const unsigned char kArtData[{len(rows)}][{maxc}] = {{')
for r in rows:
    row = ','.join(str(c) for c in r)
    # Pad shorter rows to maxc with zeros
    pad = ',0' * (maxc - len(r)) if len(r) < maxc else ''
    print(f'  {{{row}{pad}}},')
print('};')
print(f'const int kArtRows = {len(rows)};')
print(f'const int kArtCols = {maxc};')
print('}}} // namespace')
