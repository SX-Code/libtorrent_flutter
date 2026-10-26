# libtorrent_flutter

Native **libtorrent 2.1.1** bindings for Flutter with a local HTTP streaming server.
This development branch ports the 1.8.7 fork's bounded cache and sliding download
window onto the 2.0.0 source snapshot. It targets iOS, macOS, Android, Linux and Windows.

**Development version: 2.0.1-dev.1.** Native libraries must be rebuilt from this
source. Old 1.8.7 binaries are incompatible with the Dart FFI exports. No default
download from the removed upstream repository is attempted.

Use a local path dependency while validating this branch:

```yaml
dependencies:
  libtorrent_flutter:
    path: /path/to/libtorrent_flutter-2.0.0
```

On a Mac with Xcode, CMake and Ninja installed:

```bash
bash scripts/build_apple.sh all
```

The GitHub Actions workflow builds all five platforms and uploads artifacts; it
does not publish releases. See [artifact installation instructions](prebuilt/README.md)
for local validation setup. All-platform CI and device validation are still pending.

## Why libtorrent?

Every other Flutter torrent package uses either a Java wrapper (Android-only) or a pure-Dart implementation that can't compete on speed. libtorrent 2.1.1 gives you:

- **`set_piece_deadline`** — tells the engine "I need this piece in 150ms", and it picks the fastest peer automatically. Far smarter than simple sequential download.
- **uTP support** — connects to peers behind NAT that other clients can't reach.
- **On-demand streaming** — focuses 100% of startup bandwidth on the first pieces. Container metadata (MP4 moov, MKV cues) is fetched reactively when the player requests it via HTTP range requests, cutting startup time to seconds.
- **DHT + PEX + LSD** — finds peers without trackers.

## Usage

### Initialize

```dart
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

// Call once, before anything else. 
// Saves to system temp dir by default — no permission setup needed on any platform.
await LibtorrentFlutter.init();

// Advanced options:
await LibtorrentFlutter.init(
  downloadLimit: 5 * 1024 * 1024,  // 5 MB/s cap
  uploadLimit:   1 * 1024 * 1024,  // 1 MB/s cap
  defaultSavePath: '/my/custom/path',
  fetchTrackers: true,  // auto-inject best public trackers (default: true)
  pollInterval: Duration(milliseconds: 500),
);
```

### Add a torrent

```dart
final engine = LibtorrentFlutter.instance;

// From a magnet link — save path defaults to system temp
final id = engine.addMagnet('magnet:?xt=urn:btih:...');

// From a magnet link with a custom save path
final id = engine.addMagnet('magnet:?xt=urn:btih:...', '/downloads');

// From a .torrent file
final id = engine.addTorrentFile('/path/to/file.torrent');

// Remove a torrent
engine.removeTorrent(id, deleteFiles: true);

// Pause / resume
engine.pauseTorrent(id);
engine.resumeTorrent(id);
```

### Listen for status updates

```dart
engine.torrentUpdates.listen((Map<int, TorrentInfo> torrents) {
  final t = torrents[id]!;
  print('${t.name}');          // torrent name
  print('${t.state.label}');   // "Downloading", "Seeding", etc.
  print('${t.progress}');      // 0.0 – 1.0
  print('${t.downloadRate}');  // bytes/sec
  print('${t.uploadRate}');    // bytes/sec
  print('${t.numPeers}');
  print('${t.numSeeds}');
  print('${t.hasMetadata}');   // true once file list is available
  print('${t.totalDone}');     // bytes downloaded
  print('${t.totalWanted}');   // bytes total
});

// Or get a snapshot right now
final Map<int, TorrentInfo> current = engine.torrents;
```

### List files and start streaming

```dart
// Wait for hasMetadata == true, then:
final files = engine.getFiles(id);  // List<FileInfo>

for (final f in files) {
  print('[${f.index}] ${f.name}');
  print('  size: ${f.size} bytes');
  print('  streamable: ${f.isStreamable}');
}

// Stream the largest streamable file (auto-selected)
final StreamInfo stream = engine.startStream(id);

// Stream a specific file by index
final StreamInfo stream = engine.startStream(id, fileIndex: 2);

// With a RAM cache limit — useful on mobile
// A 500MB sliding window: the engine evicts old pieces as you watch,
// keeping a 10% safety buffer behind playback so it never stutters.
final StreamInfo stream = engine.startStream(
  id,
  fileIndex: 2,
  maxCacheBytes: 500 * 1024 * 1024,  // 500 MB
);

print(stream.url);  // http://127.0.0.1:PORT/stream/...
// Hand this URL to any player: media_kit, video_player, VLC, mpv, etc.
```

### Monitor stream status

