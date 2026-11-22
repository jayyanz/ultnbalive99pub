"""Writes nba99fix.ini for one build from common/nba99fix.ini.in.
Lines between ';@debug' and ';@end' are kept only in the Debug build, lines
between ';@main' and ';@end' only in Main. '{LOG}' becomes the build's log
default (1 Debug, 0 Main)."""
import sys
src, variant = sys.argv[1], sys.argv[2]
keep, out = True, []
for line in open(src, encoding='utf-8').read().splitlines():
    t = line.strip()
    if t == ';@debug': keep = variant == 'Debug'; continue
    if t == ';@main': keep = variant == 'Main'; continue
    if t == ';@end': keep = True; continue
    if keep:
        out.append(line.replace('{LOG}', '1' if variant == 'Debug' else '0').replace('{BUILD}', variant))
sys.stdout.write('\r\n'.join(out) + '\r\n')
