import tempfile
from pathlib import Path
import unittest
from scarlet_performance_report import summarize

class ReportTests(unittest.TestCase):
    def test_missing_data_is_not_zero_cost(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertNotIn('frames',summarize(d))
            text=(Path(d)/'report.md').read_text()
            self.assertIn('Sem frames validos',text)
            self.assertIn('indisponiveis',text)

    def test_percentiles_hle_and_truncated_frame(self):
        with tempfile.TemporaryDirectory() as d:
            p=Path(d)
            (p/'frames.csv').write_text('frame,host_ms,frame_ms,backend_ms,gpu_wait_ms,present_ms,other_ms,draws,submits,shader_jobs\n1,100,10,5,2,1,4,20,2,0\n2,130,30,15,8,2,13,40,3,1\n3,')
            (p/'scarlet_boot.log').write_text('[hle-prof] 12.500 ms 5 calls wait\n[hle-prof] 7.500 ms 3 calls wait\n[gpuprof] 5 marks\nFATAL synthetic\n')
            s=summarize(d)['frames']
            self.assertEqual(s['count'],2)
            self.assertEqual(s['fps_over_intervals'],50)
            self.assertEqual(s['median_ms'],20)
            self.assertAlmostEqual(s['p95_ms'],29)
            self.assertIn('|wait|20.000|8|',(p/'report.md').read_text())
            self.assertIn('FATAL synthetic',(p/'alerts.txt').read_text())

if __name__=='__main__': unittest.main()