```dart
engine.streamUpdates.listen((Map<int, StreamInfo> streams) {
  for (final s in streams.values) {
    print('url: ${s.url}');
    print('ready: ${s.isReady}');       // HTTP reader readiness, not decoder state
    print('buffer: ${s.bufferPct}%');   // 0–100 buffered ahead
    print('fileSize: ${s.fileSize}');
    print('readHead: ${s.readHead}');   // primary HTTP reader's byte position
  }
});

// Check if a specific torrent is being streamed
final bool active = engine.isStreaming(id);

// Get info for a specific stream
final StreamInfo? info = engine.getStreamInfo(streamId);

// Stop a stream; the last stream removes its torrent and downloaded files.
engine.stopStream(streamId); // keepTorrent defaults to false

// Keep the torrent ID and metadata; streamOnly tasks clear stopped-episode data.
engine.stopStream(streamId, keepTorrent: true);

// Stop all streams, optionally retaining the torrent.
engine.stopAllStreamsForTorrent(torrentId, keepTorrent: true);
```

`readHead` follows the first active HTTP GET; when it ends, another active GET
takes over. It is not the player's decoded playback position. MP4 metadata/index
probes can read different offsets before the first frame. Each GET owns its own
download window; overlapping requests share priorities and use the earliest
deadline. Completing or disconnecting a GET withdraws only that GET's demands.
HEAD returns headers without changing the read position, readiness or downloads.
Range changes no longer emit `seeking`; the server cannot distinguish a container
probe from a user seek. Use the player's events for playback and seek status.

