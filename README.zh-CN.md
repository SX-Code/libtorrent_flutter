# libtorrent_flutter

[English](README.md) | [简体中文](README.zh-CN.md) · [更新日志](CHANGELOG.zh-CN.md)

基于 **libtorrent 2.1.1** 的 Flutter 原生绑定，内置本地 HTTP 流服务，支持边下载边播放。
当前开发分支将 1.8.7 分支中的有界缓存和滑动下载窗口移植到 2.0.0 源码，面向 iOS、macOS、Android、Linux 和 Windows。

**当前开发版本：2.0.1-dev.1。** 必须使用当前源码重新编译原生库。旧的 1.8.7 二进制与当前 Dart FFI 导出接口不兼容；项目不会再尝试从已删除的上游仓库下载原生库。

验证此分支时，建议使用本地路径依赖：

```yaml
dependencies:
  libtorrent_flutter:
    path: /path/to/libtorrent_flutter-2.0.0
```

在已安装 Xcode、CMake 和 Ninja 的 Mac 上构建 Apple 平台原生库：

```bash
bash scripts/build_apple.sh all
```

**调试期间，GitHub Actions 暂时只构建 iOS，其他四个平台的任务已停用。** 工作流上传构建产物，不自动发布 Release。本地安装方法见[原生构建产物安装说明](prebuilt/README.md)。全平台 CI 和设备验证尚待完成。

## 为什么使用 libtorrent？

libtorrent 2.1.1 提供原生 BT 网络通信和分片调度能力：

- **`set_piece_deadline`**：声明分片的紧急程度，由时限调度器优先安排请求。deadline 是调度目标，不保证在指定时间内完成下载。
- **uTP**：除 TCP 外，还支持基于 UDP 的 peer 传输；实际连通性仍取决于网络和 NAT 配置。
- **按需流式下载**：优先下载起播分片，预取文件头尾，并根据播放器的 HTTP Range 请求调整下载。起播速度受 peer 可用性、带宽和容器结构影响。
- **DHT、PEX、LSD**：支持不依赖 Tracker 的 peer 发现。

## 使用方法

### 初始化

```dart
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

// 在调用其他接口前初始化一次。
// 默认保存到系统临时目录。
await LibtorrentFlutter.init();

// 自定义初始化参数：
await LibtorrentFlutter.init(
  downloadLimit: 5 * 1024 * 1024, // 下载上限：5 MiB/s
  uploadLimit: 1 * 1024 * 1024,   // 上传上限：1 MiB/s
  defaultSavePath: '/my/custom/path',
  fetchTrackers: true,           // 自动补充公共 Tracker，默认 true
  pollInterval: Duration(milliseconds: 500),
);
```

### 添加下载任务

以下是不同的调用方式，按需选择：

```dart
final engine = LibtorrentFlutter.instance;

// 磁力链接：使用默认保存目录
final id = engine.addMagnet('magnet:?xt=urn:btih:...');

// 磁力链接：指定保存目录
final id = engine.addMagnet('magnet:?xt=urn:btih:...', '/downloads');

// 本地 .torrent 文件
final id = engine.addTorrentFile('/path/to/file.torrent');

// 删除任务及下载文件
engine.removeTorrent(id, deleteFiles: true);

// 暂停／恢复
engine.pauseTorrent(id);
engine.resumeTorrent(id);
```

### 监听任务状态

```dart
engine.torrentUpdates.listen((Map<int, TorrentInfo> torrents) {
  final t = torrents[id];
  if (t == null) return;
  print(t.name);           // 任务名称
  print(t.state.label);    // Downloading、Finished、Seeding 等
  print(t.progress);       // 0.0～1.0
  print(t.downloadRate);   // 字节/秒
  print(t.uploadRate);     // 字节/秒
  print(t.numPeers);       // 当前连接的 peer 数量
  print(t.numSeeds);       // 当前连接的完整做种 peer 数量
  print(t.hasMetadata);    // true 时可以获取文件列表
  print(t.totalDone);      // 已完成的字节数
  print(t.totalWanted);    // 当前选中下载的字节数，流式下载时可能变化
});

// 获取当前状态快照
final Map<int, TorrentInfo> current = engine.torrents;
```

按需下载时，`progress == 1` 可能只表示当前选中的下载需求完成，不代表整集或整个合集下载完成，也不能用来判断播放器是否就绪。

### 获取文件列表并开流

