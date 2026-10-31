// LibtorrentFlutter — Main engine class.
// Manages a libtorrent session with automatic tracker injection,
// status polling, and a built-in HTTP streaming server.

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:math';

import 'package:ffi/ffi.dart';
import 'package:flutter/services.dart' show rootBundle;

import 'ffi_bindings.dart';
import 'models.dart';

part 'download_persistence.dart';

// ─── Tracker Management ─────────────────────────────────────────────────────

/// Automatically fetches and injects best public trackers into magnet URIs.
class TrackerManager {
  static final List<String> _extraTrackers = [];

  /// Fetch the latest best-performing tracker list from GitHub.
  static Future<void> fetchBestTrackers() async {
    try {
      final client = HttpClient();
      client.connectionTimeout = const Duration(seconds: 5);
      final req = await client.getUrl(
        Uri.parse(
          'https://raw.githubusercontent.com/ngosang/trackerslist/master/trackers_best.txt',
        ),
      );
      final res = await req.close();
      if (res.statusCode == 200) {
        final body = await res.transform(const SystemEncoding().decoder).join();
        final list = body
            .split('\n')
            .map((s) => s.trim())
            .where((s) => s.isNotEmpty)
            .toList();
        _extraTrackers.clear();
        _extraTrackers.addAll(list);
      }
      client.close(force: true);
    } catch (_) {}
  }

  /// Inject extra trackers into a magnet URI for better peer discovery.
  static String injectTrackers(String magnetUri) {
    if (_extraTrackers.isEmpty) return magnetUri;
    var uri = magnetUri;
    for (final tr in _extraTrackers) {
      if (!uri.contains(Uri.encodeComponent(tr))) {
        uri += '&tr=${Uri.encodeComponent(tr)}';
      }
    }
    return uri;
  }
}

// ─── Status converters ──────────────────────────────────────────────────────

TorrentInfo _toTorrentInfo(LtTorrentStatus s) => TorrentInfo(
      id: s.id,
      name: readCharArray(s.name, 512),
      savePath: readCharArray(s.savePath, 1024),
      errorMsg: readCharArray(s.errorMsg, 256),
      state: stateFromInt(s.state),
      progress: s.progress.clamp(0.0, 1.0),
      downloadRate: s.downloadRate,
      uploadRate: s.uploadRate,
      totalDone: s.totalDone,
      totalWanted: s.totalWanted,
      totalUploaded: s.totalUploaded,
      numPeers: s.numPeers,
      numSeeds: s.numSeeds,
      isPaused: s.isPaused != 0,
      isFinished: s.isFinished != 0,
      hasMetadata: s.hasMetadata != 0,
      queuePosition: s.queuePosition,
    );

FileInfo _toFileInfo(LtFileInfo f) => FileInfo(
      index: f.index,
      name: readCharArray(f.name, 512),
      path: readCharArray(f.path, 1024),
      size: f.size,
      isStreamable: f.isStreamable != 0,
    );

StreamInfo _toStreamInfo(LtStreamStatus s) => StreamInfo(
      id: s.id,
      torrentId: s.torrentId,
      fileIndex: s.fileIndex,
      url: readCharArray(s.url, 256),
      fileSize: s.fileSize,
      readHead: s.readHead,
      streamState: streamStateFromInt(s.streamState),
      bufferSeconds: s.bufferSeconds,
      bufferPieces: s.bufferPieces,
      readaheadWindow: s.readaheadWindow,
      activePeers: s.activePeers,
      downloadRate: s.downloadRate,
    );

// ─── LibtorrentFlutter ──────────────────────────────────────────────────────

/// The main libtorrent engine for Flutter.
///
/// Usage:
/// ```dart
/// await LibtorrentFlutter.init();
/// final torrentId = LibtorrentFlutter.instance.addMagnet(magnetUri, savePath);
/// final stream = LibtorrentFlutter.instance.startStream(torrentId);
/// // Pass stream.url to your video player
/// ```
class LibtorrentFlutter {
  static LibtorrentFlutter? _instance;
  static Future<void>? _initializing;

  late final TorrentBridgeBindings _b;
  late final Pointer<LtSessionOpaque> _session;
  late final String _defaultSavePath;

  // Torrent status
  final _torrentsCtrl = StreamController<Map<int, TorrentInfo>>.broadcast();
  final Map<int, TorrentInfo> _torrents = {};
  Timer? _pollTimer;
  bool _disposed = false;
  bool _disposing = false;
  Future<void>? _disposeFuture;
  late final _DownloadPersistence _persistence = _DownloadPersistence(this);

  void _checkAlive() {
    if (_disposed || _disposing)
      throw StateError('Engine is closing or disposed');
  }

