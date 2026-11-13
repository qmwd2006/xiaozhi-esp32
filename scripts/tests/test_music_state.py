"""Exercise the music playlist index/repeat logic on the host.

Application::StartMusicAt / HandleMusicFinished / NextMusic / PrevMusic carry the
playlist state machine (index-based, non-consuming) that is easy to get wrong and
hard to notice until a real device is playing. The interesting parts are plain
integer arithmetic, so we compile a tiny harness that mirrors the exact
expressions and assert the wrap/advance semantics documented in AGENTS.md.

The arithmetic is duplicated here on purpose: the surrounding methods depend on
HTTP/AudioService and cannot be linked on the host. Keep the expressions in sync
with main/application.cc; the source-scan tests below guard against drift.
"""

import pathlib
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
APPLICATION = (ROOT / "main/application.cc").read_text()
PLAYER_CC = (ROOT / "main/music/music_player.cc").read_text()


def definition(source, signature):
    start = source.index(signature)
    brace = source.index("{", start)
    depth, end = 1, brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class MusicIndexMathTests(unittest.TestCase):
    """Pure arithmetic: run the extracted logic against the host compiler."""

    def compile_and_run(self, body, check):
        compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("requires a host C++ compiler")
        program = f"""
#include <cassert>
#include <deque>
#include <string>
#include <vector>

// index normalization used by StartMusicAt (list loop wraps via modulo).
static int normalize(int index, int n) {{
    if (n <= 0) return -1;
    if (index < 0) index = (index % n + n) % n;
    else if (index >= n) index %= n;
    return index;
}}

// advance used by HandleMusicFinished.
enum Repeat {{ kOff = 0, kList = 1, kOne = 2 }};
static int advance(int index, int n, Repeat mode) {{
    if (n <= 0) return -1;
    if (mode == kOne) return index;
    if (mode == kList) return normalize(index + 1, n);
    // kOff: stop at end.
    if (index + 1 >= n) return -1;
    return index + 1;
}}

// next/prev index math used by NextMusic/PrevMusic deferred branches.
static int next_index(int index, int n) {{
    return n > 0 ? (index + 1) % n : -1;
}}
static int prev_index(int index, int n) {{
    return n > 0 ? ((index - 1) % n + n) % n : -1;
}}

int main() {{
{body}
    return 0;
}}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "music_state.cc"
            source.write_text(program, encoding="utf-8")
            binary = pathlib.Path(directory) / "music_state"
            compiled = subprocess.run(
                [compiler, "-std=c++17", str(source), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_normalize_wraps_both_directions(self):
        self.compile_and_run(
            """
    assert(normalize(0, 5) == 0);
    assert(normalize(4, 5) == 4);
    assert(normalize(5, 5) == 0);
    assert(normalize(6, 5) == 1);
    assert(normalize(-1, 5) == 4);
    assert(normalize(-2, 5) == 3);
    assert(normalize(-6, 5) == 4);
    assert(normalize(3, 0) == -1);
""",
            None,
        )

    def test_repeat_one_stays_on_current(self):
        self.compile_and_run(
            """
    assert(advance(2, 5, kOne) == 2);
    assert(advance(0, 1, kOne) == 0);
""",
            None,
        )

    def test_repeat_list_loops_back_to_first(self):
        self.compile_and_run(
            """
    assert(advance(0, 5, kList) == 1);
    assert(advance(3, 5, kList) == 4);
    assert(advance(4, 5, kList) == 0);  // wrap, list not consumed
    assert(advance(0, 1, kList) == 0);  // single song loops on itself
""",
            None,
        )

    def test_repeat_off_stops_at_last(self):
        self.compile_and_run(
            """
    assert(advance(0, 5, kOff) == 1);
    assert(advance(3, 5, kOff) == 4);
    assert(advance(4, 5, kOff) == -1);  // past last => stop
    assert(advance(0, 1, kOff) == -1);  // single song ends
""",
            None,
        )

    def test_next_and_prev_wrap(self):
        self.compile_and_run(
            """
    assert(next_index(0, 5) == 1);
    assert(next_index(4, 5) == 0);
    assert(prev_index(0, 5) == 4);
    assert(prev_index(4, 5) == 3);
    assert(prev_index(0, 1) == 0);
    assert(next_index(0, 0) == -1);
    assert(prev_index(0, 0) == -1);
""",
            None,
        )


class ResolveMusicTitleTests(unittest.TestCase):
    """Compile the real ResolveMusicTitle to verify title fallback behaviour."""

    def compile_and_assert(self, body):
        compiler = shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
        if compiler is None:
            self.skipTest("requires a host C++ compiler")
        fn = definition(APPLICATION, "std::string Application::ResolveMusicTitle(")
        # Strip the class qualifier so the extracted body compiles standalone.
        fn = fn.replace("std::string Application::ResolveMusicTitle(",
                        "std::string ResolveMusicTitle(", 1)
        program = f"""
