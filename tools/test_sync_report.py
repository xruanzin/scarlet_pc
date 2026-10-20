#!/usr/bin/env python3
"""Keep wall-time scopes and actual helper barriers distinct from guest flags."""
from pathlib import Path
import tempfile
import unittest

from scarlet_performance_report import summarize


class SyncReportTests(unittest.TestCase):
    def test_overlapping_scopes_and_barrier_windows(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d/'scarlet_boot.log').write_text(
                '[vk-sync-mode] scoped-images=on\n'
                '[sync-prof] inclusive wall times; overlapping scopes: big-lock=1.250ms/7 backend-present=90.000ms/2 present-ticket=110.000ms/2\n'
                '[vk-sync] emitted full=14 scoped-calls=9 image-dependencies=17\n'
                '[vk] t=2 frame 50 [nvn-barrier-flags 0:61440] rdiscard 8320/17000\n'
                '[sync-prof] inclusive wall times; overlapping scopes: big-lock=2.000ms/5 backend-present=70.000ms/2 present-ticket=85.000ms/2\n'
                '[vk-sync] emitted full=12 scoped-calls=8 image-dependencies=15\n')
            result = summarize(d)
            self.assertEqual(result['synchronization']['big-lock'], {'wall_ms': 3.25, 'calls': 12})
            self.assertEqual(result['synchronization']['present-ticket']['wall_ms'], 195)
            self.assertEqual(result['vulkan_barriers'],
                             {'full': 26, 'scoped_calls': 17, 'image_dependencies': 32, 'windows': 2})
            report = (d/'report.md').read_text()
            self.assertIn('nao some as linhas', report)
            self.assertIn('nao mede passes jogados fora', report)
            timeline = (d/'renderer-timeline.txt').read_text()
            self.assertIn('[sync-prof]', timeline)
            self.assertIn('[vk-sync]', timeline)
            self.assertIn('[vk-sync-mode]', timeline)

    def test_old_capture_does_not_turn_guest_flags_into_vulkan_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            (d/'scarlet_boot.log').write_text('[vk] t=2 frame 50 [barrier 0:61440]\n')
            result = summarize(d)
            self.assertEqual(result['synchronization'], {})
            self.assertIsNone(result['vulkan_barriers'])
            self.assertIn('Nao equivale a tempo zero', (d/'report.md').read_text())


if __name__ == '__main__': unittest.main(verbosity=2)