  // Stream status
  final _streamsCtrl = StreamController<Map<int, StreamInfo>>.broadcast();
  final Map<int, StreamInfo> _streams = {};

  static const _maxTorrents = 1024;
  static const _maxStreams = 64;

  LibtorrentFlutter._();

  /// The singleton instance. Only available after [init] completes.
  static LibtorrentFlutter get instance {
    if (_instance == null)
      throw StateError('LibtorrentFlutter.init() not called');
    return _instance!;
  }

  /// Whether the engine has been initialized.
  static bool get isInitialized => _instance != null;

  /// Initialize the libtorrent engine.
  ///
  /// - [listenInterface] — network interface to listen on (empty = all).
  /// - [downloadLimit] / [uploadLimit] — speed limits in bytes/sec (0 = unlimited).
  /// - [pollInterval] — how often to poll for torrent/stream status updates.
  /// - [fetchTrackers] — automatically fetch best public trackers on startup.
  /// - [downloadStateDirectory] — durable task-state directory; restores before init completes.
  /// - [downloadStateSaveInterval] — periodic resume checkpoint interval (positive).
  /// - [defaultSavePath] — where to save torrent data. Defaults to system temp dir.
  static Future<void> init({
    String listenInterface = '',
    int downloadLimit = 0,
    int uploadLimit = 0,
    Duration pollInterval = const Duration(milliseconds: 600),
    bool fetchTrackers = true,
    String? defaultSavePath,
    String? downloadStateDirectory,
    Duration downloadStateSaveInterval = const Duration(seconds: 30),
  }) {
    if (_instance != null) return Future<void>.value();
    return _initializing ??= _initialize(
      listenInterface: listenInterface,
      downloadLimit: downloadLimit,
      uploadLimit: uploadLimit,
      pollInterval: pollInterval,
      fetchTrackers: fetchTrackers,
      defaultSavePath: defaultSavePath,
      downloadStateDirectory: downloadStateDirectory,
      downloadStateSaveInterval: downloadStateSaveInterval,
    ).whenComplete(() => _initializing = null);
  }

  static Future<void> _initialize({
    String listenInterface = '',
    int downloadLimit = 0,
    int uploadLimit = 0,
    Duration pollInterval = const Duration(milliseconds: 600),
    bool fetchTrackers = true,
    String? defaultSavePath,
    String? downloadStateDirectory,
    Duration downloadStateSaveInterval = const Duration(seconds: 30),
  }) async {
    if (downloadStateSaveInterval <= Duration.zero) {
      throw ArgumentError.value(
          downloadStateSaveInterval, 'downloadStateSaveInterval');
    }
    final engine = LibtorrentFlutter._();
    engine._persistence.interval = downloadStateSaveInterval;

    // Fetch best trackers in background (fire & forget)
    if (fetchTrackers) {
      TrackerManager.fetchBestTrackers();
    }

    final lib = TorrentBridgeBindings.open();
    engine._b = lib;

    if (Platform.isAndroid) {
      try {
        // Android does not expose its platform trust store in a form that the
        // statically linked OpenSSL build can reliably consume. Ship a PEM CA
        // bundle with the plugin, copy it to an ordinary app-readable file,
        // and set SSL_CERT_FILE before the native libtorrent session starts.
        final certDir = Directory(
          '${Directory.systemTemp.path}/libtorrent_flutter',
        );
        await certDir.create(recursive: true);

        final certFile = File('${certDir.path}/cacert.pem');
        final byteData = await rootBundle.load(
          'packages/libtorrent_flutter/assets/cacert.pem',
        );
        final certBytes = byteData.buffer.asUint8List(
          byteData.offsetInBytes,
          byteData.lengthInBytes,
        );

        // A normal Mozilla CA bundle is well over 100 KB.
        final certText = ascii.decode(certBytes, allowInvalid: true);
        if (certBytes.length < 100 * 1024 ||
            !certText.contains('-----BEGIN CERTIFICATE-----')) {
          throw const FormatException(
            'Bundled cacert.pem is invalid or too small',
          );
        }

        // Write atomically so a killed process cannot leave a partial bundle.
        final pendingFile = File('${certFile.path}.pending');
        await pendingFile.writeAsBytes(certBytes, flush: true);
        if (await certFile.exists()) {
          await certFile.delete();
        }
        await pendingFile.rename(certFile.path);

        final certPathPtr = certFile.path.toNativeUtf8();
        try {
          engine._b.setSslCertPath(certPathPtr);
        } finally {
          malloc.free(certPathPtr);
        }
      } catch (e) {
        throw StateError(
          'libtorrent_flutter: failed to prepare Android CA bundle: $e\n'
          'Make sure assets/cacert.pem is present in the package. '
          'Download it from https://curl.se/ca/cacert.pem',
        );
      }
    }

    final iface = listenInterface.toNativeUtf8();
    try {
      final session = engine._b.createSession(
        iface,
        downloadLimit,
        uploadLimit,
      );
      if (session == nullptr) {
        final err = engine._b.lastError().toDartString();
        throw StateError('Failed to create libtorrent session: $err');
      }
      engine._session = session;
    } finally {
      malloc.free(iface);
    }

    engine._defaultSavePath = defaultSavePath ?? Directory.systemTemp.path;
    try {
      if (downloadStateDirectory != null) {
        await engine.restoreDownloadState(downloadStateDirectory);
      }
    } catch (_) {
      engine._b.destroySession(engine._session);
      engine._disposed = true;
      await engine._persistence.close();
      await engine._torrentsCtrl.close();
      await engine._streamsCtrl.close();
      rethrow;
    }
    _instance = engine;
    engine._startPolling(pollInterval);
  }

