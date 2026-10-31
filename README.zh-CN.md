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

// 只停止播放，保留任务和文件，已选择缓存的集继续下载。
engine.stopStream(streamId);
engine.stopAllStreamsForTorrent(torrentId);
// keepTorrent 仅保留参数兼容，即使传 false 也不再删除任务。
```

`bufferPct` 的范围是 0～1，显示百分比时乘以 100；它表示前向缓冲窗口的完成比例，不是整个文件的下载进度。

`readHead` 跟随第一个活动 HTTP GET；该请求结束后，由另一个活动 GET 接管。它不是播放器已经解码到的播放位置。播放器在首帧之前可能为了读取容器元数据或索引而请求多个位置。

每个 GET 独立持有下载窗口。重叠分片采用最高优先级和最早 deadline；请求完成或断开时，只撤回该请求自己的需求。HEAD 只返回响应头，不改变读取位置、就绪状态或下载调度。

Range 位置变化不会再自动产生 `seeking` 状态，因为服务端无法仅凭 Range 区分容器探测和用户拖动。实际播放、缓冲和拖动状态应以播放器事件为准。

如需在不修改 `media_kit` 的情况下诊断起播卡顿，可以在启动应用前启用原生 HTTP 日志。在 Xcode 的 **Product → Scheme → Edit Scheme → Run → Arguments → Environment Variables** 中设置 `TB_LOG_FILE=stderr`。控制台将输出 stream/reader ID、GET/HEAD、Range、解析后的偏移、等待分片以及响应完成／中断信息。

也可以将 `TB_LOG_FILE` 设置为可写文件的绝对路径；iOS 上应使用应用沙盒内的路径。环境变量必须在原生初始化前设置，每个进程只读取一次。默认不输出这些日志。需要使用更新源码重新编译原生库／XCFramework，并完整重启；Flutter 热重载不足以生效。

### 合集切集与上传

`streamOnly` 是第三个位置参数，传 `true` 表示按需下载。切集使用 `switchFile`，替换旧流需求并保留 torrent、连接和已下载数据。已经下载并校验的分片可以在播放期间上传，当前集下载完成后也能继续上传。

```dart
final torrentId = engine.addMagnet(magnetUri, null, true);
// 等待元数据，从 engine.getFiles(torrentId) 中选择下标。
final first = engine.startStream(torrentId, fileIndex: firstFileIndex);
// 播放 first.url。

// 切集前，先让播放器停止使用旧 URL。
final next = engine.switchFile(torrentId, fileIndex: nextFileIndex);
// switchFile 停止旧流并调整优先级，不必另调 stopStream。
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
    // 重试等待同一个 ID 即可；再次 switchFile 会替换流及 URL。
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

`startStream` 只开流，不隐式停掉旧流或删除数据。两种下载模式都允许同时播放不同文件，因此重复开流可能使下载文件累积。使用 `switchFile` 可以停止全部旧流，替换为一个新流。

`switchFile` 复用已有 torrent handle、调度器、peer 连接和已校验的磁盘分片（包括 partfile 数据）。它撤销旧流的优先级和 deadline，并优先调度新文件的启动分片。既没有播放需求、也没有完整下载需求的缺失分片优先级归零；已校验的分片仍可上传。已在传输中的数据块可能继续到达。切集不会暂停、删除、重新校验或重建 torrent。

此前下载的集和边界分片会保留在磁盘上，切回时可以复用。`maxCacheBytes` 只限制内存分片缓存，切集不再淘汰磁盘数据。`switchFile` 要求明确、非负的文件下标，在停止旧流之前校验；切到同一个文件也会创建新流和 URL，但复用已下载数据。两种 `streamOnly` 模式采用相同切集行为，已选择缓存的集在撤销播放需求后仍保留下载基础优先级。

切集直接调用 `switchFile`，不必另调 `stopStream`。两个接口都保留磁盘数据；停止播放只撤销播放需求，没有活动流时，已选择缓存的文件仍会继续下载。

### 播放途中缓存当前集

添加磁力时应使用应用持久目录作为 `savePath`。以下接口需要元数据；`downloadFile` 不会迁移已有文件。

