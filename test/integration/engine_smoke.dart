// FFI smoke test independent of public trackers or Internet availability.
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:libtorrent_flutter/libtorrent_flutter.dart';
import 'stream_start_checks.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('startup waits survive timeout and follow stream lifecycle', (tester) async {
    await checkStreamStartWaits();
  });

  testWidgets('native exports, session lifecycle and input error cross FFI',
      (tester) async {
    final directory = await Directory.systemTemp.createTemp('ltf_smoke_');
    try {
      for (var iteration = 0; iteration < 2; iteration++) {
        await LibtorrentFlutter.init(
          listenInterface: '127.0.0.1:0',
          fetchTrackers: false,
        );
        try {
          expect(LibtorrentFlutter.isInitialized, isTrue);
          final engine = LibtorrentFlutter.instance;
          expect(engine.libraryVersion, startsWith('2.1.1'));
          expect(() => engine.addMagnet('not-a-magnet', directory.path),
              throwsException);
          expect(engine.torrents, isEmpty);
          expect(engine.streams, isEmpty);
        } finally {
          await LibtorrentFlutter.instance.dispose();
        }
        expect(LibtorrentFlutter.isInitialized, isFalse);
      }
    } finally {
      await directory.delete(recursive: true);
    }
  });

  testWidgets('current-episode cleanup and direct removal cross FFI', (tester) async {
    final directory = await Directory.systemTemp.createTemp('ltf_episodes_');
    final torrent = File('${directory.path}/episodes.torrent');
    // Two absent files: no public peers or media download are needed.
    final prefix = 'd4:infod5:filesld6:lengthi16384e4:pathl9:first.mp4ee'
        'd6:lengthi16384e4:pathl10:second.mp4eee4:name8:episodes'
        '12:piece lengthi16384e6:pieces40:';
    await torrent.writeAsBytes([
      ...prefix.codeUnits,
      ...List<int>.filled(40, 1),
      ...'ee'.codeUnits,
    ]);
    await LibtorrentFlutter.init(
      listenInterface: '127.0.0.1:0',
      fetchTrackers: false,
    );
    final engine = LibtorrentFlutter.instance;
    try {
      final id = engine.addTorrentFile(torrent.path, directory.path, true);
      expect(engine.getFiles(id), hasLength(2));
      final first = engine.startStream(id, fileIndex: 0);
      final concurrent = engine.startStream(id, fileIndex: 1);
      expect(engine.streams, hasLength(2));
      expect(engine.streams.containsKey(concurrent.id), isTrue);
      expect(engine.streams.containsKey(first.id), isTrue);
      expect(() => engine.switchFile(id, fileIndex: -1), throwsException);
      expect(() => engine.switchFile(id, fileIndex: 99), throwsException);
      expect(engine.streams.containsKey(first.id), isTrue);
      final next = engine.switchFile(id, fileIndex: 1);
      expect(engine.streams.containsKey(first.id), isFalse);
      expect(engine.streams.containsKey(concurrent.id), isFalse);
      expect(engine.streams, hasLength(1));
      engine.stopStream(first.id, keepTorrent: true); // Already stopped by switch.
      expect(engine.getFiles(id), hasLength(2));
      expect(engine.streams.containsKey(next.id), isTrue);
      engine.stopStream(next.id, keepTorrent: true);
      expect(engine.streams, isEmpty);
      expect(engine.getFiles(id), hasLength(2));
      expect(engine.torrents[id]?.isPaused, isTrue);

      final again = engine.startStream(id, fileIndex: 0);
      final sibling = engine.switchFile(id, fileIndex: 1);
      expect(engine.streams.containsKey(again.id), isFalse);
      engine.stopStream(again.id); // A stale old stream ID cannot remove the new one.
      expect(engine.getFiles(id), hasLength(2));
      engine.stopStream(sibling.id); // Default policy removes the last task.
      expect(engine.getFiles(id), isEmpty);
      expect(engine.torrents.containsKey(id), isFalse);

      final removable = engine.addTorrentFile(torrent.path, directory.path, true);
      engine.startStream(removable, fileIndex: 0);
      engine.switchFile(removable, fileIndex: 1);
      engine.removeTorrent(removable, deleteFiles: true);
      expect(engine.streams, isEmpty);
      expect(engine.getFiles(removable), isEmpty);
      engine.removeTorrent(removable, deleteFiles: true);

      // Cleanup must also find a task added before the next status poll.
      final unpolled = engine.addTorrentFile(torrent.path, directory.path, true);
      expect(engine.disposeTorrent(unpolled), isTrue);
      expect(engine.disposeTorrent(unpolled), isFalse);
    } finally {
      await engine.dispose();
      await directory.delete(recursive: true);
    }
  });

}
