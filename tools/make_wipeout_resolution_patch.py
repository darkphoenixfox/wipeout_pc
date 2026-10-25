#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The "Resolution Patch - WxH" entry of WipEout Omega Collection for any size and aspect ratio.

  python tools/make_wipeout_resolution_patch.py EBOOT WIDTH HEIGHT [--name NAME] [--write]
      Prints (or with --write puts into patches/WipEout_Omega_Collection.xml, replacing an earlier entry
      of that size, before "Native 4K rendering") the entry for WIDTH x HEIGHT. EBOOT is the CUSA07671
      01.07 eboot.bin (sha256 3a52e910...): every site is checked against it, by its signature.
      --name: the size's built-in name instead of WIDTHxHEIGHT ("4K" for 3840 2160).

The render resolution sites (output size, half-resolution buffers) are those of the "Resolution Patch -
1440p" entry of that file. The camera (0x3adb40 and 0x3af3a0, one constant pool each, at 0x860cfc and
0x860d3c) builds its projection from a fixed *vertical* field of view f (65 degrees in a race):
    t = tan(f * pi/360);  y = 1/t;  x = 1/(t * A)          A: the aspect ratio float, 16/9
and its culling planes from the half angles f * pi/360 (vertical) and f * pi/360 * 16/9 (horizontal: a
constant 0.0155 at 0x860d28, wider than the 16:9 view, 57.8 against 48.6 degrees).
  - Wider than 16:9 (21:9, 32:9): A = WIDTH/HEIGHT in both pools, so the view widens (Hor+). From about
    2.49:1 the true horizontal half angle passes the culling one, so objects at the sides would vanish:
    the culling constant is raised to 1.1 times the true angle at f = 65.
  - Taller than 16:9 (4:3, 16:10): A = WIDTH/HEIGHT alone would crop the sides. The pi/360 of both
    pools is scaled so tan(f * c') = tan(f * pi/360) * (16/9) / A at f = 65: the horizontal view stays
    the one of 16:9 and the picture shows more at the top and bottom (Vert+). The scale is exact at
    65 degrees and within a few percent at the speed effect's wider angles. The horizontal culling
    angle is left as it is (the horizontal view does not change).
"""
import argparse
import math
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
sys.path.insert(0, str(ROOT / 'scripts'))
from make_signatures import load  # noqa: E402
from patches import EBOOT_BASE, find_pattern, parse_pattern  # noqa: E402

XML = ROOT / 'patches/WipEout_Omega_Collection.xml'
TEMPLATE = 'Resolution Patch - 1440p'
FOV = 65.0                                  # the race camera's vertical field of view, degrees
HALF_ANGLE = struct.unpack('<f', struct.pack('<f', math.pi / 360))[0]
POOLS = ('00 00 00 bf f3 8e 63 3f f3 8e 63 40 39 8e e3 3f 35 fa 0e 3c 00 00 80 bf 9a 99',
         '00 00 00 bf f3 8e 63 3f f3 8e 63 40 39 8e e3 3f 35 fa 0e 3c 00 00 80 bf 00 00')
ASPECT_AT, FOV_AT = 12, 16                  # 16/9 and pi/360 in each pool
CULL = '3c 00 00 80 bf 9a 99 99 bf 00 00 a0 40 00 00 00 3e 17 b7 d1 38 00 00 40 c0 97 2e 7e 3c'
CULL_AT = 25                                # 0.0155 = pi/360 * 16/9, the horizontal culling half angle
CULL_MARGIN = 1.1
AUTHOR = 'jontorre (sites by d4g)'


def f32(value):
    return struct.pack('<f', value).hex()


def ratio_name(width, height):
    for name, value in (('16:9', 16 / 9), ('16:10', 1.6), ('21:9', 2.37), ('32:9', 32 / 9), ('4:3', 4 / 3)):
        if abs(width / height - value) < 0.05:
            return name
    return f'{width / height:.3f}:1'


def camera_lines(width, height):
    """(comment, pattern, at, value) of the camera lines for this aspect ratio."""
    aspect = width / height
    if abs(aspect - 16 / 9) < 1e-6:
        return []
    lines = [('camera aspect ratio (16/9)', pool, ASPECT_AT, f32(aspect)) for pool in POOLS]
    t = math.tan(math.radians(FOV / 2))
    if aspect < 16 / 9:
        half = math.degrees(math.atan(t * (16 / 9) / aspect))
        scale = HALF_ANGLE * half / (FOV / 2)
        lines += [('camera field of view: pi/360 scaled, the 16:9 horizontal view (Vert+)', pool, FOV_AT, f32(scale))
                  for pool in POOLS]
    else:
        true_half = math.degrees(math.atan(t * aspect))
        if true_half > FOV / 2 * 16 / 9:
            cull = HALF_ANGLE * true_half * CULL_MARGIN / (FOV / 2)
            lines.append(('horizontal culling half angle (pi/360 * 16/9): wider than the view', CULL, CULL_AT, f32(cull)))
    return lines


def template_lines(xml):
    """(comment, pattern, at, original value, role) of the template entry's size lines."""
    root = ET.parse(xml, parser=ET.XMLParser(target=ET.TreeBuilder(insert_comments=True))).getroot()
    meta = next(m for m in root.iter('Metadata') if m.get('Name') == TEMPLATE)
    out, comment = [], None
    for node in meta.find('PatchList'):
        if node.tag is ET.Comment:
            comment = node.text.strip()
            continue
        role = ('half ' if 'half-resolution' in comment else '') + ('width' if 'width' in comment else 'height')
        out.append((comment, node.get('Pattern'), int(node.get('At')), len(node.get('Value')) // 2, role))
    return out


def size_lines(xml, width, height):
    sizes = {'width': width, 'height': height, 'half width': width // 2, 'half height': height // 2}
    return [(comment, pattern, at, sizes[role].to_bytes(length, 'little').hex())
            for comment, pattern, at, length, role in template_lines(xml)]


def entry(image, xml, width, height, name=None):
    if width % 2 or height % 2:
        raise SystemExit('the half-resolution buffers need an even width and height')
    lines = camera_lines(width, height) + size_lines(xml, width, height)
    body = []
    for comment, pattern, at, value in lines:
        found = find_pattern(image, parse_pattern(pattern))
        if len(found) != 1:
            raise SystemExit(f'{comment}: signature found {len(found)} times')
        address = EBOOT_BASE + found[0] + at
        body.append(f'            <!-- {comment} -->\n'
                    f'            <Line Type="bytes" Address="0x{address:08x}" Value="{value}" Pattern="{pattern}" At="{at}"/>')
    size_name = name or f'{width}x{height}'
    aspect, name = width / height, ratio_name(width, height)
    if abs(aspect - 16 / 9) < 1e-6:
        view = 'Same 16:9 view as the game, sharper.'
    elif aspect > 16 / 9:
        view = ('The camera keeps its vertical field of view, so the wider picture shows more at the sides (Hor+). '
                'HUD and menus stay 16:9 in the middle of the screen.')
    else:
        view = ('The camera keeps the horizontal view of the 16:9 game, so the taller picture shows more at the top '
                'and bottom (Vert+). HUD and menus stay 16:9 across the width of the screen.')
    note = (f'Renders at {width}x{height} ({name}): output and half-resolution buffers at that size'
            f'{"" if abs(aspect - 16 / 9) < 1e-6 else " and the camera set to match"}. {view}')
    return (f'    <Metadata Title="WipEout Omega Collection" Name="Resolution Patch - {size_name}" '
            f'Note="{note}" Author="{AUTHOR}" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="false">\n'
            f'        <PatchList>\n' + '\n'.join(body) + '\n        </PatchList>\n    </Metadata>\n')


def write(xml, size_name, text):
    source = xml.read_text(encoding='utf-8')
    name = re.escape(f'Resolution Patch - {size_name}')
    old = re.compile(rf'    <Metadata [^>]*Name="{name}".*?</Metadata>\n', re.S)
    if old.search(source):
        source = old.sub(lambda _m: text, source)
    else:
        anchor = source.index('    <Metadata Title="WipEout Omega Collection" Name="Native 4K rendering')
        source = source[:anchor] + text + source[anchor:]
    xml.write_text(source, encoding='utf-8', newline='\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('eboot')
    parser.add_argument('width', type=int)
    parser.add_argument('height', type=int)
    parser.add_argument('--name', help='the built-in name of the size (4K), else WIDTHxHEIGHT')
    parser.add_argument('--xml', type=Path, default=XML)
    parser.add_argument('--write', action='store_true', help='put the entry into --xml')
    args = parser.parse_args()
    image, _text = load(args.eboot)
    text = entry(image, args.xml, args.width, args.height, args.name)
    if args.write:
        write(args.xml, args.name or f'{args.width}x{args.height}', text)
    else:
        print(text, end='')


if __name__ == '__main__':
    main()
