// Real FFI: cancellation survives checkpoints, preserves playback and clears
// pause state without deleting files until the task's normal cleanup boundary.
import 'dart:convert';
import 'dart:io';

import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void _check(bool condition, String message) {
  if (!condition) throw StateError(message);
}

Future<void> checkFileCancellation() async {
  final root = await Directory.systemTemp.createTemp('ltf_cancel_');
  final state = '${root.path}/state';
  final torrent = File('${root.path}/files.torrent');
  const prefix = 'd4:infod5:filesld6:lengthi16384e4:pathl9:first.mp4ee'
      'd6:lengthi16384e4:pathl10:second.mp4eee4:name8:cancelfx'
      '12:piece lengthi16384e6:pieces40:';
  await torrent.writeAsBytes([
    ...prefix.codeUnits,
    ...base64Decode('fbZ7rO7ss6QgvzenvspKRRhfjzx5mW+/WQqtnafOL+BICCMzGRO28A=='),
    ...'ee'.codeUnits,
  ]);
  final payload = File('${root.path}/cancelfx/first.mp4');
  await payload.parent.create();
  await payload.writeAsBytes(List<int>.filled(16384, 65), flush: true);
  Future<LibtorrentFlutter> init() async {
    await LibtorrentFlutter.init(
      listenInterface: '127.0.0.1:0',
      fetchTrackers: false,
      downloadStateDirectory: state,
    );
    final engine = LibtorrentFlutter.instance;
    engine
        .configureSession(const BtConfig(disableDht: true, disableUpnp: true));
    return engine;
  }

  var engine = await init();
  try {
    // Ordinary downloads must also lose the cancelled selection after restart.
    final id = engine.addTorrentFile(torrent.path, root.path, false);
    engine.pauseFileDownload(id, fileIndex: 1);
    await engine.saveDownloadState(state);
    final manifest = File('$state/downloads.json');
    final magnet = (jsonDecode(manifest.readAsStringSync())['downloads'] as Map)
        .keys
        .single as String;
    final stream = engine.startStream(id, fileIndex: 0);
    final capturing = engine.saveDownloadState(state);
    await Future<void>.delayed(const Duration(milliseconds: 1));
    engine.cancelFileDownload(id, fileIndex: 0);
    engine.cancelFileDownload(id, fileIndex: 0);
    await capturing;
    _check(engine.getStreamInfo(stream.id) != null && await payload.exists(),
        'cancel stopped playback or deleted existing bytes');
    final remaining = engine.getFileDownloads(id);
    _check(
        remaining.length == 1 &&
            remaining.single.fileIndex == 1 &&
            remaining.single.isPaused,
        'cancel changed sibling selection/pause');
    var rejected = false;
    try {
      engine.resumeFileDownload(id, fileIndex: 0);
    } on StateError {
      rejected = true;
    }
    _check(rejected, 'resume recreated cancelled file');
    await engine.dispose();
    _check(await payload.exists(), 'remaining selection lost data retention');
    engine = await init();
    final restored = engine.findTorrentByMagnet(magnet)!;
    _check(
        engine.getFileDownloads(restored).length == 1 &&
            engine.getFileDownloads(restored).single.fileIndex == 1 &&
            engine.getFileDownloads(restored).single.isPaused,
        'restart resurrected cancelled file or resumed sibling');
    engine.cancelFileDownload(restored, fileIndex: 1);
    engine.downloadFile(restored, fileIndex: 1);
    _check(!engine.getFileDownloads(restored).single.isPaused,
        're-added file inherited stale pause');
    await engine.saveDownloadState(state);
    final finalCapture = engine.saveDownloadState(state);
    await Future<void>.delayed(const Duration(milliseconds: 1));
    engine.cancelFileDownload(restored, fileIndex: 1);
    await finalCapture;
    _check(
        !engine.hasDownloads(restored) &&
            engine.getFileDownloads(restored).isEmpty &&
            await payload.exists(),
        'last cancellation retained selection or immediately deleted files');
    _check(
        (jsonDecode(manifest.readAsStringSync())['downloads'] as Map).isEmpty,
        'stale checkpoint resurrected last cancelled selection');
    await engine.dispose();
    engine = await init();
    _check(engine.findTorrentByMagnet(magnet) == null,
        'last cancelled task restored after restart');
  } finally {
    await engine.dispose();
    await root.delete(recursive: true);
  }
}
