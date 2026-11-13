import os
import shutil
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

# Add bindings directory to sys.path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "bindings" / "python"))
import purpledb


class TestPurpleDB(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.mkdtemp(prefix="purpledb_test_")

    def tearDown(self):
        if os.path.exists(self.temp_dir):
            shutil.rmtree(self.temp_dir, ignore_errors=True)

    def test_open_close_lifecycle(self):
        db = purpledb.open(self.temp_dir, dim=32)
        self.assertFalse(db.is_closed)
        db.close()
        self.assertTrue(db.is_closed)

        # Context manager test
        with purpledb.open(self.temp_dir, dim=32) as ctx_db:
            self.assertFalse(ctx_db.is_closed)
        self.assertTrue(ctx_db.is_closed)

    def test_add_and_search_moments(self):
        with purpledb.open(self.temp_dir, dim=32) as db:
            moments_count = db.add_synthetic_video("vid_sunset", count=15, fps=1.0)
            self.assertGreater(moments_count, 0)

            hits = db.search("golden sunset at the beach", top_k=5)
            self.assertGreater(len(hits), 0)
            self.assertLessEqual(len(hits), 5)

            hit = hits[0]
            self.assertEqual(hit.video_id, "vid_sunset")
            self.assertGreaterEqual(hit.start_sec, 0.0)
            self.assertGreater(hit.end_sec, hit.start_sec)
            self.assertEqual(hit.duration, hit.end_sec - hit.start_sec)
            self.assertIsInstance(hit.score, float)

            # Test dict conversion and string representation
            d = hit.to_dict()
            self.assertEqual(d["video_id"], "vid_sunset")
            self.assertEqual(d["duration"], hit.duration)
            self.assertIn("MomentHit", repr(hit))

    def test_temporal_and_video_filters(self):
        with purpledb.open(self.temp_dir, dim=32) as db:
            db.add_synthetic_video("vid_forest", count=12, fps=1.0)
            db.add_synthetic_video("vid_desert", count=12, fps=1.0)

            # Restrict to vid_forest
            forest_hits = db.search("tall green pine trees", top_k=10, video_id="vid_forest")
            for h in forest_hits:
                self.assertEqual(h.video_id, "vid_forest")

            # Temporal filtering
            windowed_hits = db.search(
                "dune sand wind",
                top_k=10,
                video_id="vid_desert",
                start_sec=2.0,
                end_sec=6.0,
            )
            for h in windowed_hits:
                self.assertEqual(h.video_id, "vid_desert")
                self.assertTrue(h.start_sec <= 6.0 and h.end_sec >= 2.0)

    def test_visual_search_by_image(self):
        with purpledb.open(self.temp_dir, dim=32) as db:
            db.add_synthetic_video("vid_visual", count=8, fps=1.0)

            # 224x224 RGB image buffer
            width, height, channels = 224, 224, 3
            synthetic_rgb = bytes([100] * (width * height * channels))

            hits = db.search_by_image(synthetic_rgb, width=width, height=height, channels=channels, top_k=3)
            self.assertGreater(len(hits), 0)
            self.assertLessEqual(len(hits), 3)
            for h in hits:
                self.assertEqual(h.video_id, "vid_visual")

    def test_stats_and_delete_cascade(self):
        with purpledb.open(self.temp_dir, dim=32) as db:
            db.add_synthetic_video("vid_temp", count=10, fps=1.0)

            st = db.stats()
            self.assertEqual(st.num_videos, 1)
            self.assertGreater(st.num_moments, 0)
            self.assertGreater(st.num_vectors, 0)
            self.assertIn("DatabaseStats", repr(st))
            self.assertEqual(st.to_dict()["num_videos"], 1)

            # Cascade delete
            db.delete_video("vid_temp")
            st_after = db.stats()
            self.assertEqual(st_after.num_videos, 0)
            self.assertEqual(st_after.num_moments, 0)
            self.assertEqual(st_after.num_vectors, 0)

            # Ensure queries return 0 hits for deleted video
            hits = db.search("temp moment", top_k=5, video_id="vid_temp")
            self.assertEqual(len(hits), 0)

    def test_concurrent_readers_during_ingest(self):
        with purpledb.open(self.temp_dir, dim=32) as db:
            db.add_synthetic_video("seed_video", count=10, fps=1.0)

            stop_event = threading.Event()
            read_count = [0]
            errors = []

            def reader_worker():
                while not stop_event.is_set():
                    try:
                        hits = db.search("scene query", top_k=3)
                        read_count[0] += 1
                    except Exception as exc:
                        errors.append(exc)
                    time.sleep(0.001)

            threads = [threading.Thread(target=reader_worker) for _ in range(4)]
            for t in threads:
                t.start()

            # Concurrent video ingestion in main thread
            for i in range(4):
                db.add_synthetic_video(f"stream_vid_{i}", count=5, fps=1.0)
                time.sleep(0.005)

            stop_event.set()
            for t in threads:
                t.join()

            self.assertEqual(len(errors), 0, f"Concurrent reader errors: {errors}")
            self.assertGreater(read_count[0], 0)

    def test_error_handling(self):
        db = purpledb.open(self.temp_dir, dim=32)
        db.close()

        # Calling methods on closed database raises RuntimeError
        with self.assertRaises(RuntimeError):
            db.search("query")

        with self.assertRaises(RuntimeError):
            db.stats()

        # Invalid buffer size raises ValueError
        with purpledb.open(self.temp_dir, dim=32) as db2:
            tiny_buffer = bytes([0] * 10)
            with self.assertRaises(ValueError):
                db2.search_by_image(tiny_buffer, width=224, height=224, channels=3)


if __name__ == "__main__":
    unittest.main()