To diagnose startup stalls without changing `media_kit`, enable native HTTP logs
before launching the app. In Xcode, open **Product → Scheme → Edit Scheme → Run →
Arguments → Environment Variables** and set `TB_LOG_FILE=stderr`. The console will
show stream/reader IDs, GET/HEAD, the requested Range, resolved offsets, waits for
missing pieces and completed/interrupted responses. Alternatively, set
`TB_LOG_FILE` to a writable absolute file path (inside the app's sandbox on iOS).
Set it before native initialization; the setting is read once per process.
Logging is disabled when unset. These diagnostics require rebuilding the native
library/XCFramework from the updated source; Flutter hot reload is insufficient.

### Episode switching and upload

`streamOnly` is the third positional argument. Set it to `true` for on-demand
downloading. Use `switchFile` for episode changes to retain only the new episode's
data. Verified pieces can upload while playing, including after download completes.

```dart
final torrentId = engine.addMagnet(magnetUri, null, true);
// Wait for metadata, then select a file from engine.getFiles(torrentId).
final first = engine.startStream(torrentId, fileIndex: firstFileIndex);
// Play first.url. When switching, release the old player first.
final next = engine.switchFile(torrentId, fileIndex: nextFileIndex);
// switchFile stops the old episode and clears its disk data; no stopStream needed.
// Play next.url. On final exit, release the player and clean up:
engine.removeTorrent(torrentId, deleteFiles: true);
```

`startStream` only opens a stream; it never deletes data or stops other streams.
It allows different files to stream simultaneously in both download modes. Their
downloaded data is retained and may accumulate. Use `switchFile` for episode
changes when old streams and data should be cleared.

`switchFile` ends all streams of the specified torrent, deletes its downloaded data (including partial data in libtorrent's partfile),
and resets the owned-piece state before the next episode starts. The torrent ID,
metadata, file list and discovery settings are retained. BT connections reconnect;
the magnet metadata does not need to be downloaded again. After a switch only the
new stream remains. Repeated `switchFile` calls retain the current episode's data,
not ten episodes after ten switches. Repeated `startStream` calls keep old streams
and data instead. BT pieces can straddle file boundaries, so a small amount of adjacent
file data may be needed to verify the current episode's boundary pieces; it is
cleared on the next switch too. This is separate from `maxCacheBytes`, which
limits the RAM cache.

You can also stop before choosing the next episode:

```dart
engine.stopStream(currentStream.id, keepTorrent: true);
// The last stream's data is cleared; only paused metadata/ID remain.
final next = engine.startStream(torrentId, fileIndex: nextFileIndex);
```

Cleanup waits for libtorrent's disk-deletion completion before starting new disk
writes. If deletion fails or exceeds 30 seconds, playback does not start and the
call throws; fix the storage problem and retry `switchFile` with the same ID.
`switchFile` requires an explicit, nonnegative file index and validates it before
stopping the old episode. Even switching to the same file clears downloaded data
and starts a fresh stream. It can also be used for ordinary download torrents;
that explicit call replaces all streams and clears the torrent's downloaded data.
`removeTorrent` remains the final cleanup path and stops all associated streams.
Release player objects separately. Final removal uses libtorrent's asynchronous
deletion; episode switching waits for deletion to finish.

**Compatibility:** `keepTorrent` defaults to `false`; stopping the last stream
removes the entire task/files in both modes. With `streamOnly=false`, explicit
`keepTorrent: true` retains downloaded data and supports different simultaneous
files. With `streamOnly=true`, it now retains metadata only after the last stream
stops; use `switchFile` for bounded episode storage. Retention does not enable all-file
background downloading. Rebuild native libraries with this Dart package: the FFI
entry points are `lt_stop_stream_ex` and `lt_switch_file`; older binaries do not
implement this behavior. Callers of the earlier automatic-switching implementation
must replace episode-changing `startStream` calls with `switchFile`.

### Cleanup

```dart
// Clean up one torrent: stops its streams, removes it, deletes the files
engine.disposeTorrent(id);  // returns false if already gone

// Clean up everything — perfect for your exit button
engine.disposeAll();

// Full shutdown — calls disposeAll(), destroys native session
await engine.dispose();
```

### Speed limits

```dart
engine.setDownloadLimit(2 * 1024 * 1024);  // 2 MB/s
engine.setUploadLimit(512 * 1024);          // 512 KB/s
engine.setDownloadLimit(0);                 // back to unlimited
```

---

## Platform setup

No changes needed on Windows, Linux, and Android — everything is bundled.

**iOS** — add the network entitlement. In your `ios/Runner/Info.plist`, ensure your app allows outgoing connections (this is allowed by default on iOS, but if you use a custom `NetworkExtension` or have restrictive `NSAppTransportSecurity` settings, make sure HTTP to `localhost` is permitted).

**macOS** — add the network entitlement to your `macos/Runner/*.entitlements`:

```xml
<key>com.apple.security.network.client</key>
<true/>
<key>com.apple.security.network.server</key>
<true/>
```

**Android** — add to `AndroidManifest.xml`:

```xml
<uses-permission android:name="android.permission.INTERNET" />
```

**Android background streaming** — Android kills your app's process when it goes to the background unless you use a Foreground Service. When streaming, start one to keep the engine alive:

```dart
// Use flutter_foreground_task or a similar package
FlutterForegroundTask.startService(
  notificationTitle: 'Streaming',
  notificationText: 'Torrent engine running',
);
```

---

## Native artifacts and offline builds

Build with `.github/workflows/build.yml` or the Apple script above. Install the
artifacts at the paths documented in [artifact installation instructions](prebuilt/README.md).
Missing native libraries are downloaded by default from
`https://github.com/SX-Code/libtorrent_flutter/releases/download/v<pubspec-version>`.
For the current version, publish the ZIP assets under tag `v2.0.1-dev.1`.
To use a different release directory, set `LIBTORRENT_FLUTTER_RELEASE_BASE_URL`
to its full URL. Gradle, CMake, CocoaPods and `scripts/download_prebuilt.sh`
all support this override. Existing local libraries are used first by platform builds;
the explicit download script downloads all platforms and replaces existing files.
The artifacts must match this source revision; version naming alone does not
validate ABI compatibility.

Until the matching release is published, supply local artifacts for testing.
Set `LIBTORRENT_FLUTTER_SKIP_DOWNLOAD=1` to disable automatic downloads.
iOS/macOS/Android fail if required artifacts are missing. Linux and Windows
can fall back to CMake using an installed
libtorrent 2.1.1 configured with `deprecated-functions=ON` and `webtorrent=OFF`.
An offline build also requires Flutter packages and native dependencies to have
been downloaded beforehand.

---

## How it works

A single C++ file (`torrent_bridge.cpp`) wraps libtorrent 2.0 and compiles to a native static/shared library on every platform. Dart talks to it via FFI — no platform channels, no Kotlin, no Swift.

When you call `startStream()`:
1. The engine estimates media bitrate from file size and computes an adaptive startup piece count (1–5 pieces instead of a fixed number) — large files start faster because fewer pieces need to arrive before the player can begin
2. Critical startup pieces get `set_piece_deadline(0)` triggering libtorrent's time-critical mode, which requests them from multiple peers simultaneously and cancels slow ones. Tail pieces (moov atom) download at lower priority so they never steal bandwidth from the first frame
3. A tiny HTTP server starts on `127.0.0.1` on a random free port, running its own accept thread
4. The server responds to byte-range requests, blocking via condition variables until each piece arrives — zero CPU polling. A hot piece cache serves repeated reads instantly without hitting disk
5. Only the current playback piece + 2 ahead are prioritized — 100% of bandwidth goes to what the player needs right now. Played pieces stay in a trailing retention window (3 pieces) for quick rewinds and player re-reads
6. On seek, old piece deadlines are cleared immediately, trailing pieces are dropped, and new deadlines are set at the seek target with tight spacing — all bandwidth redirects to the seek position within milliseconds

The stream URL works with any player that handles HTTP range requests.

---

## License

GPL v3