```dart
// 用户点击“缓存当前集”，重复调用不重复创建下载。
engine.downloadFile(torrentId, fileIndex: currentIndex);

// 切集后，上一集仍继续完整下载。
final next = engine.switchFile(torrentId, fileIndex: nextIndex);

// 文件进度只统计已校验的字节；progress 为 0..1。
final downloads = engine.getFileDownloads(torrentId);
for (final file in downloads) {
  print('${file.fileIndex}: ${file.progress * 100}% 完成=${file.isComplete}');
}

// 退出播放；已经下载完成的缓存也算需要保留的下载需求。
engine.stopStream(next.id);
if (!engine.hasDownloads(torrentId)) {
  engine.removeTorrent(torrentId, deleteFiles: true);
}
```

C++ 按文件记录完整下载需求，独立于播放流。缓存文件的所有缺失分片保持基础优先级 4，播放急需分片可提升到 7 并设置 deadline。拖动、切集、停流只撤销播放需求，回退到下载基础优先级而不是归零。优先级不保证固定带宽。跳过的中间部分仍会补齐，所有所需分片校验完成才算文件下载完成；跨文件边界分片的进度只统计属于该文件的字节。

`streamOnly=false` 的普通下载任务在播放时也保留原文件选择。`setFilePriorities` 替换完整下载选择（需先停流），设为 0 只取消选择，不删除数据；播放期间可用 `downloadFile` 追加缓存文件，将其基础优先级至少提高到 4。完成后的文件仍保留下载选择，直到取消选择或显式删除。这个 torrent 中临时播放过的其他集也仍在磁盘上，暂不实现分集物理删除或磁盘容量自动淘汰。

**清理兼容性变更：** `stopStream`、`stopAllStreamsForTorrent` 不再删除任务和文件。`keepTorrent` 仅保留参数兼容，即使传 `false` 也会被忽略。原先依赖停流自动清理的调用方，需要改为显式调用 `removeTorrent`。`dispose()` 保留下载任务文件、删除临时任务；`disposeTorrent`、`disposeAll` 仍是显式删除入口，也会删除用户选择的缓存文件。播放器需单独释放；libtorrent 文件删除是异步操作。

### 按文件暂停与恢复缓存

```dart
try {
  engine.pauseFileDownload(torrentId, fileIndex: episodeIndex);
} on FileDownloadInUseException {
  // 提示：该集正在播放，无法暂停。
}
final paused = engine.getFileDownloads(torrentId)
    .firstWhere((file) => file.fileIndex == episodeIndex).isPaused;
engine.resumeFileDownload(torrentId, fileIndex: episodeIndex);
```

暂停保留文件选择、校验进度和已有数据，`hasDownloads` 仍为 true，只撤销目标文件的后台分片需求，不影响合集其他文件。Native 在生命周期锁内检查目标文件的播放流，仍有流时拒绝暂停；缓冲中或播放器暂停但未 `stopStream` 也属于占用，全部对应流停止后才能暂停。整组暂停由应用逐文件调用，并报告播放占用项，不能把部分成功显示成全部暂停。

两个接口均要求文件已经选中缓存，支持重复调用；再次 `downloadFile` 不会恢复已暂停文件。`resumeFileDownload` 也会恢复整个 torrent 的暂停状态。`setFilePriorities` 替换选择时会清除文件暂停状态。先暂停缓存再启动播放仍可播放，后台暂停保持不变；共享分片和在途请求仍可能使暂停文件的进度增加。完成判断使用 `FileDownloadInfo.isComplete`，不能根据 torrent 整体完成状态判断暂停文件已完成。

配置断点持久化后，文件暂停状态可保存并在重启后恢复。新版 resume 格式为 v2，兼容读取 v1（原有选择默认启用），旧原生库不能读取 v2。需同步重编译原生库以匹配新增导出和状态字段。

### 取消文件缓存

```dart
engine.cancelFileDownload(torrentId, fileIndex: episodeIndex);
await engine.saveDownloadState(stateDirectory); // 等待取消状态持久化
```

取消会撤销指定文件的缓存选择并清除暂停状态，`getFileDownloads` 不再返回该文件，不影响当前播放、其他文件选择和共享分片。与暂停不同，目标文件正在播放时也允许取消后台缓存。需要元数据；重复取消有效但未选中的文件是幂等操作，无效文件索引或任务抛出 `StateError`。取消不会恢复整个 torrent 的暂停。再次缓存使用 `downloadFile`，不能通过 `resumeFileDownload` 恢复已取消选择。