#include <cassert>
#include <string>
{fn}
int main() {{
{body}
    return 0;
}}
"""
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / "title.cc"
            source.write_text(program, encoding="utf-8")
            binary = pathlib.Path(directory) / "title"
            compiled = subprocess.run(
                [compiler, "-std=c++17", str(source), "-o", str(binary)],
                capture_output=True,
                text=True,
            )
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_explicit_title_wins(self):
        self.compile_and_assert(
            """
    assert(ResolveMusicTitle("http://h/library/BV1x.mp3", "情歌 - 周深") == "情歌 - 周深");
""")

    def test_fallback_strips_path_and_extension(self):
        self.compile_and_assert(
            """
    assert(ResolveMusicTitle("http://h/library/BV17g411o73F.mp3", "") == "BV17g411o73F");
    assert(ResolveMusicTitle("http://h/a/b/song", "") == "song");
    assert(ResolveMusicTitle("plain", "") == "plain");
    assert(ResolveMusicTitle("http://h/.hidden", "") == ".hidden");
""")

    def test_titles_synced_with_queue_in_set_queue(self):
        body = definition(APPLICATION, "bool Application::SetMusicQueue(")
        self.assertIn("music_titles_ = std::move(url_titles);", body)

    def test_play_url_keeps_titles_in_sync(self):
        body = definition(APPLICATION, "bool Application::PlayMusicNow(")
        self.assertIn("music_titles_.push_front(title);", body)
        self.assertIn("music_titles_.erase(", body)

    def test_status_reports_title(self):
        self.assertIn('cJSON_AddStringToObject(json, "title", app.GetCurrentMusicTitle().c_str());',
                      (ROOT / "main/mcp_server.cc").read_text())


class MusicStateSourceGuardTests(unittest.TestCase):
    """Guard the mirrored arithmetic against silent drift in application.cc."""

    def test_start_music_at_normalizes_with_modulo(self):
        body = definition(APPLICATION, "bool Application::StartMusicAt(int index)")
        self.assertIn("index %= n;", body)
        self.assertIn("(index % n + n) % n", body)

    def test_handle_music_finished_repeat_modes(self):
        body = definition(APPLICATION, "void Application::HandleMusicFinished(")
        self.assertIn("MusicRepeatMode::kOne", body)
        self.assertIn("next_index = music_index_;", body)
        self.assertIn("next_index = music_index_ + 1;", body)
        self.assertIn("music_index_ + 1 >= static_cast<int>(music_queue_.size())", body)

    def test_next_music_defers_with_pending_during_dialog(self):
        body = definition(APPLICATION, "void Application::NextMusic()")
        # Deferred branch must register a pending request so playback starts as
        # soon as the device leaves listening/speaking, not only on natural idle.
        self.assertIn("pending_music_url_ = music_queue_[music_index_];", body)
        self.assertIn("ArmPendingMusicRetry();", body)
        self.assertIn("(music_index_ + 1) %", body)

    def test_prev_music_defers_with_pending_during_dialog(self):
        body = definition(APPLICATION, "void Application::PrevMusic()")
        self.assertIn("pending_music_url_ = music_queue_[music_index_];", body)
        self.assertIn("ArmPendingMusicRetry();", body)

    def test_resume_after_stop_registers_pending_when_not_idle(self):
        body = definition(APPLICATION, "void Application::ResumeMusic()")
        # Deferred resume must register pending (same index) instead of silently
        # giving up, otherwise "接着放" only starts on a later idle transition
        # and may pick a different song.
        self.assertIn("pending_music_url_ = music_queue_[music_index_];", body)
        self.assertIn("ArmPendingMusicRetry();", body)

    def test_try_start_switches_idle_before_busy_guard(self):
        body = definition(APPLICATION, "void Application::TryStartPendingMusic()")
        listening = body.index("state == kDeviceStateListening")
        busy_guard = body.index("music_player_.IsBusy() || notify_player_.IsBusy()")
        # The listening->idle switch must run before the player-busy guard,
        # otherwise a just-stopped worker can block the switch for tens of seconds.
        self.assertLess(listening, busy_guard)

    def test_pending_retry_timer_is_periodic(self):
        body = definition(APPLICATION, "void Application::ArmPendingMusicRetry()")
        match = re.search(r"esp_timer_start_periodic\([^,]+,\s*(\d+)\s*\*\s*1000\)", body)
        self.assertIsNotNone(match, "expected a periodic retry timer in ms")
        self.assertLessEqual(int(match.group(1)), 500, "retry interval should be short")

    def test_player_seek_defers_until_bitrate_known(self):
        body = definition(PLAYER_CC, "bool MusicPlayer::Seek(int seconds)")
        self.assertIn("pending_seek_seconds_ = seconds;", body)
        self.assertIn("bitrate_bps_", body)

    def test_player_position_and_duration_from_bitrate(self):
        position = definition(PLAYER_CC, "int MusicPlayer::PositionSeconds() const")
        duration = definition(PLAYER_CC, "int MusicPlayer::DurationSeconds() const")
        self.assertIn("current_byte_ * 8 / bitrate_bps_", position)
        self.assertIn("total_bytes_ * 8 / bitrate_bps_", duration)


if __name__ == "__main__":
    unittest.main()