  // ─── Public Streams ─────────────────────────────────────────────────────────

  /// Stream of all torrent statuses, emitted on every poll update.
  Stream<Map<int, TorrentInfo>> get torrentUpdates => _torrentsCtrl.stream;

  /// Stream of all active stream statuses.
  Stream<Map<int, StreamInfo>> get streamUpdates => _streamsCtrl.stream;

  /// Current snapshot of all known torrents.
  Map<int, TorrentInfo> get torrents => Map.unmodifiable(_torrents);

  /// Current snapshot of all active streams.
  Map<int, StreamInfo> get streams => Map.unmodifiable(_streams);

  /// libtorrent version string.
  String get libraryVersion => _b.version().toDartString();

  // ─── Torrent Management ─────────────────────────────────────────────────────

  /// Save selected download tasks and flushed resume data, never playback demands.
  /// Binds this engine to one directory. Restore existing state before saving.
  /// Completes after an atomic checkpoint; throws on I/O/native errors or timeout.
  Future<void> saveDownloadState(String directory) {
    _checkAlive();
    return _persistence.save(directory);
  }

  /// Restore downloads (not streams), retaining live tasks with matching infohashes.
  /// IDs are session-local. Missing payload is checked by libtorrent as needed.
  /// Corrupt/unsupported state throws and is not silently overwritten.
  Future<List<int>> restoreDownloadState(String directory) {
    _checkAlive();
    return _persistence.restore(directory);
  }

  /// Auto-save failures. Manual saves/dispose report failures through their Future.
  Stream<Object> get downloadStateErrors => _persistence.errors.stream;

  /// Find an existing task using v1/v2 hashes, independent of tracker/name ordering.
  /// Returns null when absent; malformed magnets throw ArgumentError.
  int? findTorrentByMagnet(String magnet) {
    _checkAlive();
    final uri = magnet.toNativeUtf8();
    try {
      final id = _b.findTorrentByMagnet(_session, uri);
      if (id < 0) throw ArgumentError(_b.lastError().toDartString());
      return id == 0 ? null : id;
    } finally {
      malloc.free(uri);
    }
  }

  /// Add a torrent from a magnet URI.
  ///
  /// Returns the torrent ID. [savePath] defaults to the path set in init().
  /// Set [streamOnly] to true for playback-only selection initially.
  /// [downloadFile] can add full-file download demands later.
  /// Multiple files can stream concurrently. Use [switchFile] when changing
  /// episodes to replace old streams while retaining connections and disk data.
  /// Stopping a stream retains the torrent, connections and downloaded files.
  /// Adding the same torrent again returns its existing ID, preserving its
  /// save path and state. A different [streamOnly] mode throws. The returned
  /// ID is shared, not a separate task or reference-counted ownership token.
  int addMagnet(String magnetUri, [String? savePath, bool streamOnly = false]) {
    _checkAlive();
    final enhanced = TrackerManager.injectTrackers(magnetUri);
    final m = enhanced.toNativeUtf8();
    final s = (savePath ?? _defaultSavePath).toNativeUtf8();
    try {
      final id = _b.addMagnet(_session, m, s, streamOnly ? 1 : 0);
      if (id < 0) throw Exception(_b.lastError().toDartString());
      _persistence.changed();
      return id;
    } finally {
      malloc.free(m);
      malloc.free(s);
    }
  }