取消调用本身不删除已有数据。如果取消的是最后一个选择，`hasDownloads` 变为 false，下次保存会从下载清单移除该任务，正常 `dispose()` 时可能按临时任务清理数据；普通 `streamOnly=false` 任务取消全部文件后也遵循这一规则。需要立即删除时，由应用确认无播放与其他引用后显式调用 `removeTorrent(deleteFiles: true)`。自动保存是异步的，要求退出前确认取消已落盘时应等待 `saveDownloadState`。使用新增 API 前需重编译包含 `lt_cancel_file_download` 的原生库。

### 下载任务持久化与重启恢复

**下载文件和任务记录都应使用应用持久目录**，不要使用系统临时目录：

```dart
await LibtorrentFlutter.init(
  defaultSavePath: downloadDirectory,
  downloadStateDirectory: stateDirectory,
  downloadStateSaveInterval: const Duration(seconds: 30),
);
// init 返回时已完成任务恢复，随后才能访问 instance。
final engine = LibtorrentFlutter.instance;
final errorSubscription = engine.downloadStateErrors.listen((error) {
  debugPrint('下载状态保存失败：$error');
});

// 按 infohash 复用已恢复任务，不受磁力名称、tracker 顺序影响。
final torrentId = engine.findTorrentByMagnet(magnet) ??
    engine.addMagnet(magnet, downloadDirectory, true);
final stream = engine.startStream(torrentId, fileIndex: currentIndex);

engine.downloadFile(torrentId, fileIndex: currentIndex);
await engine.saveDownloadState(stateDirectory); // 需要确认落盘时显式等待

// 退出时先保存下载任务，再销毁原生会话。
await errorSubscription.cancel();
await engine.dispose();
```

如果 `init` 没有配置状态目录，可在**进入播放界面前**调用
`await engine.restoreDownloadState(stateDirectory)`，返回本次会话的任务 ID 列表。
`torrentId` 不保证跨重启稳定；恢复不会重建播放流。重复恢复不会新增重复任务；
若已有相同 v1/v2 infohash 的任务，保留其当前下载选择。播放时复用任务即可，
无需再次 `addMagnet` 创建另一份下载任务。一个引擎会话绑定一个状态目录，
同一目录应仅由一个引擎／进程写入。

`saveDownloadState(directory)` 等待 libtorrent 异步刷盘和恢复数据回调，随后原子替换
`downloads.json`。保存内容包括元数据、下载路径、分片进度、下载文件选择和暂停状态；
不保存播放流、播放 deadline 和纯播放临时任务。任务／下载选择变化后会合并触发保存，
默认每 30 秒定期保存进度。手动调用保存／恢复方法后也会启用自动保存。

`removeTorrent` 会先同步删除持久化记录，再移除原生任务，`deleteFiles: false` 也一样；
并发保存不会让已删除任务重现。`disposeTorrent`／`disposeAll` 使用同一路径。
`stopStream` 保留下载及其记录。记录删除写盘失败时抛出异常，不继续删除任务。

文件缺失、旧恢复数据与磁盘不符时由 libtorrent 按需校验；记录损坏或版本不支持时显式报错，
不会静默覆盖。手动保存和 `dispose()` 通过 Future 抛错（原生快照等待预算为 30 秒），
自动保存失败通过 `downloadStateErrors` 通知。退出前保存失败时引擎仍有效，可重试退出。
进程在保存完成前被杀死，可能丢失最近的任务变更／进度；定期保存不代表 iOS 挂起后仍可持续下载。
下载路径按原值保存，本阶段不处理下载目录／应用容器迁移。

需要重新编译包含恢复与查询接口的原生库／iOS XCFramework，Flutter 热重载无法更新原生行为。

### 清理

```dart
// 清理一个任务：停止关联流、删除任务及文件；任务已不存在时返回 false。
engine.disposeTorrent(id);

// 显式删除全部任务和文件，包括用户选择的缓存。
engine.disposeAll();

// 关闭引擎：保留下载文件、清理临时任务，再销毁 session。
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
