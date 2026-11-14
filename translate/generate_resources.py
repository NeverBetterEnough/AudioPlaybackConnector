"""Generate the project's Chinese YMO resources using only the Python standard library."""
import ast
import re
import struct
from pathlib import Path


def generate_catalog(source):
    units = {}
    for block in re.split(r'\n\s*\n', source.read_text(encoding='utf-8-sig')):
        fields, active, fuzzy = {}, None, False
        for raw in block.splitlines():
            line = raw.strip()
            if line.startswith('#'):
                fuzzy |= line.startswith('#,') and 'fuzzy' in line
                continue
            match = re.match(r'(msgctxt|msgid|msgstr)\s+(".*")$', line)
            if match:
                active = match.group(1)
                fields[active] = ast.literal_eval(match.group(2))
            elif line.startswith('"') and active:
                fields[active] += ast.literal_eval(line)
            elif line:
                raise ValueError('Unsupported PO entry: ' + line)
        original, translated = fields.get('msgid', ''), fields.get('msgstr', '')
        if not original or not translated or fuzzy:
            continue
        if fields.get('msgctxt'):
            original = fields['msgctxt'] + '\x04' + original
        hashed = 0x811c9dc5
        for byte in original.encode('utf-16le'):
            hashed = ((hashed ^ byte) * 0x01000193) & 0xffffffff
        units[hashed] = translated.encode('utf-16le') + b'\x00\x00'
    result = bytearray(struct.pack('<H', len(units)))
    offset = 2 + len(units) * 6
    for hashed, value in units.items():
        result += struct.pack('<IH', hashed, offset)
        offset += len(value)
    return result + b''.join(units.values())


if __name__ == '__main__':
    root = Path(__file__).resolve().parent
    output = root / 'generated'
    output.mkdir(exist_ok=True)
    for language in ('zh_CN', 'zh_TW'):
        data = generate_catalog(root / 'source' / (language + '.po'))
        (output / (language + '.ymo')).write_bytes(data)
        print(language, struct.unpack_from('<H', data)[0], 'translations')
    resource = '''#include "../../targetver.h"
#include "windows.h"
LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED
1 YMO "zh_CN.ymo"
LANGUAGE LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL
1 YMO "zh_TW.ymo"
'''
    with (output / 'translate.rc').open('w', encoding='utf-16', newline='\r\n') as file:
        file.write(resource)
