import copy
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import sample_kill_camera as probe

class Memory:
    base = 0x180000000
    def __init__(self):
        self.data = {}
        self.changing = False
        self.collects = 0
        self.owner = 0x100000
        self.put(self.base + 0x13DCF90, '<Q', self.owner)
        self.block(self.owner, 0x238)
        self.put(self.owner, '<Q', self.base + 0x10C6200)
        self.put(self.owner + 0x170, '<I', 7)
        self.put(self.owner + 0x178, '<3Q', 0x200000, 0x201000, 0x202000)
        self.put(self.owner + 0x190, '<3Q', 0x200000, 0x202000, 0x201000)
        self.put(self.owner + 0x22C, '<2f', 768, 300)
        for i, name in enumerate(('base', 'SShotEffect', 'mask')):
            source = 0x300000 + i * 0x1000
            self.node(0x200000 + i * 0x1000, name, 7, source)
            self.put(self.owner + (0x150, 0x158, 0x168)[i], '<Q', source)
            self.block(source, 0x580); self.put(source, '<Q', self.base + 0x10CC0D8)
            material = source + 0x600
            texture = source + 0x700
            self.block(material, 0x60); self.block(texture, 0x68)
            self.put(source + 0x308, '<Q', material)
            self.put(material, '<Q', self.base + 0x706570)
            self.put(material + 0x50, '<Q', texture)
            self.put(texture + 0x18, '<2H', 768, 300)
        self.put(self.base + 0x13DCF58, '<Q', 0x400000)
        self.put(0x401373, '<B', 1)
        self.put(self.base + 0x13D8E98, '<Q', 0x500000)
        self.block(0x500000, 0x50); self.put(0x500000, '<Q', self.base + 0x10C07A8)
        self.put(0x500030, '<I', 34); self.put(0x500048, '<I', 1)
        self.put(0x500028, '<Q', 0x600000); self.put(0x600000, '<Q', self.base + 0x10CD1E8)
        self.put(0x600580, '<4f', 960, 540, 607.2035, 1080)
        self.put(self.base + 0x13DCF78, '<Q', 0x700000)
        self.put(0x700000, '<Q', self.base + 0x10CA298)
        self.put(0x7000D0, '<2Q', 0x800000, 0x900000)
        self.put(0x800000, '<Q', self.base + 0x10CA1D8); self.put(0x900000, '<Q', self.base + 0x10CA200)
        self.put(0x9000F0, '<Q', 0xA00000); self.put(0x900098, '<I', 8)
        self.put(0x800478, '<Q', 0xB00000); self.put(0x8000E0, '<I', 9)
        self.put(0x8004A0, '<fB', 180, 0)
        self.node(0xA00000, 'm_bt_backlight_Nick', 8, 0xDEAD)
        self.node(0xB00000, 'lcd_title_bar', 9, 0xBEEF)
    def node(self, address, name, group, source):
        self.block(address, 0x130)
        self.put(address, '<2Q', self.base + 0x10CEBA8, address)
        self.put(address + 0x3C, '<I', group)
        self.bytes(address + 0x40, name.encode() + b'\0')
        self.put(address + 0x110, '<Q', source)
    def block(self, address, size): self.bytes(address, bytes(size))
    def bytes(self, address, values): self.data.update({address+i: b for i,b in enumerate(values)})
    def put(self, address, fmt, *values): self.bytes(address, struct.pack(fmt, *values))
    def read(self, address, size):
        if address == self.base + 0x13DCF90:
            self.collects += 1
            if self.changing and self.collects == 2: self.put(0x200010, '<Q', 0xCC0000)
        try: return bytes(self.data[address+i] for i in range(size))
        except KeyError: raise OSError('unreadable')
    def u64(self, address): return struct.unpack('<Q', self.read(address, 8))[0]
    def u32(self, address): return struct.unpack('<I', self.read(address, 4))[0]
    def tick(self): return 1000

class Tests(unittest.TestCase):
    def test_readonly_pair_and_both_source_modes(self):
        for height in (300, 1366):
            reader = Memory(); reader.put(reader.owner+0x230, '<f', height)
            before = copy.deepcopy(reader.data)
            row = probe.sample(reader)
            self.assertTrue(row['accepted'], row)
            self.assertEqual(row['nodes']['mask']['name'], 'mask')
            self.assertEqual(reader.data, before)
    def test_wrong_owner_alias_name_and_source_rejected(self):
        for addr,fmt,value in ((0x100000,'<Q',1),(0x100198,'<Q',0x201000),(0x202040,'<B',ord('X')),
                               (0x202110,'<Q',0x301000),(0x302000,'<Q',1),(0x302718,'<H',0)):
            reader=Memory();reader.put(addr,fmt,value)
            self.assertFalse(probe.sample(reader)['accepted'])
    def test_changed_identity_nonfinite_and_bad_mode(self):
        reader=Memory();reader.changing=True
        self.assertFalse(probe.sample(reader)['accepted'])
        for addr,fmt,value in ((0x202070,'<f',float('nan')),(0x100224,'<B',2),(0x100230,'<f',float('inf')),
                               (0x500048,'<I',0),(0x8004A4,'<B',2)):
            reader=Memory();reader.put(addr,fmt,value)
            self.assertFalse(probe.sample(reader)['accepted'])

if __name__ == '__main__': unittest.main()
