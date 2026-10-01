import copy
import contextlib
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools'))
import sample_menu_white as probe


class Memory:
    base = 0x180000000

    def __init__(self):
        self.data = {}; self.collections = 0; self.mutate_second = None; self.reads = []
        self.put(self.base + 0x13DCF48, '<Q', 0x100000)
        for front in (False, True):
            layout, root, leaf, source = ((0x300000, 0x500000, 0x700000, 0x900000) if front else
                                         (0x200000, 0x400000, 0x600000, 0x800000))
            index, group, display, count, resource = (2, 5, 1, 3, 2956) if front else (0, 4, 0, 2, 2986)
            self.put(0x100000 + (0x120 if front else 0x128), '<Q', layout)
            self.put(layout, '<Q', self.base + 0x10C9FA8)
            self.put(layout + 0x18, '<2I', 2, 1)
            self.put(layout + 0x3C90, '<I', display); self.put(layout + 0x3C98, '<I', count)
            self.put(layout + 0x64 + 4 * index, '<I', resource)
            self.put(layout + 0x368, '<32s', b'Root')
            self.put(layout + 0x3F8 + 4 * index, '<I', group)
            self.put(layout + 0x428 + 8 * index, '<Q', root)
            self.put(layout + 0x50, '<3f', 1, 1, 1)
            self.node(root, b'Root', group, (5, 5) if front else (20, 20), root=True)
            self.node(leaf, b'white', group, (968, 1566) if front else (1000, 1480))
            self.put(root + 0x18, '<Q', leaf); self.put(leaf + 0x10, '<Q', root)
            self.put(leaf + 0x60, '<2f', -100, -100)
            self.put(leaf + 0x70, '<2f', 564.685, -45.066)
            self.source(leaf, source, source + 0x600, source + 0x700, source + 0x800,
                        selector=2 if front else 0, key=0x1111, color=0xFFFFFFFF)
        self.put(self.base + 0x13DCF58, '<Q', 0xA00000)
        self.put(0xA00000, '<Q', self.base + 0x10C5FA8); self.put(0xA01373, '<B', 1)
        self.put(self.base + 0x13D8E98, '<Q', 0xB00000)
        self.put(0xB00000, '<Q', self.base + 0x10C07A8)
        self.put(0xB00030, '<I', 34); self.put(0xB00048, '<I', 1)
        self.put(0xB00028, '<Q', 0xC00000); self.put(0xC00000, '<Q', self.base + 0x10CD1E8)
        self.put(0xC00580, '<4f', 960, 540, 607.2035, 1080)
        self.put(self.base + 0x12E24E8, '<Q', 0xD00000)
        self.put(0xD00000, '<Q', self.base + 0x703E78); self.put(0xD00028, '<2H', 1920, 1080)
        self.set_scene()

    def node(self, pointer, name, group, size, root=False):
        self.block(pointer, 0x130)
        self.put(pointer, '<2Q', self.base + (0x10CEB48 if root else 0x10CEBA8), pointer)
        self.put(pointer + 0x3C, '<I', group); self.put(pointer + 0x40, '<32s', name)
        self.put(pointer + 0x98, '<2f', *size); self.put(pointer + 0xA0, '<4f', 1, 1, 1, 1)
        self.put(pointer + 0xC9, '<B', 1)

    def source(self, leaf, source, material, texture, record, selector=0, key=0x5553455200055180, color=0x7FFFFFFF):
        self.put(leaf + 0x110, '<Q', source); self.put(leaf + 0x30, '<Q', record)
        self.block(record, 0x48); self.put(record, '<Q', self.base + 0x10CEC08)
        self.put(record + 0x40, '<Q', source); self.put(record + 8, '<I', color)
        self.block(source, 0x580); self.put(source, '<Q', self.base + 0x10CC0D8)
        self.put(source + 0x570, '<I', selector); self.put(source + 0x1A8, '<Q', 0xE00000 + selector * 0x800)
        self.put(source + 0x308, '<Q', material); self.block(material, 0x60); self.block(texture, 0x68)
        self.put(material, '<Q', self.base + 0x706570); self.put(material + 0x50, '<Q', texture)
        self.put(texture, '<Q', key); self.put(texture + 0x18, '<2H', 64, 64)

    def set_scene(self, scene_id=1, primary=1, secondary=12, pending=-1, actual_vtable=None):
        manager, child = 0xF00000, 0x1000000
        self.put(self.base + 0x13DD000, '<Q', manager); self.block(manager, 0x48)
        self.put(manager, '<Q', self.base + 0x10C6B38)
        self.put(manager + 0x38, '<iiQ', scene_id, pending, child)
        expected = probe.SCENE_TYPES.get(scene_id)
        self.put(child, '<Q', self.base + (actual_vtable if actual_vtable is not None else expected[1] if expected else 0x123456))
        # Deliberately leave all nonmenu +10 fields unreadable.
        for offset in range(0x10, 0x18): self.data.pop(child + offset, None)
        if scene_id in (1, 2) and (actual_vtable is None or actual_vtable == expected[1]):
            self.put(child + 0x10, '<II', primary, secondary)

    def add_mask(self):
        layout, root, top, bottom = 0x200000, 0x1100000, 0x1200000, 0x1300000
        self.put(layout + 0x68, '<I', 0xBF6); self.put(layout + 0x3FC, '<I', 6); self.put(layout + 0x430, '<Q', root)
        self.node(root, b'Root', 6, (5, 5), root=True)
        self.node(top, b'Top', 6, (850, -64)); self.node(bottom, b'Btm', 6, (850, 64))
        self.put(root + 0x18, '<Q', top); self.put(top + 0x28, '<Q', bottom); self.put(bottom + 0x20, '<Q', top)
        for i, leaf in enumerate((top, bottom)):
            self.put(leaf + 0x10, '<Q', root); self.put(leaf + 0x60, '<2f', 0, -45 if i == 0 else 1261)
            self.put(leaf + 0xA8, '<2f', .79062957, .79062957)
            self.source(leaf, leaf + 0x1000, leaf + 0x2000, leaf + 0x3000, leaf + 0x4000)

    def add_lifecycle(self, scene_id=3, primary=2, secondary=0, child_primary=76, child_secondary=0):
        self.set_scene(scene_id)
        owner, child = 0x1000000, 0x1A00000
        self.block(owner + 8, 0x18)
        self.put(owner + 0x10, '<IIQ', primary, secondary, child)
        self.block(child, 0x18)
        self.put(child, '<Q', self.base + probe.LIFECYCLE_CHILDREN[scene_id][1])
        self.put(child + 0x10, '<II', child_primary, child_secondary)

    def add_first_credit(self, primary=1, secondary=3, with_pass_wait=False, child=0x1B00000):
        self.add_lifecycle(4, 1, 8, 1, 5)
        if not with_pass_wait: self.put(0x1000018, '<Q', 0)
        self.put(0x1000048, '<Q', child)
        self.block(child, 0x18)
        self.put(child, '<Q', self.base + probe.FIRST_CREDIT_VTABLE)
        self.put(child + 0x10, '<II', primary, secondary)

    def add_quarter(self, order=('BG', 'BG2', 'BG3', 'White')):
        self.set_scene(2, 1, 12)
        layout, root = 0x1600000, 0x1700000
        self.put(0x1000020, '<Q', layout)
        self.block(layout, 0x80); self.put(layout, '<Q', self.base + 0x10C9FA8)
        self.put(layout + 0x18, '<2I', 2, 1); self.put(layout + 0x58, '<f', 1)
        self.put(layout + 0x64, '<I', 0xBA0)
        self.block(layout + 0x3C90, 12); self.put(layout + 0x3C98, '<I', 1)
        self.block(layout + 0x3F8, 0x38); self.put(layout + 0x3F8, '<I', 7)
        self.put(layout + 0x428, '<Q', root)
        self.node(root, b'Root', 7, (5,5), root=True)
        children = [0x1800000 + index * 0x1000 for index in range(len(order))]
        self.put(root + 0x18, '<Q', children[0])
        for index, (name, child) in enumerate(zip(order, children)):
            self.node(child, name.encode(), 7, (800,1480) if name == 'White' else (800,969))
            self.put(child + 0x10, '<Q', root)
            self.put(child + 0x20, '<Q', children[index-1] if index else 0)
            self.put(child + 0x28, '<Q', children[index+1] if index + 1 < len(children) else 0)
            self.put(child + 0x60, '<2f', 0, -100 if name == 'White' else 88)
            if name == 'White':
                self.white = child
                self.put(child + 0x70, '<2f', 643.748169, -45.066)
                self.put(child + 0xA8, '<2f', .790629566, .790629566)
                self.put(child + 0xB8, '<2f', 1, 1)
                self.source(child, 0x1900000, 0x1901000, 0x1902000, 0x1903000,
                            selector=0, key=0x123456789, color=0xFFFFFFFF)
                self.put(0x1902018, '<2H', 2048,2048)
                self.block(0x1903048, 0x30)
                u0,v0,u1,v1 = probe.QUARTER_UV_BOUNDS
                self.put(0x1903058, '<8f', u0,v0,u0,v1,u1,v1,u1,v0)
        self.put(self.base + 0x13DB5A0, '<Q', 0xE00000)
        self.put(0xE00000, '<Q', self.base + 0x6FDF48)

    def add_first_credit_quarter(self, with_pass_wait=False, order=('BG', 'BG2', 'White')):
        self.add_quarter(order)
        self.add_first_credit(with_pass_wait=with_pass_wait)
        self.put(0x1B00020, '<Q', 0x1600000)
        # Trap the different scene-owned MainMenu+20 path: FirstCredit must
        # follow the nominated child, not this stale/reused scene offset.
        self.put(0x1000020, '<Q', 0xDEAD0020)

    def block(self, addr, count): self.data.update({addr + i: 0 for i in range(count)})
    def put(self, addr, fmt, *values): self.data.update(dict(enumerate(struct.pack(fmt, *values), addr)))
    def tick(self): return 1234
    def read(self, addr, count):
        self.reads.append((addr, count))
        if addr == self.base + 0x13DCF48:
            self.collections += 1
            if self.collections == 2 and self.mutate_second: self.mutate_second(self)
        try: return bytes(self.data[addr + i] for i in range(count))
        except KeyError: raise OSError('unreadable fixture')


