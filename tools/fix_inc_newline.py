"""One-off: repair the MAP_BAD_PAYLOAD format string in the collection template.

Only the inserted diagnostic line is touched: the real newline between
`caller=%s.%s` and the closing quote is replaced with the two characters
backslash + n so the C string literal is valid again.  Uses chr(92) so the
shell/heredoc layer cannot collapse the escape.
"""
from pathlib import Path

BACKSLASH = chr(92)
TARGET = Path('sdk-runtime-spike/build/integrated-login-next/source/generated/upstream_collections.inc')

text = TARGET.read_text(encoding='utf-8')
broken = 'caller=%s.%s' + chr(10) + '"'
fixed = 'caller=%s.%s' + BACKSLASH + 'n"'
count = text.count(broken)
if count != 1:
    raise SystemExit('unexpected occurrence count: %d' % count)
TARGET.write_text(text.replace(broken, fixed), encoding='utf-8')

check = TARGET.read_text(encoding='utf-8')
assert 'caller=%s.%s' + BACKSLASH + 'n"' in check
assert broken not in check
print('fixed MAP_BAD_PAYLOAD newline')