  /// Add a torrent from a .torrent file path.
  /// Duplicate torrents follow [addMagnet]'s ID reuse and mode-conflict rules,
  /// including torrents originally added from a magnet URI.
  int addTorrentFile(
    String filePath, [
    String? savePath,
    bool streamOnly = false,
  ]) {
    _checkAlive();
    final f = filePath.toNativeUtf8();
    final s = (savePath ?? _defaultSavePath).toNativeUtf8();
    try {
      final id = _b.addTorrentFile(_session, f, s, streamOnly ? 1 : 0);
      if (id < 0) throw Exception(_b.lastError().toDartString());
      _persistence.changed();
      return id;
    } finally {
      malloc.free(f);
      malloc.free(s);
    }
  }

  /// Stop all associated streams and remove the torrent.
  /// Optionally delete downloaded files. Safe to call repeatedly.
  void removeTorrent(int id, {bool deleteFiles = false}) {
    _checkAlive();
    _persistence.beforeRemove(id);
    _b.removeTorrent(_session, id, deleteFiles ? 1 : 0);
    _streams.removeWhere((_, stream) => stream.torrentId == id);
    _streamsCtrl.add(Map.unmodifiable(_streams));
    _torrents.remove(id);
    _torrentsCtrl.add(Map.unmodifiable(_torrents));
  }

  /// Pause a torrent.
  void pauseTorrent(int id) {
    _checkAlive();
    _b.pauseTorrent(_session, id);
    _persistence.changed();
  }

  /// Resume a paused torrent.
  void resumeTorrent(int id) {
    _checkAlive();
    _b.resumeTorrent(_session, id);
    _persistence.changed();
  }

  /// Recheck torrent integrity.
  void recheckTorrent(int id) => _b.recheckTorrent(_session, id);

  // ─── File Enumeration ───────────────────────────────────────────────────────

  /// Get the list of files in a torrent (requires metadata).
  List<FileInfo> getFiles(int torrentId) {
    final count = _b.getFileCount(_session, torrentId);
    if (count <= 0) return [];
    final buf = calloc<LtFileInfo>(count);
    try {
      final n = _b.getFiles(_session, torrentId, buf, count);
      return List.generate(n, (i) => _toFileInfo(buf[i]));
    } finally {
      calloc.free(buf);
    }
  }

  /// Download and retain the whole selected file without interrupting playback.
  /// Requires metadata. Repeated calls are idempotent. Selection survives seek,
  /// switch and stop; configure downloadStateDirectory for restart recovery.
  /// Use a durable savePath when adding the torrent; this does not move files.
  void downloadFile(int torrentId, {required int fileIndex}) {
    _checkAlive();
    if (_b.downloadFile(_session, torrentId, fileIndex) == 0) {
      throw StateError('downloadFile failed: ${_b.lastError().toDartString()}');
    }
    _persistence.changed();
  }

  /// Pause a selected file's background download, retaining data and selection.
  /// Throws [FileDownloadInUseException] if any stream still owns this file,
  /// including buffering or a paused player whose stream has not been stopped.
  /// Other files keep downloading. Repeated pauses are idempotent when unused.
  /// Native validation serializes the check with stream start/switch/stop.
  void pauseFileDownload(int torrentId, {required int fileIndex}) {
    _checkAlive();
    final result = _b.pauseFileDownload(_session, torrentId, fileIndex);
    if (result == -1) {
      throw FileDownloadInUseException(torrentId, fileIndex);
    }
    if (result != 1) {
      throw StateError(
          'pauseFileDownload failed: ${_b.lastError().toDartString()}');
    }
    _persistence.changed();
  }

  /// Resume an existing file download without changing other file selections.
  /// Throws for an unselected file. Also resumes a torrent paused as a whole.
  /// Calling [downloadFile] on a paused selection does not resume it.
  void resumeFileDownload(int torrentId, {required int fileIndex}) {
    _checkAlive();
    if (_b.resumeFileDownload(_session, torrentId, fileIndex) != 1) {
      throw StateError(
          'resumeFileDownload failed: ${_b.lastError().toDartString()}');
    }
    _persistence.changed();
  }

  /// Cancel this file's cache selection without stopping playback or deleting data.
  /// Requires metadata; repeated cancellation of a valid file is harmless.
  /// The file disappears from [getFileDownloads]. Use [downloadFile] to add it again.
  /// After the last selection is cancelled, dispose may clean up this task's data.
  /// With persistence configured, changes are checkpointed automatically; await
  /// [saveDownloadState] to confirm durable cancellation before terminating.
  void cancelFileDownload(int torrentId, {required int fileIndex}) {
    _checkAlive();
    if (_b.cancelFileDownload(_session, torrentId, fileIndex) != 1) {
      throw StateError(
          'cancelFileDownload failed: ${_b.lastError().toDartString()}');
    }
    _persistence.changed();
  }

