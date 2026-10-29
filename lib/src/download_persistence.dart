part of 'libtorrent_flutter_base.dart';

// One atomic manifest owns the task set. Resume blobs include metadata, save
// paths and download selection; no player/HTTP state is persisted.
class _DownloadPersistence {
  _DownloadPersistence(this.engine);

  final LibtorrentFlutter engine;
  final errors = StreamController<Object>.broadcast();
  Duration interval = const Duration(seconds: 30);
  String? _directory;
  bool _loaded = false;
  Timer? _timer;
  Timer? _debounce;
  Future<void> _tail = Future<void>.value();
  int _revision = 0;
  int _writeId = 0;
  Map<String, String> _records = {};
  Map<int, Set<String>> _recordIds = {};

  File get _file => File('$_directory/downloads.json');

  Future<T> _enqueue<T>(Future<T> Function() operation) {
    final result = _tail.then((_) => operation());
    // Keep the queue usable after a failure; the caller still receives it.
    _tail = result.then<void>((_) {}, onError: (Object _, StackTrace __) {});
    return result;
  }

  void _bind(String directory) {
    if (directory.trim().isEmpty)
      throw ArgumentError('State directory is empty');
    final path = Directory(directory).absolute.path;
    if (_directory != null && _directory != path) {
      throw StateError('Use one download-state directory per engine session');
    }
    Directory(path).createSync(recursive: true);
    _directory = path;
  }

  Future<void> save(String directory) => _enqueue(() async {
        _bind(directory);
        if (!_loaded) {
          if (_file.existsSync()) {
            throw StateError('Restore existing download state before saving');
          }
          _loaded = true;
        }
        await _save();
        startTimer();
      });

  Future<List<int>> restore(String directory) => _enqueue(() async {
        _bind(directory);
        if (_loaded) return _recordIds.keys.toList();
        final records = <String, String>{};
        if (_file.existsSync()) {
          final json = jsonDecode(_file.readAsStringSync());
          if (json is! Map ||
              json['version'] != 1 ||
              json['downloads'] is! Map) {
            throw const FormatException(
                'Unsupported or corrupt download state');
          }
          for (final entry in (json['downloads'] as Map).entries) {
            if (entry.key is! String || entry.value is! String) {
              throw const FormatException('Invalid download record');
            }
            // Validate all envelopes before creating any task.
            final bytes = base64Decode(entry.value as String);
            final buffer = calloc<Uint8>(bytes.length);
            try {
              buffer.asTypedList(bytes.length).setAll(0, bytes);
              if (engine._b.validateResumeData(
                      engine._session, buffer, bytes.length) <
                  0) {
                throw _nativeError('validateDownloadState');
              }
            } finally {
              calloc.free(buffer);
            }
            records[entry.key as String] = entry.value as String;
          }
        }
        final ids = <int, Set<String>>{};
        for (final entry in records.entries) {
          final bytes = base64Decode(entry.value);
          final buffer = calloc<Uint8>(bytes.length);
          try {
            buffer.asTypedList(bytes.length).setAll(0, bytes);
            final id = engine._b
                .restoreResumeData(engine._session, buffer, bytes.length);
            if (id < 0) throw _nativeError('restoreDownloadState');
            ids.putIfAbsent(id, () => {}).add(entry.key);
          } finally {
            calloc.free(buffer);
          }
        }
        _records = records;
        _recordIds = ids;
        _loaded = true;
        _revision++;
        engine._pollTorrents();
        startTimer();
        return ids.keys.toList();
      });

  StateError _nativeError(String operation) =>
      StateError('$operation failed: ${engine._b.lastError().toDartString()}');

  void changed() {
    _revision++;
    if (!_loaded || engine._disposing || engine._disposed) return;
    _debounce?.cancel();
    _debounce = Timer(const Duration(milliseconds: 250), _autoSave);
  }

  void startTimer() {
    if (!_loaded || engine._disposing || engine._disposed) return;
    _timer ??= Timer.periodic(interval, (_) => _autoSave());
  }

  bool _autoSaving = false;
  void _autoSave() {
    if (_autoSaving || !_loaded || engine._disposing || engine._disposed)
      return;
    _autoSaving = true;
    save(_directory!).catchError((Object error) {
      if (!errors.isClosed) errors.add(error);
    }).whenComplete(() => _autoSaving = false);
  }

