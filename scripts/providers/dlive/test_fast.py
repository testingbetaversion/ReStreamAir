"""Small, offline timing check for fast manifest resolution."""
import time
from unittest.mock import patch
import get_all_players as resolver

players = [{"name": f"Player {i}", "player_url": f"https://fixture.example/{i}"} for i in range(3)]


def resolve(player, *args, **kwargs):
    index = int(player["player_url"].rsplit("/", 1)[-1])
    time.sleep((0.01, 0.02, 0.35)[index])
    return {**player, "streams": [{"type": "hls", "url": f"https://fixture.example/{index}.m3u8"}]}


with patch.object(resolver, "resolve_player", resolve):
    started = time.monotonic()
    fast = resolver.collect_players(players, "https://fixture.example", None, started + 1, 1, (1, 1), True, 0.05)
    elapsed_fast = time.monotonic() - started
    assert sum(bool(p["streams"]) for p in fast) == 2
    assert fast[2]["status"] == "skipped"
    started = time.monotonic()
    full = resolver.collect_players(players, "https://fixture.example", None, started + 1, 1, (1, 1))
    elapsed_full = time.monotonic() - started
    assert sum(bool(p["streams"]) for p in full) == 3
    assert elapsed_fast < elapsed_full / 2, (elapsed_fast, elapsed_full)

with patch.object(resolver, "resolve_player", lambda player, *a, **kw: {**player, "streams": [], "error": "fixture failure"}):
    result = resolver.collect_players(players, "https://fixture.example", None, time.monotonic() + 1, 1, (1, 1), True, 0.05)
    assert all(p.get("error") == "fixture failure" for p in result)
print(f"fast resolver: {elapsed_fast:.3f}s vs full {elapsed_full:.3f}s in offline fixture; failure handling PASS")