  /// Whether this task owns any full-file downloads, including paused/completed ones.
  /// Ordinary download magnets count before metadata arrives. Missing IDs do not.
  bool hasDownloads(int torrentId) {
    _checkAlive();
    final result = _b.hasDownloads(_session, torrentId);
    if (result < 0) {
      throw StateError('hasDownloads failed: ${_b.lastError().toDartString()}');
    }
    return result != 0;
  }

  /// Snapshot of selected downloads. Progress counts verified bytes only.
  /// Returns an empty list for absent tasks or unavailable metadata.
  List<FileDownloadInfo> getFileDownloads(int torrentId) {
    _checkAlive();
    final count = _b.getFileCount(_session, torrentId);
    if (count <= 0) return [];
    final buf = calloc<LtFileDownloadStatus>(count);
    try {
      final result = _b.getFileDownloads(_session, torrentId, buf, count);
      if (result < 0) {
        throw StateError(
            'getFileDownloads failed: ${_b.lastError().toDartString()}');
      }
      return List.generate(
          result,
          (i) => FileDownloadInfo(
                fileIndex: buf[i].fileIndex,
                fileSize: buf[i].fileSize,
                verifiedBytes: buf[i].verifiedBytes,
                priority: buf[i].priority,
                isComplete: buf[i].isComplete != 0,
                isPaused: buf[i].isPaused != 0,
              ));
    } finally {
      calloc.free(buf);
    }
  }

  /// Set download priorities per file (0 = skip, 1-7 = priority levels).
  /// Replaces persistent download selection and clears file-level pauses.
  /// Stop associated streams first.
  /// Zero deselects a file without deleting bytes; downloadFile adds selection during playback.
  void setFilePriorities(int torrentId, List<int> priorities) {
    _checkAlive();
    if (_streams.values.any((stream) => stream.torrentId == torrentId)) {
      throw StateError('Stop active streams before changing file priorities');
    }
    final count = priorities.length;
    final buf = calloc<Int32>(count);
    try {
      for (var i = 0; i < count; i++) {
        buf[i] = priorities[i];
      }
      _b.setFilePriorities(_session, torrentId, buf, count);
      final error = _b.lastError().toDartString();
      if (error.isNotEmpty)
        throw StateError('setFilePriorities failed: $error');
      _persistence.changed();
    } finally {
      calloc.free(buf);
    }
  }

  // ─── Streaming ──────────────────────────────────────────────────────────────

  /// Start streaming a file from a torrent.
  ///
  /// Returns [StreamInfo] with an HTTP URL that can be passed to any video player.
  /// [fileIndex] = -1 auto-selects the largest streamable file.
  /// [maxCacheBytes] controls how much RAM the piece cache uses (0 = default ~128MB).
  /// Opens a stream without stopping existing streams or deleting data.
  /// Different files may stream concurrently, including in streamOnly mode.
  /// Their downloaded data is retained and may accumulate; use [switchFile]
  /// to replace old streams and their download demands when changing episodes.
  StreamInfo startStream(
    int torrentId, {
    int fileIndex = -1,
    int maxCacheBytes = 0,
  }) {
    _checkAlive();
    final streamId = _b.startStream(
      _session,
      torrentId,
      fileIndex,
      maxCacheBytes,
    );
    return _registerStream(torrentId, streamId, 'startStream');
  }

  /// Stop this torrent's streams and open [fileIndex] on the same torrent.
  /// Keeps peer connections and downloaded data, withdrawing old stream demands
  /// and prioritizing the new file's startup pieces. Data may accumulate on disk.
  /// Release the old player before calling;
  /// no separate [stopStream] call is needed. The returned URL/ID replace the old
  /// ones. File validation happens before stopping the old streams.
  /// [fileIndex] must be explicit (>= 0); reselecting the same file creates a new
  /// stream while reusing its downloaded data and preserving full-file downloads.
  StreamInfo switchFile(
    int torrentId, {
    required int fileIndex,
    int maxCacheBytes = 0,
  }) {
    _checkAlive();
    final streamId = _b.switchFile(
      _session,
      torrentId,
      fileIndex,
      maxCacheBytes,
    );
    return _registerStream(torrentId, streamId, 'switchFile');
  }

