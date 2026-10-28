# libtorrent_flutter

[English](README.md) | [简体中文](README.zh-CN.md) · [Changelog](CHANGELOG.md) | [中文更新日志](CHANGELOG.zh-CN.md)

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

The GitHub Actions workflow currently builds only iOS for debugging; the other
four platform jobs are temporarily disabled. It uploads artifacts without
publishing releases. See [artifact installation instructions](prebuilt/README.md)
for local validation setup. All-platform CI and device validation are still pending.

## Why libtorrent?

libtorrent 2.1.1 provides native torrent networking and piece scheduling:

- **`set_piece_deadline`** — expresses piece urgency so the time-critical picker can prioritize requests. It is a scheduling target, not a delivery-time guarantee.
- **uTP support** — supports UDP-based peer transport alongside TCP; connectivity still depends on the network and NAT configuration.
- **On-demand streaming** — prioritizes startup pieces, prefetches head/tail data, and follows the player's HTTP Range requests. Startup time depends on peer availability, bandwidth and container layout.
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
  print('${t.totalWanted}');   // currently selected bytes; may change during streaming
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
// A 500 MiB RAM budget: old in-memory pieces are evicted as reads advance.
// This does not limit downloaded files on disk or guarantee stall-free playback.
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
    print('buffer: ${(s.bufferPct * 100).toStringAsFixed(1)}%'); // ahead window, not whole file
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

Before passing a new stream URL to the player, optionally wait for its first
piece to be verified and successfully read. This works after either
`startStream` or `switchFile` and does not block the Dart event loop:

```dart
final result = await engine.waitForStreamStart(
  next.id,
  timeout: const Duration(seconds: 15),
);
switch (result) {
  case StreamStartResult.ready:
    // If this is still the user's selected stream, open next.url in the player.
    break;
  case StreamStartResult.timeout:
    // Keep next.id/next.url. Show a retry action which waits on next.id again.
    // Do not call switchFile again: that would delete the data just downloaded.
    break;
  case StreamStartResult.invalid:
    // The stream was stopped/replaced/removed, or the engine was disposed.
    // Discard this result; do not reopen this URL.
    break;
}
```

Timeout only ends that caller's wait; the stream, URL, download demands and
other waiters survive. A repeated wait returns `ready` immediately once the first
piece has been verified/read. Zero timeout performs a single check; negative
timeouts throw `ArgumentError`. Validity is checked before readiness and timeout.
`ready` does not guarantee an immediate video frame: container probing may need
more data. Guard against stale callbacks if the user switches episodes again.
This API requires rebuilt native artifacts exporting `lt_check_stream_start`.

Add the magnet once and retain its `torrentId` for episode changes. Repeated
`addMagnet`/`addTorrentFile` calls for the same torrent return the existing ID,
keeping its original save path and state. A conflicting `streamOnly` mode throws.
This is one shared task, not separate ownership references: switching or removing
that ID affects all its streams.

`startStream` only opens a stream; it never deletes data or stops other streams.
It allows different files to stream simultaneously in both download modes. Their
downloaded data is retained and may accumulate. Use `switchFile` for episode
changes when old streams and data should be cleared.

`switchFile` ends all streams of the specified torrent, deletes its downloaded data (including partial data in libtorrent's partfile),
and resets the owned-piece state before the next episode starts. The torrent ID,
metadata, file list and discovery settings are retained. Up to 64 known outgoing
peer endpoints are retained (preferring connected seeds) and supplied to the new
torrent for reconnection without waiting for discovery. These are connection
hints, not preserved sockets or a guarantee that a peer remains reachable.
BT connections reconnect;
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

All platforms require matching native artifacts; see the build/install section below.

**iOS** — ensure your app permits HTTP access to the local streaming server. Check `ios/Runner/Info.plist` if you use restrictive `NSAppTransportSecurity` settings or a custom `NetworkExtension`.

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

**Android background streaming** — Android may restrict or terminate background apps. Use a Foreground Service when continued background streaming is needed, following the target Android version's requirements:

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

A single C++ file (`torrent_bridge.cpp`) wraps libtorrent 2.1.1 and compiles to a native static/shared library on each platform. Dart calls the engine via FFI without platform channels.

When you call `startStream()`:

1. The engine estimates bitrate from file size and selects 1–5 critical startup pieces.
2. Critical head pieces get priority 7 and deadlines spaced 50 ms apart, starting at 0 ms. The remainder of the roughly 8 MiB head region gets priority 4; the roughly 8 MiB tail region gets priority 5 and a 1000 ms deadline.
3. An HTTP server starts on `127.0.0.1` using a random free port and a dedicated accept thread.
4. Each GET has its own bounded sliding download window. Missing pieces wait on condition variables with periodic disconnect/timeout checks; cached pieces can serve repeated reads without disk I/O. A piece blocking a read is promoted to priority 7 and deadline 0.
5. The scheduler merges demands using the highest priority and earliest deadline. Independent probes cannot cancel one another. Disk prefetch reads up to two available pieces ahead; this is separate from the download window and the three-piece trailing window.
6. HEAD does not alter scheduling. Finishing/disconnecting a GET removes only that reader's demands. The server does not infer user seeks from Range offsets; use player events to track seeks.

The stream URL works with any player that handles HTTP range requests.

---

## License

GPL v3
