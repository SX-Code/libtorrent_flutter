// Real Dart/FFI lifecycle checks; no public swarm or player is needed.
import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void _check(bool condition, String message) {
  if (!condition) throw StateError(message);
}

Future<void> checkStreamStartWaits() async {
  final directory = await Directory.systemTemp.createTemp('ltf_start_');
  final torrent = File('${directory.path}/episodes.torrent');
  const prefix = 'd4:infod5:filesld6:lengthi16384e4:pathl9:first.mp4ee'
      'd6:lengthi16384e4:pathl10:second.mp4eee4:name8:episodes'
      '12:piece lengthi16384e6:pieces40:';
  await torrent.writeAsBytes([
    ...prefix.codeUnits,
    // SHA-1 of 16384 A bytes, followed by SHA-1 of 16384 B bytes.
    ...base64Decode('fbZ7rO7ss6QgvzenvspKRRhfjzx5mW+/WQqtnafOL+BICCMzGRO28A=='),
    ...'ee'.codeUnits,
  ]);
  await LibtorrentFlutter.init(
    listenInterface: '127.0.0.1:0',
    fetchTrackers: false,
  );
  final engine = LibtorrentFlutter.instance;
  engine.configureSession(const BtConfig(
    disableDht: true,
    disableUpnp: true,
    enableIpv6: true,
  ));
  Timer? ticker;
  try {
    _check(
        await engine.waitForStreamStart(-1, timeout: Duration.zero) ==
            StreamStartResult.invalid,
        'unknown stream must be invalid');
    final id = engine.addTorrentFile(torrent.path, directory.path, true);
    final first = engine.startStream(id, fileIndex: 0);
    var negativeRejected = false;
    try {
      await engine.waitForStreamStart(first.id,
          timeout: const Duration(microseconds: -1));
    } on ArgumentError {
      negativeRejected = true;
    }
    _check(negativeRejected, 'negative timeout was accepted');
    _check(
        await engine.waitForStreamStart(first.id, timeout: Duration.zero) ==
            StreamStartResult.timeout,
        'empty stream should time out without disposal');

    var ticks = 0;
    ticker = Timer.periodic(const Duration(milliseconds: 10), (_) => ticks++);
    final continuing = engine.waitForStreamStart(first.id,
        timeout: const Duration(seconds: 10));
    final short = await engine.waitForStreamStart(first.id,
        timeout: const Duration(milliseconds: 150));
    _check(short == StreamStartResult.timeout, 'short wait did not time out');
    _check(ticks > 0, 'startup wait blocked the Dart event loop');
    _check(engine.streams[first.id]?.url == first.url,
        'timeout invalidated stream ID/URL');
    _check(
        await engine.waitForStreamStart(first.id, timeout: Duration.zero) ==
            StreamStartResult.timeout,
        'same stream cannot be retried after timeout');

    // Complete the same task from a deterministic on-disk fixture. The long
    // waiter must survive another waiter's timeout and see verified bytes.
    await Directory('${directory.path}/episodes').create(recursive: true);
    await File('${directory.path}/episodes/first.mp4')
        .writeAsBytes(List<int>.filled(16384, 65), flush: true);
    engine.recheckTorrent(id);
    final retry = engine.waitForStreamStart(first.id,
        timeout: const Duration(seconds: 10));
    _check(await continuing == StreamStartResult.ready,
        'one timeout cancelled the other waiter');
    _check(await retry == StreamStartResult.ready,
        'same stream did not become ready after timeout');
    _check(
        await engine.waitForStreamStart(first.id, timeout: Duration.zero) ==
            StreamStartResult.ready,
        'already-readable startup did not return ready');
    ticker.cancel();

    final next = engine.switchFile(id, fileIndex: 1);
    _check(
        await engine.waitForStreamStart(first.id, timeout: Duration.zero) ==
            StreamStartResult.invalid,
        'old readable stream survived switch');
    final switchWait = engine.waitForStreamStart(next.id);
    final replacement = engine.switchFile(id, fileIndex: 0);
    _check(await switchWait == StreamStartResult.invalid,
        'switch did not invalidate pending wait');
    _check(
        await engine.waitForStreamStart(replacement.id) ==
            StreamStartResult.ready,
        'switching back lost previously downloaded startup data');
    final retained =
        await File('${directory.path}/episodes/first.mp4').readAsBytes();
    _check(retained.length == 16384 && retained.every((byte) => byte == 65),
        'switch changed the downloaded file');
    engine.stopStream(replacement.id, keepTorrent: true);
    _check(
        await engine.waitForStreamStart(replacement.id) ==
            StreamStartResult.invalid,
        'stop did not invalidate ready stream');
    // The second file remains missing, so removal invalidates a pending wait.
    final removable = engine.startStream(id, fileIndex: 1);
    final removeWait = engine.waitForStreamStart(removable.id);
    engine.removeTorrent(id, deleteFiles: true);
    _check(await removeWait == StreamStartResult.invalid,
        'removal did not invalidate pending wait');

    final lastId = engine.addTorrentFile(torrent.path, directory.path, true);
    final last = engine.startStream(lastId, fileIndex: 0);
    final disposeWait = engine.waitForStreamStart(last.id);
    await engine.dispose();
    _check(await disposeWait == StreamStartResult.invalid,
        'dispose did not invalidate pending wait');
    _check(
        await engine.waitForStreamStart(last.id) == StreamStartResult.invalid,
        'disposed engine accessed the destroyed native session');
    await LibtorrentFlutter.init(
        listenInterface: '127.0.0.1:0', fetchTrackers: false);
    _check(
        await engine.waitForStreamStart(last.id) == StreamStartResult.invalid,
        'old engine wait attached to a new native session');
  } finally {
    ticker?.cancel();
    if (LibtorrentFlutter.isInitialized)
      await LibtorrentFlutter.instance.dispose();
    await directory.delete(recursive: true);
  }
}
