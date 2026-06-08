"""Extract SNK_CreditsText (24-byte-stride array) from English Language.dll and
emit a C array literal + the photo-letter set. One-shot helper for the Extras
credits-scroll port."""
import pefile, io

pe = pefile.PE('original/Language/English/Language.dll')
img = pe.get_memory_mapped_image()
rva = 0x7ad0
# bound by the gap to the next export RVA (the array's true byte size)
rvas = sorted(s.address for s in pe.DIRECTORY_ENTRY_EXPORT.symbols if s.address)
nxt = min((r for r in rvas if r > rva), default=rva + 24 * 280)
count = (nxt - rva) // 24
print('next export rva', hex(nxt), 'byte size', nxt - rva, 'entries', count)
entries = []
for i in range(count):
    e = img[rva + i * 24: rva + i * 24 + 24]
    z = e.split(b'\x00')[0]
    entries.append(z.decode('latin-1', 'replace'))
# trim trailing empties
while entries and entries[-1].strip() == '':
    entries.pop()
print('total entries:', len(entries))

def esc(s):
    return s.replace('\\', '\\\\').replace('"', '\\"')

buf = io.StringIO()
buf.write('static const char *const k_credits[] = {\n')
line = '    '
for idx, s in enumerate(entries):
    tok = '"%s",' % esc(s)
    if len(line) + len(tok) > 110:
        buf.write(line.rstrip() + '\n')
        line = '    '
    line += tok + ' '
buf.write(line.rstrip() + '\n')
buf.write('};\n')
buf.write('#define K_CREDITS_COUNT %d\n' % len(entries))
open('re/analysis/frontend_layout/credits_array.txt', 'w').write(buf.getvalue())
print('wrote credits_array.txt')

letters = sorted({s[1] for s in entries if s.startswith('#') and len(s) > 1})
print('photo letters:', letters)
# letter -> mugshot: orig indexes (0x4961dc + letter*4); the table at 0x4962e0 is
# loaded Bob,Gareth,Snake,MikeT,Chris,Headley,Steve,Rich,Mike,Bez,Les,TonyP,JohnS,
# DavidT,TonyC,DaveyB,ChrisD,Slade,Matt,Marie,JFK,Daz (order of LoadTga calls).
# (0x4962e0 - 0x4961dc)/4 = 0x41 = 'A', so 'A'->slot0=Bob, 'B'->Gareth, ...
load_order = ['Bob','Gareth','Snake','MikeT','Chris','Headley','Steve','Rich','Mike','Bez',
              'Les','TonyP','JohnS','DavidT','TonyC','DaveyB','ChrisD','Slade','Matt','Marie','JFK','Daz']
print('letter -> mugshot:')
for L in letters:
    slot = ord(L) - ord('A')
    name = load_order[slot] if 0 <= slot < len(load_order) else '???'
    print('  %s -> slot %d -> %s.tga' % (L, slot, name))
