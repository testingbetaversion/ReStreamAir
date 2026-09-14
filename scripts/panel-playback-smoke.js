#!/usr/bin/env node
// Verify the URL used by panel players/copy actions never loses credentials
// or leaks a playback key to an external source.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const source = fs.readFileSync(path.join(__dirname, '../public/app.js'), 'utf8');
const start = source.indexOf('function rememberPlaybackKey(');
const end = source.indexOf('// Copy the stream\'s HLS', start);
assert(start >= 0 && end > start);
const first = { id: 'first', label: 'Living room', key: 'fixture key+&' };
const state = { providers: [{ id: 'provider' }], apiKeys: [first] };
const stored = new Map();
const location = { origin: 'http://localhost:8787' };
const { playbackUrl, streamPlaybackUrl, xtreamPlaylistUrl, applyKeyList, selectedPlaybackKey, selectPlaybackUser, counts } = vm.runInNewContext(`
  let selectedPlaybackKeyId, stateMutationEpoch = 0, reloads = 0, renders = 0;
  const render = () => renders++;
  ${source.slice(start, end)}
  reloadPlaybackForUser = () => reloads++;
  ({ playbackUrl, streamPlaybackUrl, xtreamPlaylistUrl, applyKeyList, selectedPlaybackKey, selectPlaybackUser, counts: () => ({reloads, renders}) })`, {
  state, URL, URLSearchParams, location,
  localStorage: { getItem: (key) => stored.get(key), setItem: (key, value) => stored.set(key, value) },
});
for (const route of ['play/channel/index.m3u8', 'restream/channel/seg.ts', 'direct/channel', 'download/channel.mp4', 'source/channel']) {
  const url = new URL(playbackUrl(`/${route}?variant=video%2Fhigh.m3u8`));
  assert.equal(url.searchParams.get('key'), 'fixture key+&');
  assert.equal(url.searchParams.get('variant'), 'video/high.m3u8');
}
assert.equal(new URL(playbackUrl('/play/channel/index.m3u8?key=stale')).searchParams.get('key'), first.key);
assert.equal(playbackUrl('https://source.example/live.m3u8'), 'https://source.example/live.m3u8');
assert.equal(playbackUrl('http://localhost:8787/api/state'), 'http://localhost:8787/api/state');
assert.equal(playbackUrl(''), '');
const second = { id: 'second', label: 'Bedroom + TV', key: 'second-secret' };
const providers = state.providers;
applyKeyList({ keys: [first, second] }, true);
assert.equal(state.providers, providers, 'key creation must preserve the rest of panel state');
assert.equal(selectedPlaybackKey().id, 'second');
assert.equal(new URL(streamPlaybackUrl({ id: 'channel' })).searchParams.get('key'), second.key);
const playlist = new URL(xtreamPlaylistUrl(second));
assert.equal(playlist.origin, location.origin);
assert.equal(playlist.searchParams.get('username'), second.label);
assert.equal(playlist.searchParams.get('password'), second.key);
assert.equal(playlist.searchParams.get('output'), 'm3u8');
selectPlaybackUser('first');
assert.equal(new URL(playbackUrl('/play/channel/index.m3u8?key=second-secret')).searchParams.get('key'), first.key);
assert.equal(stored.get('restreamair-playback-key-id'), 'first');
applyKeyList({ keys: [second] });
assert.equal(selectedPlaybackKey().id, 'second', 'revoking the selected user must choose a valid remaining key');
location.origin = 'https://localhost:8787';
assert.equal(new URL(playbackUrl('http://localhost:8787/play/channel/index.m3u8')).protocol, 'https:');
assert.equal(playbackUrl('http://elsewhere.example/play/channel/index.m3u8'), 'http://elsewhere.example/play/channel/index.m3u8');
applyKeyList({ keys: [] });
assert.equal(counts().reloads, 4, 'active playback is reloaded after selection or credential changes');
location.origin = 'http://localhost:8787';
assert.equal(playbackUrl('/play/channel/index.m3u8'), 'http://localhost:8787/play/channel/index.m3u8');
console.log('panel-playback-smoke: selected user, key mutations, complete URLs and Xtream playlist credentials PASS');