```dart
// 等待 hasMetadata == true 后再获取文件列表。
final files = engine.getFiles(id); // List<FileInfo>

for (final f in files) {
  print('[${f.index}] ${f.name}');
  print('大小：${f.size} 字节');
  print('可流式播放：${f.isStreamable}');
}

// 方式一：自动选择最大的可播放文件
final StreamInfo stream = engine.startStream(id);

// 方式二：指定文件下标
final StreamInfo stream = engine.startStream(id, fileIndex: 2);

// 方式三：指定文件下标和内存缓存上限
// maxCacheBytes 限制的是内存，不限制磁盘上的下载文件大小。
// 已读取的内存分片会按缓存策略淘汰；设置缓存大小不能保证播放永不卡顿。
final StreamInfo stream = engine.startStream(
  id,
  fileIndex: 2,
  maxCacheBytes: 500 * 1024 * 1024, // 500 MiB
);

print(stream.url); // http://127.0.0.1:PORT/stream/...
// 将 URL 交给 media_kit、video_player、VLC、mpv 等播放器。
```

### 监听流状态

```dart
engine.streamUpdates.listen((Map<int, StreamInfo> streams) {
  for (final s in streams.values) {
    print('url: ${s.url}');
    print('ready: ${s.isReady}'); // HTTP 读取状态，不是解码器是否就绪
    print('buffer: ${(s.bufferPct * 100).toStringAsFixed(1)}%'); // 前向缓冲窗口比例
    print('fileSize: ${s.fileSize}');
    print('readHead: ${s.readHead}'); // 主 HTTP 读取请求的字节位置
  }
});

// 指定任务是否存在活动流
final bool active = engine.isStreaming(id);

// 获取指定流的状态快照
final StreamInfo? info = engine.getStreamInfo(streamId);

// 默认 keepTorrent=false：停止最后一个流时删除任务及下载文件。
engine.stopStream(streamId);

// 保留任务 ID 和元数据。
// streamOnly 任务在最后一个流停止时会清理已下载的分集数据。
engine.stopStream(streamId, keepTorrent: true);

// 停止某个任务的全部流，并选择是否保留任务。
engine.stopAllStreamsForTorrent(torrentId, keepTorrent: true);
```

`bufferPct` 的范围是 0～1，显示百分比时乘以 100；它表示前向缓冲窗口的完成比例，不是整个文件的下载进度。

`readHead` 跟随第一个活动 HTTP GET；该请求结束后，由另一个活动 GET 接管。它不是播放器已经解码到的播放位置。播放器在首帧之前可能为了读取容器元数据或索引而请求多个位置。

每个 GET 独立持有下载窗口。重叠分片采用最高优先级和最早 deadline；请求完成或断开时，只撤回该请求自己的需求。HEAD 只返回响应头，不改变读取位置、就绪状态或下载调度。

Range 位置变化不会再自动产生 `seeking` 状态，因为服务端无法仅凭 Range 区分容器探测和用户拖动。实际播放、缓冲和拖动状态应以播放器事件为准。

如需在不修改 `media_kit` 的情况下诊断起播卡顿，可以在启动应用前启用原生 HTTP 日志。在 Xcode 的 **Product → Scheme → Edit Scheme → Run → Arguments → Environment Variables** 中设置 `TB_LOG_FILE=stderr`。控制台将输出 stream/reader ID、GET/HEAD、Range、解析后的偏移、等待分片以及响应完成／中断信息。

也可以将 `TB_LOG_FILE` 设置为可写文件的绝对路径；iOS 上应使用应用沙盒内的路径。环境变量必须在原生初始化前设置，每个进程只读取一次。默认不输出这些日志。需要使用更新源码重新编译原生库／XCFramework，并完整重启；Flutter 热重载不足以生效。

### 合集切集与上传

`streamOnly` 是第三个位置参数，传 `true` 表示按需下载。切集使用 `switchFile`，以清理旧集数据。已经下载并校验的分片可以在播放期间上传，当前集下载完成后也能继续上传。

```dart
final torrentId = engine.addMagnet(magnetUri, null, true);
// 等待元数据，从 engine.getFiles(torrentId) 中选择下标。
final first = engine.startStream(torrentId, fileIndex: firstFileIndex);
// 播放 first.url。

// 切集前，先让播放器停止使用旧 URL。
final next = engine.switchFile(torrentId, fileIndex: nextFileIndex);
// switchFile 会停止旧流并清理磁盘数据，不必另调 stopStream。
// 播放 next.url。

// 最终退出时，释放播放器并清理任务。
engine.removeTorrent(torrentId, deleteFiles: true);
```

