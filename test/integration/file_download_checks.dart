// Real Dart/FFI checks for download selection, progress and shutdown retention.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void _check(bool condition, String message) {
  if (!condition) throw StateError(message);
}

Future<void> checkFileDownloads() async {
  final root = await Directory.systemTemp.createTemp('ltf_downloads_');
  const prefix = 'd4:infod5:filesld6:lengthi16384e4:pathl9:first.mp4ee'
      'd6:lengthi16384e4:pathl10:second.mp4eee4:name8:episodes'
      '12:piece lengthi16384e6:pieces40:';
  final torrent = File('${root.path}/files.torrent');
  await torrent.writeAsBytes([
    ...prefix.codeUnits,
    ...base64Decode('fbZ7rO7ss6QgvzenvspKRRhfjzx5mW+/WQqtnafOL+BICCMzGRO28A=='),
    ...'ee'.codeUnits,
  ]);
  final temporaryTorrent = File('${root.path}/temporary.torrent');
  await temporaryTorrent.writeAsBytes([
    ...prefix.replaceFirst('8:episodes', '8:tempdata').codeUnits,
    ...base64Decode('fbZ7rO7ss6QgvzenvspKRRhfjzx5mW+/WQqtnafOL+BICCMzGRO28A=='),
    ...'ee'.codeUnits,
  ]);
  await LibtorrentFlutter.init(
      listenInterface: '127.0.0.1:0', fetchTrackers: false);
  final engine = LibtorrentFlutter.instance;
  engine.configureSession(const BtConfig(disableDht: true, disableUpnp: true));
  try {
    final id = engine.addTorrentFile(torrent.path, root.path, true);
    final first = engine.startStream(id, fileIndex: 0);
    _check(!engine.hasDownloads(id), 'playback alone created a download');
    _check(engine.getFileDownloads(id).isEmpty,
        'temporary task has selected files');
    var rejected = false;
    try {
      engine.downloadFile(id, fileIndex: 99);
    } on StateError {
      rejected = true;
    }
    _check(
        rejected && !engine.hasDownloads(id), 'invalid selection changed task');
    engine.downloadFile(id, fileIndex: 0);
    engine.downloadFile(id, fileIndex: 0);
    _check(engine.getStreamInfo(first.id) != null, 'download stopped playback');
    final selection = engine.getFileDownloads(id);
    _check(
        selection.length == 1 &&
            selection.single.fileIndex == 0 &&
            selection.single.priority == 4 &&
            !selection.single.isComplete &&
            selection.single.verifiedBytes == 0,
        'download selection/status is wrong');
    final next = engine.switchFile(id, fileIndex: 1);
    engine.stopStream(next.id, keepTorrent: false);
    _check(engine.getFiles(id).length == 2 && engine.hasDownloads(id),
        'legacy stop flag removed download task');

    final episodes = Directory('${root.path}/episodes');
    await episodes.create();
    final retainedFile = File('${episodes.path}/first.mp4');
    await retainedFile.writeAsBytes(List<int>.filled(16384, 65), flush: true);
    engine.recheckTorrent(id);
    final deadline = DateTime.now().add(const Duration(seconds: 10));
    while (!engine.getFileDownloads(id).single.isComplete &&
        DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(const Duration(milliseconds: 50));
    }
    final complete = engine.getFileDownloads(id).single;
    _check(
        complete.isComplete &&
            complete.verifiedBytes == 16384 &&
            complete.progress == 1.0 &&
            engine.hasDownloads(id),
        'completed download lost progress or retention');

    // A separate temporary task is cleaned on shutdown; selected files survive.
    final tempDir = Directory('${root.path}/tempdata');
    await tempDir.create();
    final tempFile = File('${tempDir.path}/first.mp4');
    await tempFile.writeAsBytes(List<int>.filled(16384, 65), flush: true);
    final tempId =
        engine.addTorrentFile(temporaryTorrent.path, root.path, true);
    final tempStream = engine.startStream(tempId, fileIndex: 0);
    _check(
        await engine.waitForStreamStart(tempStream.id,
                timeout: const Duration(seconds: 10)) ==
            StreamStartResult.ready,
        'temporary file was not verified');
    engine.stopStream(tempStream.id);
    _check(await tempFile.exists() && !engine.hasDownloads(tempId),
        'stop deleted temporary files or created a download');
    await engine.dispose();
    _check(
        await retainedFile.exists() &&
            (await retainedFile.readAsBytes()).length == 16384,
        'shutdown deleted selected download');
    _check(!await tempFile.exists(), 'shutdown retained temporary task data');

    // Restart restoration is not automatic in phase one. Re-add at the same
    // path to verify retained bytes; ordinary tasks own full-file selection.
    await LibtorrentFlutter.init(
        listenInterface: '127.0.0.1:0', fetchTrackers: false);
    final reopened = LibtorrentFlutter.instance;
    try {
      final restored = reopened.addTorrentFile(torrent.path, root.path, false);
      _check(
          reopened.hasDownloads(restored) &&
              reopened.getFileDownloads(restored).length == 2,
          'ordinary download did not retain its full-file selection');
      final playback = reopened.startStream(restored, fileIndex: 0);
      _check(
          await reopened.waitForStreamStart(playback.id,
                  timeout: const Duration(seconds: 10)) ==
              StreamStartResult.ready,
          'retained bytes could not be reused');
      reopened.stopStream(playback.id);
      _check(
          reopened.hasDownloads(restored), 'ordinary download erased by stop');
      reopened.disposeAll();
      _check(reopened.getFiles(restored).isEmpty,
          'explicit delete-all retained task');
    } finally {
      await reopened.dispose();
    }
    _check(!await retainedFile.exists(), 'explicit delete-all retained files');
  } finally {
    await engine.dispose();
    await root.delete(recursive: true);
  }
}
