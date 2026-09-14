# DLive provider scripts

Copy both `dlive.py` and `get_all_players.py` into the server's scripts directory.
Dependencies: `requests` and `beautifulsoup4`. Point the provider at `dlive.py`.

Manifest resolution runs players in parallel. Fast mode returns one second
after the first usable HLS URL is resolved, including alternatives completed
in that interval. It still uses the full deadline if no HLS URL is found.
This avoids waiting for unrelated slow players; it cannot fix a refused
connection to the channel directory or guarantee a resolved URL is playable.

Stream script parameters:

- `id=51`: channel ID; fast mode is the default.
- `id=51 fast=0`: wait for every player, preserving the full CDN list.
- `id=51 cdn_grace=2`: give alternatives two seconds after the first result.
- `id=51 player=4`: resolve only Player 4.

Existing `timeout`, `player_timeout`, `connect_timeout`, `read_timeout`, and
network/session arguments remain supported. No session URLs are cached, so a
manifest refresh always resolves fresh URLs.
