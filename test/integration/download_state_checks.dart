// Real native FFI: restart recovery, stale-snapshot exclusion and disk failures.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void _check(bool condition, String message) {
  if (!condition) throw StateError(message);
}

Future<void> _until(bool Function() condition, String message) async {
  final watch = Stopwatch()..start();
  while (!condition()) {
    if (watch.elapsed > const Duration(seconds: 10)) throw StateError(message);
    await Future<void>.delayed(const Duration(milliseconds: 25));
  }
}

Future<void> checkDownloadState() async {
  final root = await Directory.systemTemp.createTemp('ltf_resume_');
  final state = '${root.path}/state';
  final manifest = File('$state/downloads.json');
  final torrent = File('${root.path}/episodes.torrent');
  const prefix = 'd4:infod5:filesld6:lengthi16384e4:pathl9:first.mp4ee'
      'd6:lengthi16384e4:pathl10:second.mp4eee4:name8:resumefx'
      '12:piece lengthi16384e6:pieces40:';
  await torrent.writeAsBytes([
    ...prefix.codeUnits,
    ...base64Decode('fbZ7rO7ss6QgvzenvspKRRhfjzx5mW+/WQqtnafOL+BICCMzGRO28A=='),
    ...'ee'.codeUnits,
  ]);
  final payload = File('${root.path}/resumefx/first.mp4');
  await payload.parent.create();
  await payload.writeAsBytes(List<int>.filled(16384, 65), flush: true);

  Future<LibtorrentFlutter> init({String? directory}) async {
    await LibtorrentFlutter.init(
      listenInterface: '127.0.0.1:0',
      fetchTrackers: false,
      downloadStateDirectory: directory,
      downloadStateSaveInterval: const Duration(milliseconds: 300),
    );
    final engine = LibtorrentFlutter.instance;
    engine
        .configureSession(const BtConfig(disableDht: true, disableUpnp: true));
    return engine;
  }

  try {
    var engine = await init(directory: state);
    final id = engine.addTorrentFile(torrent.path, root.path, true);
    engine.downloadFile(id, fileIndex: 0);
    final stream = engine.startStream(id, fileIndex: 1); // unselected file
    await _until(() => engine.getFileDownloads(id).single.isComplete,
        'local download not verified');
    engine.pauseFileDownload(id, fileIndex: 0);
    await engine.saveDownloadState(state);
    final saved = jsonDecode(await manifest.readAsString()) as Map;
    final magnet = (saved['downloads'] as Map).keys.single as String;
    final uri = Uri.parse(magnet);
    final hashes = uri.queryParametersAll['xt']!;
    final alternate =
        'magnet:?dn=renamed&xt=${Uri.encodeComponent(hashes.first)}';
    _check(
        engine.findTorrentByMagnet(alternate) == id, 'hash lookup used text');
    _check(engine.getStreamInfo(stream.id) != null, 'snapshot stopped stream');
    _check(
        engine.findTorrentByMagnet('magnet:?xt=urn:btih:${'a' * 40}') == null,
        'missing hash matched');
    var invalid = false;
    try {
      engine.findTorrentByMagnet('not a magnet');
    } on ArgumentError {
      invalid = true;
    }
    _check(invalid, 'invalid magnet accepted');
    // Concurrent final save and dispose must share a live native session.
    final pending = engine.saveDownloadState(state);
    await engine.dispose();
    await pending;
    _check(await payload.exists(), 'shutdown deleted download');

    // An interrupted temporary write is ignored, and concurrent init calls share
    // one restoration/session rather than racing to create two engines.
    File('$state/downloads.json.interrupted.pending')
        .writeAsStringSync('{partial');
    final initialized =
        await Future.wait([init(directory: state), init(directory: state)]);
    _check(identical(initialized[0], initialized[1]),
        'concurrent init created two engines');
    engine = initialized.first;
    final restored = engine.findTorrentByMagnet(alternate)!;
    _check(engine.streams.isEmpty && engine.activeStreamCount == 0,
        'restored playback stream');
    _check(
        engine.getFileDownloads(restored).length == 1 &&
            engine.getFileDownloads(restored).single.fileIndex == 0,
        'playback-only file became a download');
    _check(
        engine.getFileDownloads(restored).single.isPaused &&
            engine.hasDownloads(restored),
        'restart lost file pause or retention');
    engine.resumeFileDownload(restored, fileIndex: 0);
    _check(!engine.getFileDownloads(restored).single.isPaused,
        'restored download cannot resume');
    await _until(() => engine.getFileDownloads(restored).single.isComplete,
        'restored verified data lost');
    final repeated = await engine.restoreDownloadState(state);
    _check(repeated.length == 1 && repeated.single == restored,
        'repeated restore duplicated tasks');

    // Selection updates and completed downloads survive automatic checkpoints.
    engine.downloadFile(restored, fileIndex: 1);
    engine.pauseFileDownload(restored, fileIndex: 1);
    await _until(() {
      final entries =
          jsonDecode(manifest.readAsStringSync())['downloads'] as Map;
      return entries.values.any((blob) {
        final content =
            ascii.decode(base64Decode(blob as String), allowInvalid: true);
        return content.contains('file_priorityli4ei4ee') &&
            content.contains('ltf-paused-filesli1ee');
      });
    }, 'automatic selection checkpoint did not update');
    await engine.dispose();
    await payload.delete();
    engine = await init(directory: state);
    final missingId = engine.findTorrentByMagnet(magnet)!;
    _check(engine.getFileDownloads(missingId).length == 2,
        'updated download selection not restored');
    _check(
        engine.getFileDownloads(missingId).last.isPaused &&
            !engine.getFileDownloads(missingId).last.isComplete,
        'unfinished paused download was enabled after restart');
    await _until(() => !engine.getFileDownloads(missingId).first.isComplete,
        'missing payload falsely stayed complete');

    // Remove after capture has begun: its late result must not resurrect it.
    final removingSave = engine.saveDownloadState(state);
    await Future<void>.delayed(const Duration(milliseconds: 1));
    engine.removeTorrent(missingId, deleteFiles: false);
    _check(
        (jsonDecode(manifest.readAsStringSync())['downloads'] as Map).isEmpty,
        'remove did not synchronously remove durable record');
    await removingSave;
    await engine.dispose();
    engine = await init(directory: state);
    _check(
        engine.findTorrentByMagnet(magnet) == null, 'removed task resurrected');

    // Ordinary magnets must be retained even before metadata arrives.
    const unknown =
        'magnet:?xt=urn:btih:1111111111111111111111111111111111111111';
    final waiting = engine.addMagnet(unknown, root.path, false);
    _check(engine.hasDownloads(waiting), 'ordinary metadata task not retained');
    final temp = engine.addMagnet(
        'magnet:?xt=urn:btih:2222222222222222222222222222222222222222',
        root.path,
        true);
    _check(!engine.hasDownloads(temp), 'temporary magnet marked for download');
    await engine.saveDownloadState(state);
    _check(
        (jsonDecode(manifest.readAsStringSync())['downloads'] as Map).length ==
            1,
        'temporary task persisted');
    await engine.dispose();
    engine = await init(directory: state);
    _check(engine.findTorrentByMagnet(unknown) != null,
        'pre-metadata magnet lost on restart');
    engine.disposeAll();
    await engine.dispose();

    // Validate every native blob before adding any task from a corrupt batch.
    engine = await init();
    final corruptBatch = {
      'version': 1,
      'downloads': {
        ...Map<String, dynamic>.from(saved['downloads'] as Map),
        'invalid': base64Encode('le'.codeUnits),
      }
    };
    manifest.writeAsStringSync(jsonEncode(corruptBatch));
    var invalidBlob = false;
    try {
      await engine.restoreDownloadState(state);
    } on StateError {
      invalidBlob = true;
    }
    _check(invalidBlob && engine.findTorrentByMagnet(magnet) == null,
        'corrupt batch partially restored');
    await engine.dispose();

    // Corrupt data must report failure, retain the file, and allow a fresh init.
    await manifest.writeAsString('{invalid');
    var corrupt = false;
    try {
      await init(directory: state);
    } on FormatException {
      corrupt = true;
    }
    _check(
        corrupt &&
            !LibtorrentFlutter.isInitialized &&
            manifest.readAsStringSync() == '{invalid',
        'corrupt state overwritten');
    await manifest.delete();
    engine = await init(directory: state);

    // A failed checkpoint leaves the engine available and dispose retryable.
    final errorEvents = <Object>[];
    final subscription = engine.downloadStateErrors.listen(errorEvents.add);
    Directory(manifest.path).createSync(); // make rename fail deterministically
    final last = engine.addTorrentFile(torrent.path, root.path, true);
    engine.downloadFile(last, fileIndex: 0);
    await _until(() => errorEvents.isNotEmpty, 'automatic I/O error hidden');
    var failed = false;
    try {
      await engine.dispose();
    } on FileSystemException {
      failed = true;
    }
    _check(failed && engine.hasDownloads(last),
        'failed dispose destroyed session');
    Directory(manifest.path).deleteSync();
    await subscription.cancel();
    await engine.dispose();
  } finally {
    if (LibtorrentFlutter.isInitialized) {
      await LibtorrentFlutter.instance.dispose();
    }
    await root.delete(recursive: true);
  }
}