将新 URL 交给播放器前，可以等待所选文件的首分片完成校验并成功读取。`startStream` 和 `switchFile` 返回的流都适用，等待不会阻塞 Dart 事件循环：

```dart
final result = await engine.waitForStreamStart(
  next.id,
  timeout: const Duration(seconds: 15),
);

switch (result) {
  case StreamStartResult.ready:
    // 确认这仍是用户当前选择的流，再打开 next.url。
    break;
  case StreamStartResult.timeout:
    // 保留 next.id 和 next.url，允许再次等待同一个 next.id。
    // 不要再次 switchFile，否则会清掉刚下载的数据。
    break;
  case StreamStartResult.invalid:
    // 流已被停止、替换、删除，或引擎已销毁。
    // 放弃旧结果，不要再打开这个 URL。
    break;
}
```

| 返回结果 | 含义 | 调用方处理 |
|---|---|---|
| `ready` | 所选文件首分片已校验且成功读取 | 确认流仍是当前选择，再交给播放器 |
| `timeout` | 仅本次等待超时，流仍有效 | 可再次等待同一个 `streamId` |
| `invalid` | 流已失效 | 不再使用旧 ID 和 URL |

等待超时不会停止流、撤回下载需求或影响其他等待者。首分片已校验并成功读取后，再次调用会立即返回 `ready`。超时设为零时只检查一次；负数会抛出 `ArgumentError`。检查顺序是流有效性、首片就绪、等待超时。

`ready` 不保证立即出画面，容器探测可能仍需要其他位置的数据。用户再次切集时，应防止旧异步回调重新打开上一集。此接口要求原生库导出 `lt_check_stream_start`，必须重新编译配套原生库。

磁力链接添加一次即可，切集时复用 `torrentId`。重复 `addMagnet`／`addTorrentFile` 添加同一 torrent 会返回已有 ID，保留原保存路径和状态；`streamOnly` 模式冲突时抛错。这是同一个任务，不是多个独立所有权引用，切换或删除该 ID 会影响它的全部流。

`startStream` 只开流，不隐式停掉旧流或删除数据。两种下载模式都允许同时播放不同文件，因此重复开流可能使下载文件累积。需要清理旧集时，显式使用 `switchFile`。

`switchFile` 会停止该 torrent 的全部流，删除下载数据（包括 libtorrent 的 partfile 中的部分数据），并在新集开始前重建“已拥有分片”状态。任务 ID、元数据、文件列表和发现配置保留；最多保留 64 个已知的主动连接 peer 地址，优先选取已连接的 seed，并交给新 torrent 尝试重连。这些只是重连地址，不是保留旧连接，也不保证 peer 仍可访问。

切集需要重新建立 BT 连接，但无需重新下载磁力元数据。切换后只保留新流及当前集数据，不会因为看了十集就保留十集文件。BT 分片可能跨越文件边界，因此校验当前集边界分片时，可能需要少量相邻文件数据；下次切集时也会清理。磁盘数据策略与 `maxCacheBytes` 的内存限制相互独立。

也可以先停流，再选择下一集：

```dart
engine.stopStream(currentStream.id, keepTorrent: true);
// streamOnly 任务的最后一个流停止后，清理下载数据，只保留暂停的元数据任务和 ID。
final next = engine.startStream(torrentId, fileIndex: nextFileIndex);
```

切集会等待 libtorrent 完成磁盘删除后再启动新的磁盘写入。删除失败或等待超过 30 秒时，不启动新流并抛出异常；解决存储问题后，可用同一个任务 ID 重试 `switchFile`。

`switchFile` 要求明确、非负的文件下标，并在停止旧集之前校验。即便切换到同一个文件，也会清除已下载数据并创建新流。普通下载任务也可以调用它，但同样会替换全部流并清理该 torrent 的下载数据。

最终清理使用 `removeTorrent`，它会停止全部关联流。播放器对象由调用方单独释放。最终删除文件是 libtorrent 的异步操作；切集则会等待删除完成。

**兼容性说明：** `keepTorrent` 默认是 `false`，两种模式下停止最后一个流都会删除任务及文件。`streamOnly=false` 时显式传 `keepTorrent: true` 会保留下载数据；`streamOnly=true` 时只保留元数据和 ID。保留任务不会自动开启全文件后台下载。需要与当前 Dart 包配套的 `lt_stop_stream_ex`、`lt_switch_file` 原生导出；旧二进制不支持这些语义。曾依赖早期自动切集实现的调用方，应将用于切集的 `startStream` 改为 `switchFile`。

### 清理

