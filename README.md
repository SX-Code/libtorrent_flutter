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

// Stop playback only; files and selected downloads survive.
engine.stopStream(streamId);
engine.stopAllStreamsForTorrent(torrentId);
// keepTorrent is accepted for compatibility but ignored, even when false.
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
downloading. Use `switchFile` for episode changes to replace old stream demands
while retaining the torrent, connections and downloaded data. Verified pieces can upload while playing, including after download completes.

```dart
final torrentId = engine.addMagnet(magnetUri, null, true);
// Wait for metadata, then select a file from engine.getFiles(torrentId).
final first = engine.startStream(torrentId, fileIndex: firstFileIndex);
// Play first.url. When switching, release the old player first.
final next = engine.switchFile(torrentId, fileIndex: nextFileIndex);
// switchFile stops old streams and changes priorities; no stopStream needed.
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
    // Retry the wait on this ID; switchFile would replace the stream and URL.
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
downloaded data is retained and may accumulate. Use `switchFile` to replace all
old streams with one new stream.

`switchFile` reuses the existing torrent handle, scheduler, peer connections and
verified disk pieces (including partfile data). It withdraws the old streams'
priorities/deadlines and gives the selected file's startup pieces immediate
priority. Missing pieces with neither playback nor full-file download demand become unwanted;
verified pieces remain available for upload. In-flight blocks can still arrive.
Switching does not pause, remove, recheck or rebuild the torrent.

Previously downloaded episodes and boundary pieces remain on disk and can be
reused when switching back. `maxCacheBytes` only limits the RAM piece cache;
there is no disk eviction on switching. `switchFile` requires an explicit,
nonnegative file index and validates it before stopping the old streams. Selecting
the same file also creates a new stream/URL, while retaining its downloaded data.
These switching semantics apply to both `streamOnly` modes; selected full-file
downloads retain their baseline priority after playback demands are withdrawn.

Call `switchFile` directly when changing episodes; no separate `stopStream` is
needed. Both APIs preserve disk data. `stopStream` withdraws only playback demand,
so selected file downloads continue even when there are no active streams.

### Cache the current episode while playing

Use a durable application directory as `savePath` when adding the torrent. The
following methods require metadata; `downloadFile` does not move existing files.

```dart
// Click "Cache this episode". Repeated calls do not create duplicate downloads.
engine.downloadFile(torrentId, fileIndex: currentIndex);

// Switch without cancelling the original episode's download.
final next = engine.switchFile(torrentId, fileIndex: nextIndex);

// File snapshots count verified bytes only; progress is 0..1.
final downloads = engine.getFileDownloads(torrentId);
for (final file in downloads) {
  print('${file.fileIndex}: ${file.progress * 100}% complete=${file.isComplete}');
}

// Exit playback. Completed downloads still count as retained selections.
engine.stopStream(next.id);
if (!engine.hasDownloads(torrentId)) {
  engine.removeTorrent(torrentId, deleteFiles: true);
}
```

C++ retains each selected file's download demand independently of playback.
Every missing piece of that file receives baseline priority 4; urgent playback
pieces can rise to 7 with deadlines. Seeking/switching/stopping withdraws only
playback demand, falling back to the download baseline instead of zero. Priorities
are scheduling preferences, not guaranteed bandwidth. Skipped middle sections
continue downloading; a file completes only when every required piece is verified.
Shared boundary pieces count only the bytes belonging to each file.

`streamOnly=false` tasks retain their original file download selection while
playing. `setFilePriorities` replaces that selection (stop active streams first);
zero deselects a file without deleting bytes. `downloadFile` adds a selection
during playback and raises its baseline to at least 4. A selected/completed file
keeps the task eligible for retention until deselected or explicitly removed.
Other temporarily played episodes in that torrent are retained on disk too;
per-file disk deletion and automatic disk quotas are not implemented.

**Breaking cleanup change:** `stopStream` and `stopAllStreamsForTorrent` never
delete tasks/files. `keepTorrent` remains accepted but is ignored, including
`false`. Replace former automatic cleanup with explicit `removeTorrent` calls.
`dispose()` preserves files for download tasks and removes temporary tasks;
`disposeTorrent`/`disposeAll` explicitly delete selected downloads too. Release
player objects separately. File deletion is asynchronous in libtorrent.

### Download persistence and restart recovery

Use durable application directories for **both** payload files and task state:

```dart
await LibtorrentFlutter.init(
  defaultSavePath: downloadDirectory,
  downloadStateDirectory: stateDirectory,
  downloadStateSaveInterval: const Duration(seconds: 30),
);
// init finishes restoring downloads before exposing the singleton.
final engine = LibtorrentFlutter.instance;
final errorSubscription = engine.downloadStateErrors.listen((error) {
  debugPrint('Download checkpoint failed: $error');
});

// Reuse a restored task, regardless of magnet tracker/name ordering.
final torrentId = engine.findTorrentByMagnet(magnet) ??
    engine.addMagnet(magnet, downloadDirectory, true);
final stream = engine.startStream(torrentId, fileIndex: currentIndex);

engine.downloadFile(torrentId, fileIndex: currentIndex);
await engine.saveDownloadState(stateDirectory); // explicit durable checkpoint

// Shutdown saves downloads before destroying the native session.
await errorSubscription.cancel();
await engine.dispose();
```

If initialization does not configure a state directory, call
`await engine.restoreDownloadState(stateDirectory)` **before opening playback**.
It returns the current session's restored task IDs; IDs are not stable across
restarts. Repeated restoration is idempotent and existing tasks with matching
v1/v2 infohashes retain their live selection. Do not add the magnet again to
obtain a second playback/download task. One engine binds to one state directory;
only one engine/process should write that directory.

`saveDownloadState(directory)` waits for libtorrent's asynchronous disk flush and
resume-data alerts, then atomically replaces `downloads.json`. It saves metadata,
save paths, verified progress, download selection and pause state. HTTP streams,
playback deadlines and temporary playback-only tasks are excluded. Checkpoints
run after selection/task changes (debounced) and every 30 seconds by default.
Calling save/restore manually also enables these automatic checkpoints.

`removeTorrent` synchronously removes its durable task record before native
removal, even with `deleteFiles: false`. A concurrent save cannot resurrect it.
`disposeTorrent`/`disposeAll` use the same path. `stopStream` retains the download
and its state. A failed state-file update throws and prevents task removal.

Missing payload or stale resume data is checked by libtorrent as needed; corrupt
or unsupported state is reported instead of silently replaced. Manual saves and
`dispose()` report errors through their Future (30-second native snapshot wait
budget); automatic errors arrive on `downloadStateErrors`. If the final save
fails, the engine remains alive so shutdown can be retried. A process killed
before a checkpoint can lose recent task changes/progress; periodic saving does
not imply continued downloading while iOS suspends the app. Payload paths are
stored as supplied; relocating downloads/app containers is not handled here.

Rebuild the native library/XCFramework with the new resume and lookup exports.
Flutter hot reload cannot update native behavior.

### Cleanup

```dart
// Clean up one torrent: stops its streams, removes it, deletes the files
engine.disposeTorrent(id);  // returns false if already gone

// Explicitly delete all torrents/files, including selected downloads
engine.disposeAll();

// Shutdown — retains download files, removes temporary tasks, closes the session
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
