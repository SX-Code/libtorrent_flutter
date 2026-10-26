// FFI smoke test independent of public trackers or Internet availability.
import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

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
}