  // Synchronous removal commits the smaller manifest before native deletion.
  // A concurrently awaiting save checks revision before rename and retries.
  void beforeRemove(int id) {
    final keys = _recordIds[id];
    if (_loaded && keys != null) {
      final remaining = Map<String, String>.of(_records);
      for (final key in keys) {
        remaining.remove(key);
      }
      _writeSync(remaining);
      _records = remaining;
      _recordIds.remove(id);
    }
    changed();
  }

  String _encode(Map<String, String> records) =>
      jsonEncode({'version': 1, 'downloads': records});

  void _writeSync(Map<String, String> records) {
    final pending = File('${_file.path}.${_writeId++}.pending');
    try {
      pending.writeAsStringSync(_encode(records), flush: true);
      pending.renameSync(_file.path);
    } finally {
      if (pending.existsSync()) pending.deleteSync();
    }
  }

  List<int> _downloadIds() {
    final count = engine._b.getTorrentCount(engine._session);
    if (count == 0) return [];
    final buffer = calloc<LtTorrentStatus>(count);
    try {
      final n = engine._b.getAllStatuses(engine._session, buffer, count);
      final result = <int>[];
      for (var i = 0; i < n; i++) {
        final id = buffer[i].id;
        final selected = engine._b.hasDownloads(engine._session, id);
        if (selected < 0) throw _nativeError('hasDownloads');
        if (selected != 0) result.add(id);
      }
      return result;
    } finally {
      calloc.free(buffer);
    }
  }

  Future<String> _capture(int id, Stopwatch watch) async {
    // A previous timed-out request may still be in flight. Drain it, then
    // explicitly request a fresh snapshot of the current download selection.
    while (true) {
      _checkTimeout(watch);
      final started = engine._b.requestResumeData(engine._session, id);
      if (started < 0) throw _nativeError('saveDownloadState');
      if (started == 1) break;
      await Future<void>.delayed(const Duration(milliseconds: 25));
    }
    while (true) {
      _checkTimeout(watch);
      final length = engine._b.readResumeData(engine._session, id, nullptr, 0);
      if (length < 0) throw _nativeError('saveDownloadState');
      if (length > 0) {
        final buffer = calloc<Uint8>(length);
        try {
          final copied =
              engine._b.readResumeData(engine._session, id, buffer, length);
          if (copied != length) throw _nativeError('readResumeData');
          return base64Encode(buffer.asTypedList(length));
        } finally {
          calloc.free(buffer);
        }
      }
      await Future<void>.delayed(const Duration(milliseconds: 25));
    }
  }

  void _checkTimeout(Stopwatch watch) {
    if (watch.elapsed > const Duration(seconds: 30)) {
      throw TimeoutException('Saving download state exceeded 30 seconds');
    }
  }

  Future<void> _save() async {
    final watch = Stopwatch()..start();
    while (true) {
      _checkTimeout(watch);
      final revision = _revision;
      final records = <String, String>{};
      final ids = <int, Set<String>>{};
      try {
        // Start disk snapshots concurrently; only libtorrent's alert thread
        // consumes the callbacks. Dart remains responsive while waiting.
        await Future.wait(_downloadIds().map((id) async {
          final bytes = await _capture(id, watch);
          final magnet =
              engine._b.getTorrentMagnet(engine._session, id).toDartString();
          if (magnet.isEmpty) throw _nativeError('getTorrentMagnet');
          records[magnet] = bytes;
          ids[id] = {magnet};
        }));
      } catch (_) {
        if (revision != _revision) continue;
        rethrow;
      }
      if (revision != _revision) continue;
      final pending = File('${_file.path}.${_writeId++}.pending');
      try {
        await pending.writeAsString(_encode(records), flush: true);
        if (revision != _revision) continue;
        // No await between revision check and replacement: deletion cannot
        // interleave here and a killed write cannot truncate the old manifest.
        pending.renameSync(_file.path);
        _records = records;
        _recordIds = ids;
        return;
      } finally {
        if (pending.existsSync()) pending.deleteSync();
      }
    }
  }

  Future<void> finish() async {
    _timer?.cancel();
    _timer = null;
    _debounce?.cancel();
    await _tail;
    if (_loaded) await _enqueue(_save);
  }

  Future<void> close() async {
    _timer?.cancel();
    _debounce?.cancel();
    await errors.close();
  }
}