class Tests(unittest.TestCase):
    def test_exact_layers_read_only_and_bounded(self):
        r = Memory(); before = copy.deepcopy(r.data); row = probe.sample(r)
        self.assertTrue(row['accepted'], row); self.assertTrue(row['dynamic_fields_equal'])
        self.assertEqual(row['before']['layers']['main']['resource'], 2986)
        self.assertEqual(row['before']['layers']['front']['source']['display_selector_570'], 2)
        self.assertEqual(before, r.data); self.assertLess(row['read_calls'], probe.MAX_READS)
        self.assertLess(row['read_bytes'], probe.MAX_BYTES)

    def test_exact_owner_role_links_resource_rejection(self):
        for addr, fmt, v in ((0x200000, '<Q', 1), (0x200018, '<I', 1), (0x20001C, '<I', 0),
                             (0x203C90, '<I', 1), (0x203C98, '<I', 3), (0x200064, '<I', 2956),
                             (0x200368, '<B', 88), (0x400000, '<Q', 1), (0x400008, '<Q', 1),
                             (0x400010, '<Q', 1), (0x600010, '<Q', 0x500000), (0x600018, '<Q', 1),
                             (0x600020, '<Q', 1), (0x600028, '<Q', 1), (0x600040, '<B', 88),
                             (0x60003C, '<I', 5), (0x600098, '<f', 800), (0x600060, '<f', 0),
                             (0x800570, '<I', 2), (0xA01373, '<B', 0), (0xD00028, '<H', 800),
                             (0x800800, '<Q', 1), (0x800840, '<Q', 0x900000)):
            with self.subTest(addr=hex(addr)):
                r = Memory(); r.put(addr, fmt, v); self.assertFalse(probe.sample(r)['accepted'])

    def test_malformed_pose_flags_metadata_rejected(self):
        for addr, fmt, v in ((0x600070, '<f', float('nan')), (0x6000C9, '<B', 2),
                             (0xC00588, '<f', 0), (0x200058, '<f', float('inf')), (0x6000BC, '<f', float('nan'))):
            r = Memory(); r.put(addr, fmt, v); self.assertFalse(probe.sample(r)['accepted'])

    def test_repeated_resource_identity_rejects(self):
        r = Memory(); r.mutate_second = lambda m: m.put(0x800700, '<Q', 0x12345)
        self.assertFalse(probe.sample(r)['accepted'])

    def test_visibility_pose_transition_reported_not_atomic(self):
        r = Memory(); r.mutate_second = lambda m: m.put(0x6000C9, '<B', 0); row = probe.sample(r)
        self.assertTrue(row['accepted'], row); self.assertFalse(row['dynamic_fields_equal'])
        self.assertEqual(row['before']['layers']['main']['leaf']['visible'], 1)
        self.assertEqual(row['after']['layers']['main']['leaf']['visible'], 0)

    def test_rotated_pose_omits_unproven_axis_aligned_bounds(self):
        r = Memory(); r.put(0x6000B4, '<f', 10); row = probe.sample(r)
        self.assertTrue(row['accepted'], row); self.assertFalse(row['before']['layers']['main']['zero_pivot_rotation'])
        self.assertIsNone(row['before']['layers']['main']['axis_aligned_cached_bounds_minus_point_one'])
        self.assertFalse(row['before']['layers']['main']['source_camera_link_verified'])

    def test_process_unavailable_reported(self):
        r = Memory(); r.data.clear(); self.assertFalse(probe.sample(r)['accepted'])

    def test_mask_fixed_two_children_metadata_bounds(self):
        r = Memory(); r.add_mask(); before = copy.deepcopy(r.data); row = probe.sample(r, True)
        self.assertTrue(row['accepted'], row); mask = row['before']['ranking_mask']
        self.assertEqual(mask['leaves']['Top']['leaf']['size'], [850, -64])
        self.assertEqual(mask['leaves']['Btm']['commit_record_color_argb'], '0x7fffffff')
        self.assertFalse(mask['leaves']['Top']['source_camera_link_verified']); self.assertEqual(before, r.data)
        self.assertLess(row['read_calls'], probe.MAX_READS); self.assertLess(row['read_bytes'], probe.MAX_BYTES)

    def test_mask_links_resource_and_source_fail_closed(self):
        for address, fmt, value in ((0x200068, '<I', 0xBAA), (0x1100008, '<Q', 1),
                  (0x1200010, '<Q', 1), (0x1200018, '<Q', 1), (0x1300020, '<Q', 0),
                  (0x1300028, '<Q', 0x1200000), (0x1300040, '<4s', b'Top'),
                  (0x1203000, '<Q', 0x1111), (0x1201570, '<I', 2)):
            with self.subTest(address=hex(address)):
                r = Memory(); r.add_mask(); r.put(address, fmt, value); self.assertFalse(probe.sample(r, True)['accepted'])

    def test_mask_repeated_identity_and_dynamic_tuple(self):
        r = Memory(); r.add_mask(); r.mutate_second = lambda m: m.put(0x12011A8, '<Q', 0xF00000)
        self.assertFalse(probe.sample(r, True)['accepted'])
        r = Memory(); r.add_mask(); r.mutate_second = lambda m: m.put(0x12000C9, '<B', 0)
        row = probe.sample(r, True); self.assertTrue(row['accepted'], row); self.assertFalse(row['dynamic_fields_equal'])

    def test_scene_title_and_mainmenu_exact_headers(self):
        for scene_id, state in ((1, 12), (2, 18)):
            r = Memory(); r.set_scene(scene_id, 1, state); r.add_mask(); before = copy.deepcopy(r.data); row = probe.sample(r, True)
            self.assertTrue(row['accepted'], row); self.assertTrue(row['scene_header_repeated'])
            self.assertTrue(row['repeated_settled_scene_owner']); scene = row['before']['scene']
            self.assertTrue(scene['type_verified']); self.assertEqual(scene['current_id'], scene_id)
            self.assertEqual(scene['menu_state']['secondary_14'], state)
            self.assertFalse(scene['menu_state']['screen_name_inferred']); self.assertEqual(before, r.data)
            self.assertLess(row['read_calls'], probe.MAX_READS); self.assertLess(row['read_bytes'], probe.MAX_BYTES)

    def test_nonmenu_types_never_read_menu_fields(self):
        for scene_id in (3, 4, 5, 6, 7, 9):
            r = Memory(); r.set_scene(scene_id); row = probe.sample(r)
            self.assertTrue(row['accepted'], row); self.assertTrue(row['scene_owner_type_verified'])
            self.assertIsNone(row['after']['scene']['menu_state'])
            self.assertEqual(row['after']['scene']['expected_type'], probe.SCENE_TYPES[scene_id][0])

    def test_unmapped_mismatched_and_null_child_are_unresolved_not_menu(self):
        for scene_id, wrong in ((99, None), (1, 0x10C6E48), (2, 0x10C7278), (5, 0x10C7030)):
            r = Memory(); r.set_scene(scene_id, actual_vtable=wrong); row = probe.sample(r)
            self.assertTrue(row['accepted'], row); self.assertFalse(row['scene_owner_type_verified'])
            self.assertFalse(row['repeated_settled_scene_owner']); self.assertIsNone(row['after']['scene']['menu_state'])
        r = Memory(); r.put(0xF00040, '<Q', 0); row = probe.sample(r)
        self.assertTrue(row['accepted'], row); self.assertEqual(row['after']['scene']['type_status'], 'no_active_child')

    def test_pending_and_substate_transition_are_dynamic_metadata(self):
        r = Memory(); r.mutate_second = lambda m: m.put(0xF0003C, '<i', 2); row = probe.sample(r)
        self.assertTrue(row['accepted'], row); self.assertFalse(row['scene_header_repeated'])
        self.assertFalse(row['repeated_settled_scene_owner'])
        self.assertEqual(row['before']['scene']['pending_id'], -1); self.assertEqual(row['after']['scene']['pending_id'], 2)
        r = Memory(); r.mutate_second = lambda m: m.put(0x1000014, '<I', 13); row = probe.sample(r)
        self.assertTrue(row['accepted'], row); self.assertFalse(row['scene_header_repeated'])
        self.assertTrue(row['repeated_settled_scene_owner'])
        self.assertEqual(row['after']['scene']['menu_state']['secondary_14'], 13)

    def test_changed_scene_owner_is_rejected(self):
        r = Memory(); r.mutate_second = lambda m: m.set_scene(2, 1, 2); self.assertFalse(probe.sample(r)['accepted'])
        r = Memory(); r.put(0xF00000, '<Q', 0); self.assertFalse(probe.sample(r)['accepted'])

    def test_raw_unknown_menu_state_not_given_screen_name(self):
        r = Memory(); r.set_scene(2, 1, 0xFFFFFFFF); row = probe.sample(r)
        self.assertTrue(row['accepted'], row)
        self.assertFalse(row['after']['scene']['menu_state']['active_secondary_in_proven_range'])
        self.assertEqual(row['after']['scene']['menu_state']['secondary_14'], 0xFFFFFFFF)

    def test_operator_screen_label_validation(self):
        self.assertEqual(probe.screen_label('Normal Start'), 'Normal Start')
        self.assertEqual(probe.screen_label('Story / \u9078\u629e'), 'Story / \u9078\u629e')
        for label in ('', 'x' * 121, 'title\nmenu', 'test\0', 'hide\x1b'):
            with self.assertRaises(probe.argparse.ArgumentTypeError): probe.screen_label(label)

    def test_quarter_exact_chain_record_uv_and_combined_read_budget(self):
        for mask in (False, True):
            r=Memory(); r.add_quarter()
            if mask: r.add_mask()
            before=copy.deepcopy(r.data); row=probe.sample(r, mask, True)
            self.assertTrue(row['accepted'], row)
            quarter=row['before']['main_quarter']
            self.assertEqual(set(quarter['leaves']), {'BG','BG2','BG3','White'})
            self.assertEqual(quarter['commit_record_alpha_byte'], 255)
            self.assertTrue(quarter['record_uv_matches_known_part_corners'])
            self.assertTrue(quarter['source_camera_link_verified'])
            self.assertFalse(quarter['texture_key_static_verified'])
            self.assertFalse(quarter['record_is_current_frame_submission_proven'])
            self.assertAlmostEqual(quarter['axis_aligned_cached_bounds_minus_point_one'][0], 643.648169, places=4)
            self.assertEqual(before,r.data)
            self.assertLessEqual(row['read_calls'],probe.MAX_READS)
            self.assertLessEqual(row['read_bytes'],probe.MAX_BYTES)
            if mask: self.assertEqual(row['read_calls'],256)

    def test_quarter_wrong_scene_type_pending_or_state_refuses_before_traversal(self):
        for scene_id,primary,secondary,pending,actual in (
                (1,1,12,-1,None),(5,1,0,-1,None),(2,0,12,-1,None),(2,2,12,-1,None),
                (2,1,19,-1,None),(2,1,12,2,None),(2,1,12,-1,0x10C7278)):
            r=Memory();r.add_quarter();r.set_scene(scene_id,primary,secondary,pending,actual)
            r.data.pop(0x1000020,None)
            row=probe.sample(r,False,True)
            self.assertFalse(row['accepted']);self.assertIn('settled, active',row['error'])

    def test_quarter_permuted_siblings_allowed_unknown_duplicate_extra_rejected(self):
        r=Memory();r.add_quarter(('White','BG3','BG','BG2'))
        self.assertTrue(probe.sample(r,False,True)['accepted'])
        for address,fmt,value in ((0x1801040,'<4s',b'BG\0'),(0x1801040,'<6s',b'Other\0'),
                (0x1803028,'<Q',0x1800000),(0x1801010,'<Q',0),
                (0x1801020,'<Q',0),(0x1802018,'<Q',0x1800000)):
            r=Memory();r.add_quarter();r.put(address,fmt,value)
            self.assertFalse(probe.sample(r,False,True)['accepted'])

    def test_quarter_layout_source_route_and_geometry_fail_closed(self):
        for address,fmt,value in ((0x1600000,'<Q',0),(0x1600018,'<I',1),(0x160001C,'<I',0),
                (0x1600064,'<I',0xBAA),(0x1603C90,'<I',1),(0x1603C98,'<I',4),
                (0x1700008,'<Q',0),(0x1700098,'<f',20),(0x1803098,'<f',1000),
                (0x1803060,'<f',1),(0x18030A0,'<f',2),(0x1900000,'<Q',0),
                (0x1900570,'<I',2),(0x1902018,'<H',64),(0x1903000,'<Q',0),
                (0x1903040,'<Q',0),(0x19001A8,'<Q',0xE00800),(0xE00000,'<Q',0)):
            with self.subTest(address=hex(address)):
                r=Memory();r.add_quarter();r.put(address,fmt,value)
                self.assertFalse(probe.sample(r,False,True)['accepted'])

    def test_quarter_nonfinite_metadata_and_invalid_uv_rejected(self):
        for address,fmt,value in ((0x1600058,'<f',float('nan')),(0x17000BC,'<f',float('inf')),
                (0x18030BC,'<f',float('nan')),(0x1903058,'<f',float('nan')),
                (0x1903058,'<f',-1),(0x1903058,'<f',2)):
            r=Memory();r.add_quarter();r.put(address,fmt,value)
            self.assertFalse(probe.sample(r,False,True)['accepted'])

    def test_quarter_dynamic_alpha_uv_and_substate_are_reported_not_certified_draw(self):
        r=Memory();r.add_quarter()
        def changed(m):
            m.put(m.white+0xBC,'<f',0.5);m.put(0x1903008,'<I',0x7FFFFFFF)
            m.put(0x1903058,'<f',0.0);m.put(0x1000014,'<I',17)
        r.mutate_second=changed;row=probe.sample(r,False,True)
        self.assertTrue(row['accepted'],row);self.assertFalse(row['dynamic_fields_equal'])
        self.assertFalse(row['scene_header_repeated'])
        self.assertEqual(row['before']['main_quarter']['commit_record_alpha_byte'],255)
        self.assertEqual(row['after']['main_quarter']['commit_record_alpha_byte'],127)
        self.assertTrue(row['before']['main_quarter']['record_uv_matches_known_part_corners'])
        self.assertFalse(row['after']['main_quarter']['record_uv_matches_known_part_corners'])

    def test_quarter_changed_descriptor_or_departing_scene_rejected(self):
        for change in (lambda m:m.put(0x1902000,'<Q',0x999),
                       lambda m:m.put(0x1000010,'<I',2),
                       lambda m:m.put(0xF0003C,'<i',1)):
            r=Memory();r.add_quarter();r.mutate_second=change
            self.assertFalse(probe.sample(r,False,True)['accepted'])

    def test_deadline_count_spacing_and_slow_reads(self):
        for cost in (0, 0.8):
            now = [0.0]; starts = []
            def clock(): return now[0]
            def sleep(x): now[0] += x
            def sample(_):
                starts.append(now[0]); now[0] += cost; return {'accepted': True}
            with patch.object(probe, 'sample', sample): rows = probe.capture_rows(None, clock=clock, sleep=sleep)
            self.assertLessEqual(len(rows), 60); self.assertTrue(all(x < 30 for x in starts))
            self.assertTrue(all(b - a >= 0.5 for a, b in zip(starts, starts[1:])))

    def test_lifecycle_options_exact_owned_type_fields_and_budget(self):
        r = Memory(); r.add_lifecycle(); r.add_mask(); before = copy.deepcopy(r.data)
        row = probe.sample(r, True, False, True)
        self.assertTrue(row['accepted'], row); self.assertTrue(row['menu_lifecycle_repeated'])
        value = row['before']['menu_lifecycle']
        self.assertEqual(value['status'], 'verified')
        self.assertEqual(value['owner_state'], {'stop_08': 0, 'done_09': 0, 'primary_10': 2})
        self.assertEqual(value['owned_child']['expected_type'], 'CWindOnOff')
        self.assertEqual(value['owned_child']['state'], {'stop_08': 0, 'done_09': 0, 'primary_10': 76})
        self.assertFalse(value['screen_name_inferred']); self.assertFalse(value['safe_mutation_phase_proven'])
        self.assertEqual(r.data, before); self.assertLessEqual(row['read_calls'], probe.MAX_READS)
        self.assertLessEqual(row['read_bytes'], probe.MAX_BYTES)

    def test_lifecycle_eamusement_has_separate_scene_and_child_substates(self):
        r = Memory(); r.add_lifecycle(4, 1, 0, 1, 5)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row)
        value = row['before']['menu_lifecycle']
        self.assertEqual(value['owner_state']['secondary_14'], 0)
        self.assertEqual(value['owned_child']['state']['secondary_14'], 5)
        self.assertEqual(value['owned_child']['expected_type'], 'CEamuPassWait')
        self.assertIsNone(row['before']['scene']['menu_state'])

    def test_lifecycle_unsupported_and_mismatched_scenes_do_not_read_fields(self):
        for scene_id, wrong in ((1, None), (2, None), (5, None), (7, None), (99, None),
                                 (3, 0x10C6E48), (4, 0x10C7278)):
            r = Memory(); r.set_scene(scene_id, actual_vtable=wrong)
            # Unsupported owners have no +18 storage; any traversal would fail.
            row = probe.sample(r, include_lifecycle=True)
            self.assertTrue(row['accepted'], row)
            value = row['before']['menu_lifecycle']
            self.assertIsNone(value['owner_state']); self.assertIsNone(value['owned_child'])
            self.assertEqual(value['status'], 'scene_type_unresolved' if scene_id in (3, 4) else 'unsupported_scene')

    def test_lifecycle_null_and_wrong_type_child_are_unresolved_without_field_reads(self):
        r = Memory(); r.add_lifecycle(); r.put(0x1000018, '<Q', 0)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row)
        self.assertEqual(row['before']['menu_lifecycle']['status'], 'no_owned_child')
        r = Memory(); r.add_lifecycle(4); r.put(0x1A00000, '<Q', r.base + 0x10CA6F0)
        for offset in range(8, 0x18): r.data.pop(0x1A00000 + offset)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row)
        value = row['before']['menu_lifecycle']
        self.assertEqual(value['status'], 'owned_child_type_mismatch')
        self.assertIsNone(value['owned_child']['state'])

    def test_lifecycle_pending_stop_phase_changes_are_retained_dynamic_metadata(self):
        r = Memory(); r.add_lifecycle(4, 1, 0, 1, 5)
        def changed(m):
            m.put(0xF0003C, '<i', 3)
            m.put(0x1000008, '<B', 1); m.put(0x1000014, '<I', 1)
            m.put(0x1A00009, '<B', 1); m.put(0x1A00010, '<II', 2, 0)
        r.mutate_second = changed
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row); self.assertFalse(row['menu_lifecycle_repeated'])
        self.assertFalse(row['dynamic_fields_equal']); self.assertFalse(row['repeated_settled_scene_owner'])
        self.assertEqual(row['before']['menu_lifecycle']['owned_child']['state']['primary_10'], 1)
        self.assertEqual(row['after']['menu_lifecycle']['owned_child']['state']['primary_10'], 2)

    def test_lifecycle_published_destroyed_or_replaced_child_rejects_pair(self):
        for change in (lambda m: m.put(0x1000018, '<Q', 0),
                       lambda m: m.put(0x1A00000, '<Q', m.base + 0x10CA7E0)):
            r = Memory(); r.add_lifecycle(); r.mutate_second = change
            self.assertFalse(probe.sample(r, include_lifecycle=True)['accepted'])
        r = Memory(); r.add_lifecycle(); r.put(0x1000018, '<Q', 0)
        r.mutate_second = lambda m: m.put(0x1000018, '<Q', 0x1A00000)
        self.assertFalse(probe.sample(r, include_lifecycle=True)['accepted'])

    def test_lifecycle_invalid_booleans_rejected_unknown_numeric_states_raw(self):
        for address in (0x1000008, 0x1000009, 0x1A00008, 0x1A00009):
            r = Memory(); r.add_lifecycle(); r.put(address, '<B', 2)
            self.assertFalse(probe.sample(r, include_lifecycle=True)['accepted'])
        r = Memory(); r.add_lifecycle(4, 0xFFFFFFFF, 0xABC, 0xFFFFFFFF, 0xDEF)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row)
        self.assertEqual(row['before']['menu_lifecycle']['owned_child']['state']['primary_10'], 0xFFFFFFFF)
        self.assertFalse(row['before']['menu_lifecycle']['safe_mutation_phase_proven'])

    def test_lifecycle_and_quarter_refused_before_open_or_reads(self):
        r = Memory(); row = probe.sample(r, include_quarter=True, include_lifecycle=True)
        self.assertFalse(row['accepted']); self.assertEqual(row['read_calls'], 0)
        with patch('sys.argv', ['probe', '--pid', '1', '--exe-sha256', '00', '--include-main-quarter', '--include-menu-lifecycle']), \
                patch.object(probe, 'ReadOnlyProcess') as reader, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as raised: probe.main()
            self.assertEqual(raised.exception.code, 2)
            reader.assert_not_called()

    def test_lifecycle_batch_forwards_flags_without_extending_schedule(self):
        now = [0.0]; calls = []
        def sample(reader, *flags):
            calls.append((now[0], flags)); now[0] += .1; return {'accepted': True}
        def sleep(value): now[0] += value
        with patch.object(probe, 'sample', sample):
            rows = probe.capture_rows(None, include_mask=True, include_lifecycle=True, clock=lambda: now[0], sleep=sleep)
        self.assertEqual(len(rows), 60)
        self.assertTrue(all(flags == (True, False, True) for _, flags in calls))
        self.assertTrue(all(b[0]-a[0] >= .5 for a, b in zip(calls, calls[1:])))

    def test_first_credit_keeps_null_pass_wait_separate_and_never_writes(self):
        r = Memory(); r.add_first_credit(); before = copy.deepcopy(r.data)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row); self.assertTrue(row['menu_lifecycle_repeated'])
        value = row['before']['menu_lifecycle']
        self.assertEqual(value['status'], 'no_owned_child')
        self.assertEqual(value['owned_child']['expected_type'], 'CEamuPassWait')
        self.assertEqual(value['owned_child']['pointer'], '0x0')
        first = value['first_credit_child']
        self.assertEqual(first['owner_offset'], '0x48'); self.assertEqual(first['status'], 'verified')
        self.assertEqual(first['expected_type'], 'CEamuFirstCredit')
        self.assertEqual(first['state'], {'stop_08': 0, 'done_09': 0, 'primary_10': 1, 'secondary_14': 3})
        self.assertFalse(value['safe_mutation_phase_proven']); self.assertFalse(value['screen_name_inferred'])
        self.assertEqual(r.data, before)

    def test_first_credit_both_named_children_and_mask_stay_bounded(self):
        r = Memory(); r.add_first_credit(with_pass_wait=True); r.add_mask()
        row = probe.sample(r, include_mask=True, include_lifecycle=True)
        self.assertTrue(row['accepted'], row); value = row['before']['menu_lifecycle']
        self.assertEqual(value['owned_child']['expected_type'], 'CEamuPassWait')
        self.assertEqual(value['owned_child']['state']['secondary_14'], 5)
        self.assertEqual(value['first_credit_child']['state']['secondary_14'], 3)
        self.assertEqual(row['read_calls'], 214); self.assertEqual(row['read_bytes'], 7642)
        self.assertLessEqual(row['read_calls'], probe.MAX_READS); self.assertLessEqual(row['read_bytes'], probe.MAX_BYTES)

    def test_first_credit_unsupported_roles_do_not_read_plus48(self):
        for scene_id in (1, 2, 3, 5, 7, 99):
            r = Memory()
            if scene_id == 3: r.add_lifecycle()
            else: r.set_scene(scene_id)
            row = probe.sample(r, include_lifecycle=True)
            self.assertTrue(row['accepted'], row)
            self.assertEqual(row['before']['menu_lifecycle']['first_credit_child']['status'], 'not_nominated_owner_role')
            self.assertFalse(any(address <= 0x1000048 < address + count for address, count in r.reads))
        for primary, secondary in ((0, 8), (2, 8), (1, 0), (1, 7), (1, 9), (1, 0xFFFFFFFF)):
            r = Memory(); r.add_lifecycle(4, primary, secondary)
            row = probe.sample(r, include_lifecycle=True)
            self.assertTrue(row['accepted'], row)
            self.assertFalse(any(address <= 0x1000048 < address + count for address, count in r.reads))
        r = Memory(); r.set_scene(4, actual_vtable=0x10C7308)
        self.assertTrue(probe.sample(r, include_lifecycle=True)['accepted'])
        self.assertFalse(any(address <= 0x1000048 < address + count for address, count in r.reads))

    def test_first_credit_null_wrong_type_and_unreadable_child(self):
        r = Memory(); r.add_first_credit(); r.put(0x1000048, '<Q', 0)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row)
        self.assertEqual(row['before']['menu_lifecycle']['first_credit_child']['status'], 'no_first_credit_child')
        r = Memory(); r.add_first_credit(); r.put(0x1B00000, '<Q', r.base + 0x10CA7E0)
        for offset in range(8, 0x18): r.data.pop(0x1B00000 + offset)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row)
        first = row['before']['menu_lifecycle']['first_credit_child']
        self.assertEqual(first['status'], 'first_credit_type_mismatch'); self.assertIsNone(first['state'])
        self.assertEqual([n for a,n in r.reads if a == 0x1B00000], [8, 8])
        r = Memory(); r.add_first_credit(); r.put(0x1000048, '<Q', 0xDEAD0000)
        self.assertFalse(probe.sample(r, include_lifecycle=True)['accepted'])

    def test_first_credit_prefix_ends_at_unreadable_boundary(self):
        r = Memory(); child = 0x1B00FE8; r.add_first_credit(child=child)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row)
        self.assertEqual([n for a,n in r.reads if a == child], [8, 0x18, 8, 0x18])
        self.assertNotIn(child + 0x18, r.data)

    def test_first_credit_publication_destruction_reuse_and_role_change_reject_pair(self):
        for change in (lambda m: m.put(0x1000048, '<Q', 0),
                       lambda m: m.put(0x1000048, '<Q', 0x1B01000),
                       lambda m: m.put(0x1B00000, '<Q', m.base + 0x10CA7E0),
                       lambda m: m.put(0x1000014, '<I', 9)):
            r = Memory(); r.add_first_credit()
            for offset in range(0x18): r.data[0x1B01000 + offset] = r.data[0x1B00000 + offset]
            r.mutate_second = change
            self.assertFalse(probe.sample(r, include_lifecycle=True)['accepted'])
        r = Memory(); r.add_first_credit(); r.put(0x1000048, '<Q', 0)
        r.mutate_second = lambda m: m.put(0x1000048, '<Q', 0x1B00000)
        self.assertFalse(probe.sample(r, include_lifecycle=True)['accepted'])

    def test_first_credit_phase_stop_and_pending_changes_are_dynamic_only(self):
        r = Memory(); r.add_first_credit()
        def changed(m):
            m.put(0xF0003C, '<i', 3); m.put(0x1000008, '<B', 1)
            m.put(0x1B00008, '<2B', 1, 1); m.put(0x1B00010, '<2I', 2, 7)
        r.mutate_second = changed
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row); self.assertFalse(row['menu_lifecycle_repeated'])
        self.assertFalse(row['repeated_settled_scene_owner'])
        self.assertEqual(row['before']['menu_lifecycle']['first_credit_child']['state']['secondary_14'], 3)
        self.assertEqual(row['after']['menu_lifecycle']['first_credit_child']['state']['secondary_14'], 7)
        self.assertFalse(row['after']['menu_lifecycle']['safe_mutation_phase_proven'])

    def test_first_credit_bad_booleans_reject_and_unknown_phase_remains_raw(self):
        for address in (0x1B00008, 0x1B00009):
            r = Memory(); r.add_first_credit(); r.put(address, '<B', 2)
            self.assertFalse(probe.sample(r, include_lifecycle=True)['accepted'])
        r = Memory(); r.add_first_credit(primary=0xFFFFFFFF, secondary=0xABC)
        row = probe.sample(r, include_lifecycle=True)
        self.assertTrue(row['accepted'], row)
        self.assertEqual(row['before']['menu_lifecycle']['first_credit_child']['state']['secondary_14'], 0xABC)

    def test_first_credit_disabled_lifecycle_does_not_read_new_path(self):
        r = Memory(); r.add_first_credit(); r.data.pop(0x1000048)
        row = probe.sample(r)
        self.assertTrue(row['accepted'], row); self.assertNotIn('menu_lifecycle', row['before'])
        self.assertFalse(any(address <= 0x1000048 < address + count for address, count in r.reads))



    def test_first_credit_quarter_distinct_owned_layout_and_read_only_budget(self):
        r=Memory();r.add_first_credit_quarter(with_pass_wait=True);before=copy.deepcopy(r.data)
        row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
        self.assertTrue(row['accepted'],row);self.assertEqual(before,r.data)
        q=row['before']['first_credit_quarter']
        self.assertEqual(q['parent'],'0x1000000');self.assertEqual(q['first_credit'],'0x1b00000')
        self.assertEqual(q['status'],'verified');self.assertEqual(q['layout']['owner'],'0x1b00000')
        self.assertEqual(q['layout']['layout'],'0x1600000')
        self.assertTrue(q['layout']['source_camera_link_verified'])
        self.assertTrue(q['layout']['record_uv_matches_known_part_corners'])
        self.assertFalse(q['safe_mutation_phase_proven'])
        self.assertFalse(q['record_is_current_frame_submission_proven'])
        self.assertNotIn((0x1000020,8),r.reads)
        self.assertLessEqual(row['read_calls'],probe.MAX_READS)
        self.assertLess(row['read_bytes'],probe.MAX_BYTES)

    def test_first_credit_quarter_inactive_phases_do_not_read_child_layout(self):
        for primary,secondary in ((0,0),(1,1),(1,2),(1,4),(1,5),(2,3),(99,99)):
            r=Memory();r.add_first_credit(primary=primary,secondary=secondary)
            row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
            self.assertTrue(row['accepted'],row)
            self.assertEqual(row['before']['first_credit_quarter']['status'],'not_nominated_lifecycle')
            self.assertNotIn((0x1B00020,8),r.reads)

    def test_first_credit_quarter_stop_pending_and_type_unresolved_skip_layout(self):
        for address,fmt,value in ((0x1000008,'<B',1),(0x1000009,'<B',1),
                                  (0x1B00008,'<B',1),(0x1B00009,'<B',1),(0xF0003C,'<i',2),
                                  (0x1B00000,'<Q',Memory.base+0x10CA7E0)):
            r=Memory();r.add_first_credit();r.put(address,fmt,value)
            row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
            self.assertTrue(row['accepted'],row)
            self.assertIsNone(row['before']['first_credit_quarter']['layout'])
            self.assertNotIn((0x1B00020,8),r.reads)

    def test_first_credit_quarter_does_not_read_options_or_other_scene_layouts(self):
        for scene in (1,2,3,5):
            r=Memory()
            if scene==3:r.add_lifecycle(3)
            else:r.set_scene(scene)
            row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
            self.assertTrue(row['accepted'],row)
            self.assertIsNone(row['before']['first_credit_quarter']['layout'])
            self.assertNotIn((0x1000020,8),r.reads)

    def test_first_credit_quarter_exact_layout_and_source_guards(self):
        for address,fmt,value in ((0x1B00020,'<Q',0),(0x1600000,'<Q',Memory.base+0x10CA7A8),
                (0x1600064,'<I',0xBAA),(0x1603C90,'<I',1),(0x1603C98,'<I',2),
                (0x1900570,'<I',2),(0x1902018,'<H',64),(0x1903040,'<Q',0x1900008),
                (Memory.base+0x13DB5A0,'<Q',0xE00008)):
            r=Memory();r.add_first_credit_quarter();r.put(address,fmt,value)
            self.assertFalse(probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)['accepted'])

    def test_first_credit_quarter_exact_known_sibling_chain(self):
        for address,value in ((0x1802028,0x1800000),(0x1802010,0x1700008),(0x1801020,0)):
            r=Memory();r.add_first_credit_quarter();r.put(address,'<Q',value)
            self.assertFalse(probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)['accepted'])

    def test_first_credit_quarter_owner_replacement_phase_exit_and_texture_change_reject(self):
        for mutate in (lambda m:m.put(0x1000048,'<Q',0),lambda m:m.put(0x1B00014,'<I',4),
                       lambda m:m.put(0x1B00020,'<Q',0),lambda m:m.put(0x1902000,'<Q',0xAABBCC),
                       lambda m:m.put(0x1000000,'<Q',m.base+0x10C7278)):
            r=Memory();r.add_first_credit_quarter();r.mutate_second=mutate
            self.assertFalse(probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)['accepted'])

    def test_first_credit_quarter_native_alpha_pose_and_uv_changes_remain_metadata(self):
        r=Memory();r.add_first_credit_quarter()
        def mutate(m):
            m.put(m.white+0xBC,'<f',.25);m.put(m.white+0x70,'<f',640)
            m.put(0x1903008,'<I',0x3FFFFFFF);m.put(0x1903058,'<f',.25)
        r.mutate_second=mutate
        row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
        self.assertTrue(row['accepted'],row);self.assertFalse(row['dynamic_fields_equal'])
        self.assertEqual(row['after']['first_credit_quarter']['layout']['commit_record_alpha_byte'],63)
        self.assertFalse(row['after']['first_credit_quarter']['layout']['record_uv_matches_known_part_corners'])

    def test_first_credit_quarter_nonzero_pivot_reports_no_bounds(self):
        r=Memory();r.add_first_credit_quarter();r.put(r.white+0x90,'<f',1)
        row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
        self.assertTrue(row['accepted'],row)
        self.assertIsNone(row['before']['first_credit_quarter']['layout']['axis_aligned_cached_bounds_minus_point_one'])

    def test_first_credit_quarter_flag_conflicts_before_open_or_reads(self):
        for options in (dict(),dict(include_mask=True,include_lifecycle=True),dict(include_quarter=True,include_lifecycle=True)):
            row=probe.sample(Memory(),include_first_credit_quarter=True,**options)
            self.assertFalse(row['accepted']);self.assertEqual(row['read_calls'],0)
        for flags in ([],['--include-menu-lifecycle','--include-ranking-mask'],['--include-menu-lifecycle','--include-main-quarter']):
            with patch('sys.argv',['probe','--pid','1','--exe-sha256','00','--include-first-credit-quarter']+flags), \
                    patch.object(probe,'ReadOnlyProcess') as reader,contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:probe.main()
                self.assertEqual(error.exception.code,2);reader.assert_not_called()

    def test_first_credit_quarter_batch_keeps_schedule_and_flags(self):
        now=[0.0];calls=[]
        def sample(reader,*flags):calls.append((now[0],flags));now[0]+=.1;return {'accepted':True}
        def sleep(value):now[0]+=value
        with patch.object(probe,'sample',sample):
            rows=probe.capture_rows(None,include_lifecycle=True,include_first_credit_quarter=True,clock=lambda:now[0],sleep=sleep)
        self.assertEqual(len(rows),60);self.assertTrue(all(f==(False,False,True,True) for _,f in calls))
        self.assertTrue(all(b[0]-a[0]>=.5 for a,b in zip(calls,calls[1:])))



    def test_first_credit_short_chain_reports_names_without_reading_null(self):
        r=Memory();r.add_first_credit_quarter()
        r.put(0x1801028,'<Q',0)  # Missing White remains rejected.
        before=copy.deepcopy(r.data)
        row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
        self.assertFalse(row['accepted']);self.assertIn('ends before 3',row['error'])
        context=row['failure_context']
        self.assertEqual(context['stage'],'quarter_child');self.assertEqual(context['child_index'],2)
        self.assertEqual(context['root'],'0x1700000');self.assertEqual(context['group'],7)
        self.assertEqual(context['child_pointer'],'0x0')
        self.assertEqual([c['name'] for c in context['completed_children']],['BG','BG2'])
        self.assertEqual(row['read_calls'],78);self.assertEqual(row['read_bytes'],3241)
        self.assertNotIn((0x40,32),r.reads);self.assertEqual(before,r.data)

    def test_first_credit_bad_third_pointer_reports_exact_failed_request(self):
        r=Memory();r.add_first_credit_quarter();r.put(0x1801028,'<Q',0xDEAD0000)
        row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
        self.assertFalse(row['accepted'])
        self.assertEqual(row['last_read_request'],{'address':'0xdead0040','bytes':32})
        self.assertEqual(row['failure_context']['child_pointer'],'0xdead0000')
        self.assertEqual([c['name'] for c in row['failure_context']['completed_children']],['BG','BG2'])
        self.assertEqual(row['read_calls'],79);self.assertEqual(row['read_bytes'],3273)

    def test_first_credit_exact_three_child_shape_rejects_other_named_chains(self):
        for order in (('BG','BG2','BG3','White'), ('BG','BG2','White','BG3'),
                      ('BG','BG','White'), ('BG','Unknown','White'), ('BG2','BG','White'),
                      ('White','BG2','BG')):
            r=Memory();r.add_first_credit_quarter(order=order)
            row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
            self.assertFalse(row['accepted'], (order,row))

    def test_main_menu_still_requires_four_leaves_and_first_credit_shape_is_explicit(self):
        r=Memory();r.add_quarter(('BG','BG2','White'))
        row=probe.sample(r,include_quarter=True)
        self.assertFalse(row['accepted']);self.assertIn('ends before 4',row['error'])
        r=Memory();r.add_quarter()
        row=probe.sample(r,include_quarter=True)
        self.assertTrue(row['accepted'],row)
        self.assertEqual(row['before']['main_quarter']['expected_child_names'],['BG','BG2','BG3','White'])
        self.assertFalse(row['before']['main_quarter']['child_order_required'])
        for both in (False,True):
            r=Memory();r.add_first_credit_quarter(with_pass_wait=both)
            row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
            self.assertTrue(row['accepted'],row)
            q=row['before']['first_credit_quarter']['layout']
            self.assertEqual(q['expected_child_names'],['BG','BG2','White'])
            self.assertEqual(list(q['leaves']),q['expected_child_names'])
            self.assertTrue(q['child_order_required'])
            self.assertEqual(row['read_calls'],190 if both else 186)
            self.assertEqual(row['read_bytes'],8050 if both else 7986)

    def test_first_credit_new_child_publication_between_collections_rejects(self):
        r=Memory();r.add_first_credit_quarter()
        r.mutate_second=lambda m:m.put(m.white+0x28,'<Q',0xDEAD0000)
        row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
        self.assertFalse(row['accepted']);self.assertIn('exceeds the 3',row['error'])
        self.assertNotIn((0xDEAD0040,32),r.reads)

    def test_failure_context_does_not_leak_previous_collection_quarter_stage(self):
        r=Memory();r.add_first_credit_quarter()
        r.mutate_second=lambda m:m.put(m.base+0x13DCF48,'<Q',0xDEAD0000)
        row=probe.sample(r,include_lifecycle=True,include_first_credit_quarter=True)
        self.assertFalse(row['accepted'])
        self.assertEqual(row['failure_context'],{'stage':'resident_source_collection'})
        self.assertEqual(row['last_read_request'],{'address':'0xdead0128','bytes':8})


if __name__ == '__main__': unittest.main()