  /// Wait for the selected file's first piece to be verified and readable.
  ///
  /// This does not wait for container parsing or decoded video. Timeout only
  /// ends this wait: the same stream ID/URL and background downloads survive.
  /// Multiple waits are independent. A stopped/replaced/removed stream or a
  /// disposed engine returns [StreamStartResult.invalid], including mid-wait.
  /// Zero timeout checks once; negative timeouts throw [ArgumentError].
  Future<StreamStartResult> waitForStreamStart(
    int streamId, {
    Duration timeout = const Duration(seconds: 15),
  }) async {
    if (timeout.isNegative) {
      throw ArgumentError.value(timeout, 'timeout', 'must not be negative');
    }
    final watch = Stopwatch()..start();
    while (true) {
      // Never pass a destroyed session pointer to FFI, even if a new engine
      // has since been initialized and reuses the same numeric stream IDs.
      if (_disposed) return StreamStartResult.invalid;
      final state = _b.checkStreamStart(_session, streamId);
      if (state == -1) return StreamStartResult.invalid;
      if (state == 1) return StreamStartResult.ready;
      if (state != 0) {
        throw StateError(
          'waitForStreamStart failed: ${_b.lastError().toDartString()}',
        );
      }
      final remaining = timeout - watch.elapsed;
      if (remaining <= Duration.zero) return StreamStartResult.timeout;
      await Future<void>.delayed(
        remaining < const Duration(milliseconds: 100)
            ? remaining
            : const Duration(milliseconds: 100),
      );
    }
  }

  StreamInfo _registerStream(int torrentId, int streamId, String operation) {
    final error = streamId < 0 ? _b.lastError().toDartString() : null;
    _removeStoppedStreams(torrentId);
    if (error != null) {
      _pollTorrents();
      throw Exception('$operation failed: $error');
    }
    final statusBuf = calloc<LtStreamStatus>();
    try {
      final ok = _b.getStreamStatus(_session, streamId, statusBuf);
      if (ok == 0) throw Exception('Failed to get stream status');
      final info = _toStreamInfo(statusBuf.ref);
      _streams[streamId] = info;
      _streamsCtrl.add(Map.unmodifiable(_streams));
      return info;
    } finally {
      calloc.free(statusBuf);
    }
  }

  /// Stop only this stream and withdraw its playback demands.
  /// Retains the task/files; selected downloads continue. Use removeTorrent for
  /// explicit cleanup. [keepTorrent] is ignored and retained for source compatibility.
  void stopStream(int streamId, {bool keepTorrent = true}) {
    _b.stopStream(_session, streamId, 1);
    final error = _b.lastError().toDartString();
    _streams.remove(streamId);
    _streamsCtrl.add(Map.unmodifiable(_streams));
    // Reflect the task after withdrawing playback demand.
    _pollTorrents();
    if (error.isNotEmpty) throw Exception('stopStream failed: $error');
  }

  // An episode switch may stop multiple native streams before returning,
  // including when new startup fails. Do not leave their URLs visible in Dart.
  void _removeStoppedStreams(int torrentId) {
    final ids = _streams.values
        .where((stream) => stream.torrentId == torrentId)
        .map((stream) => stream.id)
        .toList();
    if (ids.isEmpty) return;
    final status = calloc<LtStreamStatus>();
    var changed = false;
    try {
      for (final id in ids) {
        if (_b.getStreamStatus(_session, id, status) == 0) {
          _streams.remove(id);
          changed = true;
        }
      }
    } finally {
      calloc.free(status);
    }
    if (changed) _streamsCtrl.add(Map.unmodifiable(_streams));
  }

  /// Stop all streams for a torrent, retaining files and download demands.
  /// [keepTorrent] is ignored for compatibility, as in stopStream.
  void stopAllStreamsForTorrent(int torrentId, {bool keepTorrent = true}) {
    final toStop = _streams.entries
        .where((e) => e.value.torrentId == torrentId)
        .map((e) => e.key)
        .toList();
    for (final sid in toStop) {
      stopStream(sid, keepTorrent: keepTorrent);
    }
  }

  /// Get the current info for a specific stream, or null if not found.
  StreamInfo? getStreamInfo(int streamId) => _streams[streamId];

  /// Whether a torrent is currently being streamed.
  bool isStreaming(int torrentId) =>
      _streams.values.any((s) => s.torrentId == torrentId && s.isActive);

  /// Preload head + tail of the stream file for fast playback start.
  /// Port of TorrServer's torr/preload.go Preload() function.
  /// [preloadBytes] = 0 defaults to 16MB (head + tail).
  bool preloadStream(int streamId, {int preloadBytes = 0}) {
    return _b.preloadStream(_session, streamId, preloadBytes) != 0;
  }

  /// Configure the TorrServer-style cache for an active stream.
  /// Port of TorrServer's settings/btsets.go BTSets fields.
  ///
  /// - [capacity] — cache size in bytes (default 64MB).
  /// - [readAheadPct] — percentage 5-100 of cache used for read-ahead (default 95%).
  /// - [connectionsLimit] — max concurrent piece requests per reader (default 25).
  void setCacheSettings(
    int streamId, {
    int capacity = 0,
    int readAheadPct = 0,
    int connectionsLimit = 0,
  }) {
    _b.setCacheSettings(
      _session,
      streamId,
      capacity,
      readAheadPct,
      connectionsLimit,
    );
  }