```dart
// 清理一个任务：停止关联流、删除任务及文件；任务已不存在时返回 false。
engine.disposeTorrent(id);

// 清理全部任务和流。
engine.disposeAll();

// 完全关闭：先 disposeAll，再销毁原生 session。
await engine.dispose();
```

### 速度限制

```dart
engine.setDownloadLimit(2 * 1024 * 1024); // 2 MiB/s
engine.setUploadLimit(512 * 1024);       // 512 KiB/s
engine.setDownloadLimit(0);             // 恢复不限速
engine.setUploadLimit(0);               // 上传恢复不限速
```

这些接口的单位是字节/秒，`0` 表示不限速。默认允许上传且不限速；实际上传量取决于其他 peer 的需求。

## 平台配置

所有平台都需要与当前源码匹配的原生构建产物，安装方式见下文。

**iOS**：确保应用允许访问本地 HTTP 流服务。如果使用了严格的 `NSAppTransportSecurity` 设置或自定义 `NetworkExtension`，请检查 `ios/Runner/Info.plist` 中的网络配置。

**macOS**：在 `macos/Runner/*.entitlements` 中添加：

```xml
<key>com.apple.security.network.client</key>
<true/>
<key>com.apple.security.network.server</key>
<true/>
```

**Android**：在 `AndroidManifest.xml` 中声明网络权限：

```xml
<uses-permission android:name="android.permission.INTERNET" />
```

**Android 后台播放**：系统可能限制或终止后台应用。如果需要持续后台播放，可使用前台服务，并遵循目标 Android 版本的要求：

```dart
// 使用 flutter_foreground_task 或类似插件。
FlutterForegroundTask.startService(
  notificationTitle: '正在播放',
  notificationText: 'BT 引擎运行中',
);
```

## 原生构建产物与离线构建

使用 `.github/workflows/build.yml` 或前述 Apple 构建脚本生成原生库。当前工作流只启用 iOS。按[原生构建产物安装说明](prebuilt/README.md)将文件放入对应目录。

缺少原生库时，默认从以下地址下载：

```text
https://github.com/SX-Code/libtorrent_flutter/releases/download/v<pubspec-version>
```

当前版本对应的 ZIP 构建产物应发布到 `v2.0.1-dev.1` 标签下。需要其他下载地址时，将 `LIBTORRENT_FLUTTER_RELEASE_BASE_URL` 设置为完整的 Release 目录 URL。Gradle、CMake、CocoaPods 和 `scripts/download_prebuilt.sh` 都支持该覆盖配置。

各平台构建优先使用本地已有原生库；显式执行下载脚本会下载全部平台并替换已有文件。原生库必须与当前源码修订匹配，版本号相同并不代表 ABI 一定兼容。

配套 Release 发布前，请自行提供本地构建产物。设置 `LIBTORRENT_FLUTTER_SKIP_DOWNLOAD=1` 可禁用自动下载。iOS、macOS 和 Android 缺少必要产物时会构建失败；Linux 和 Windows 可回退到 CMake，使用已安装且配置为 `deprecated-functions=ON`、`webtorrent=OFF` 的 libtorrent 2.1.1。离线构建还需要提前准备 Flutter 包和原生依赖。

## 工作原理

`torrent_bridge.cpp` 封装 libtorrent 2.1.1，并在各平台编译为静态库或动态库。Dart 通过 FFI 调用引擎，不经过平台通道。

调用 `startStream()` 时：

1. 根据文件大小估算码率，计算 1～5 个关键起播分片。
2. 关键头部分片设置优先级 7，deadline 从 0 ms 开始以 50 ms 递增；头部约 8 MiB 的其余分片为优先级 4；尾部约 8 MiB 为优先级 5、deadline 1000 ms。
3. 在 `127.0.0.1` 的随机空闲端口启动 HTTP 服务，使用独立接收线程。
4. 每个 GET 拥有独立的有界滑动下载窗口。缺片时通过条件变量等待，并定期检查连接关闭和超时；命中内存缓存时无需再次读盘。阻塞当前读取的分片会提升为优先级 7、deadline 0。
5. 多请求需求按最高优先级、最早 deadline 合并，互不取消。磁盘预读最多提前读取两个已有分片，与下载窗口和三片后向保留窗口是不同机制。
6. HEAD 不改变调度。GET 完成或断开时只撤回该 reader 的需求；不从 Range 变化推断用户拖动，拖动状态应由播放器提供。

流 URL 可供支持 HTTP Range 的播放器使用。

## 许可证

GPL v3