  // ─── Speed Limits ─────────────────────────────────────────────────────────

  /// Set download speed limit in bytes/sec (0 = unlimited).
  void setDownloadLimit(int bps) => _b.setDownloadLimit(_session, bps);

  /// Set upload speed limit in bytes/sec (0 = unlimited).
  void setUploadLimit(int bps) => _b.setUploadLimit(_session, bps);

  // ─── Engine Configuration — port of settings/btsets.go + btserver.go ───────

  /// Apply TorrServer-style engine configuration.
  ///
  /// Port of btserver.go configure(). Maps [BtConfig] fields to libtorrent
  /// settings_pack values: encryption, DHT, UPnP, rate limits, etc.
  /// Also applies cache/reader settings to all future streams.
  void configureSession(BtConfig config) {
    final cfgPtr = calloc<LtBtConfig>();
    try {
      cfgPtr.ref.cacheSize = config.cacheSize;
      cfgPtr.ref.readerReadAhead = config.readerReadAhead;
      cfgPtr.ref.preloadCache = config.preloadCache;
      cfgPtr.ref.connectionsLimit = config.connectionsLimit;
      cfgPtr.ref.torrentDisconnectTimeout = config.torrentDisconnectTimeout;
      cfgPtr.ref.forceEncrypt = config.forceEncrypt ? 1 : 0;
      cfgPtr.ref.disableTcp = config.disableTcp ? 1 : 0;
      cfgPtr.ref.disableUtp = config.disableUtp ? 1 : 0;
      cfgPtr.ref.disableUpload = config.disableUpload ? 1 : 0;
      cfgPtr.ref.disableDht = config.disableDht ? 1 : 0;
      cfgPtr.ref.disableUpnp = config.disableUpnp ? 1 : 0;
      cfgPtr.ref.enableIpv6 = config.enableIpv6 ? 1 : 0;
      cfgPtr.ref.downloadRateLimit = config.downloadRateLimit;
      cfgPtr.ref.uploadRateLimit = config.uploadRateLimit;
      cfgPtr.ref.peersListenPort = config.peersListenPort;
      cfgPtr.ref.responsiveMode = config.responsiveMode ? 1 : 0;
      _b.configureSession(_session, cfgPtr);
    } finally {
      calloc.free(cfgPtr);
    }
  }

  /// Get TorrServer's default engine configuration.
  ///
  /// Port of settings.SetDefaultConfig(). Returns a [BtConfig] with the
  /// same defaults TorrServer uses out of the box.
  BtConfig getDefaultConfig() {
    final cfgPtr = calloc<LtBtConfig>();
    try {
      _b.getDefaultConfig(cfgPtr);
      return BtConfig(
        cacheSize: cfgPtr.ref.cacheSize,
        readerReadAhead: cfgPtr.ref.readerReadAhead,
        preloadCache: cfgPtr.ref.preloadCache,
        connectionsLimit: cfgPtr.ref.connectionsLimit,
        torrentDisconnectTimeout: cfgPtr.ref.torrentDisconnectTimeout,
        forceEncrypt: cfgPtr.ref.forceEncrypt != 0,
        disableTcp: cfgPtr.ref.disableTcp != 0,
        disableUtp: cfgPtr.ref.disableUtp != 0,
        disableUpload: cfgPtr.ref.disableUpload != 0,
        disableDht: cfgPtr.ref.disableDht != 0,
        disableUpnp: cfgPtr.ref.disableUpnp != 0,
        enableIpv6: cfgPtr.ref.enableIpv6 != 0,
        downloadRateLimit: cfgPtr.ref.downloadRateLimit,
        uploadRateLimit: cfgPtr.ref.uploadRateLimit,
        peersListenPort: cfgPtr.ref.peersListenPort,
        responsiveMode: cfgPtr.ref.responsiveMode != 0,
      );
    } finally {
      calloc.free(cfgPtr);
    }
  }

  /// Number of active streams across all torrents.
  int get activeStreamCount => _b.getActiveStreams(_session);

  // ─── Polling ──────────────────────────────────────────────────────────────

  void _startPolling(Duration interval) {
    _pollTimer = Timer.periodic(interval, (_) => _poll());
  }

  void _poll() {
    _pollTorrents();
    _pollStreams();
    _pollAlerts();
  }

  void _pollAlerts() {
    final callback = NativeCallable<LtAlertCallbackNative>.isolateLocal(
      _onAlert,
    );
    try {
      _b.pollAlerts(_session, callback.nativeFunction, nullptr);
    } finally {
      callback.close();
    }
  }

  static void _onAlert(
    int type,
    int torrentId,
    Pointer<Utf8> message,
    Pointer<Void> userData,
  ) {
    // Silently consume alerts — users can listen to torrentUpdates for state changes.
    // Uncomment for debugging:
    // Native diagnostics can be enabled with TB_LOG_FILE when needed.
  }

  void _pollTorrents() {
    final count = _b.getTorrentCount(_session);
    final buf = calloc<LtTorrentStatus>(max(count, _maxTorrents));
    try {
      final n = _b.getAllStatuses(_session, buf, max(count, _maxTorrents));
      bool changed = false;
      final seen = <int>{};

      for (var i = 0; i < n; i++) {
        final info = _toTorrentInfo(buf[i]);
        seen.add(info.id);
        final old = _torrents[info.id];
        if (old == null || _changed(old, info)) {
          _torrents[info.id] = info;
          changed = true;
        }
      }
      final stale = _torrents.keys.where((k) => !seen.contains(k)).toList();
      for (final k in stale) {
        _torrents.remove(k);
        changed = true;
      }
      if (changed) _torrentsCtrl.add(Map.unmodifiable(_torrents));
    } finally {
      calloc.free(buf);
    }
  }

  void _pollStreams() {
    if (_streams.isEmpty) return;
    final buf = calloc<LtStreamStatus>(_maxStreams);
    try {
      final n = _b.getAllStreamStatuses(_session, buf, _maxStreams);
      bool changed = false;
      for (var i = 0; i < n; i++) {
        final info = _toStreamInfo(buf[i]);
        final old = _streams[info.id];
        if (old == null ||
            old.streamState != info.streamState ||
            old.bufferPieces != info.bufferPieces ||
            old.readHead != info.readHead ||
            old.activePeers != info.activePeers) {
          _streams[info.id] = info;
          changed = true;
        }
        if (!info.isActive) {
          _streams.remove(info.id);
          changed = true;
        }
      }
      if (changed) _streamsCtrl.add(Map.unmodifiable(_streams));
    } finally {
      calloc.free(buf);
    }
  }

  bool _changed(TorrentInfo a, TorrentInfo b) =>
      a.state != b.state ||
      a.progress != b.progress ||
      a.downloadRate != b.downloadRate ||
      a.uploadRate != b.uploadRate ||
      a.totalDone != b.totalDone ||
      a.numPeers != b.numPeers ||
      a.isPaused != b.isPaused ||
      a.hasMetadata != b.hasMetadata ||
      a.name != b.name;

  // ─── Cleanup ───────────────────────────────────────────────────────────────

  /// Clean up a single torrent: stops its streams, removes it, deletes files.
  ///
  /// Returns true if the torrent existed and was removed, false if it was
  /// already gone.
  bool disposeTorrent(int torrentId) {
    // A torrent may have been added before the first status poll.
    final status = calloc<LtTorrentStatus>();
    try {
      final existed = _b.getStatus(_session, torrentId, status) != 0;
      removeTorrent(torrentId, deleteFiles: true);
      return existed;
    } finally {
      calloc.free(status);
    }
  }

  /// Clean up ALL torrents: stops every stream, removes every torrent,
  /// deletes all downloaded files, including explicitly selected downloads.
  /// This is an explicit delete-all action, not normal playback exit.
  void disposeAll() {
    _pollTorrents();
    for (final tid in _torrents.keys.toList()) {
      removeTorrent(tid, deleteFiles: true);
    }
  }

  /// Save configured download state before destroying the engine. Download files
  /// survive; temporary playback tasks are deleted. On save failure the engine
  /// remains alive and dispose may be retried; explicit disposeAll deletes data.
  Future<void> dispose() {
    if (_disposed) return Future<void>.value();
    return _disposeFuture ??= _dispose();
  }

  Future<void> _dispose() async {
    _disposing = true;
    try {
      await _persistence.finish();
    } catch (_) {
      _disposing = false;
      _disposeFuture = null;
      _persistence.startTimer();
      rethrow;
    }
    _disposing = false;
    _pollTorrents();
    for (final tid in _torrents.keys.toList()) {
      if (!hasDownloads(tid)) removeTorrent(tid, deleteFiles: true);
    }
    _pollTimer?.cancel();
    _disposed = true;
    _b.destroySession(_session);
    _streams.clear();
    _torrents.clear();
    await _persistence.close();
    await _torrentsCtrl.close();
    await _streamsCtrl.close();
    _instance = null;
  }
}
