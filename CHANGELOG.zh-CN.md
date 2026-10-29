# 更新日志

[English](CHANGELOG.md) | [简体中文](CHANGELOG.zh-CN.md)

历史条目描述的是各自版本当时的行为；当前接口和运行方式请以[中文 README](README.zh-CN.md)为准。版本顺序和重复版本号沿用英文记录。

## 2.0.1-dev.1（尚未发布）

- 新增 `saveDownloadState`、`restoreDownloadState` 和按 infohash 查询的 `findTorrentByMagnet`；仅恢复下载任务，包括已完成文件、等待元数据的普通磁力。
- `init` 新增 `downloadStateDirectory`、`downloadStateSaveInterval`；支持变更后自动保存、定期保存和 `downloadStateErrors` 错误通知。等待 libtorrent 恢复数据回调后原子替换记录，不保存播放优先级／deadline。
- 退出前等待保存，保存失败保留引擎供重试；显式删除先移除持久化记录，并发旧快照不能恢复已删除任务。恢复前校验损坏记录，保留已有同磁力任务的选择与暂停状态。需重编译含新增 FFI 接口的原生库。

- 调试期间，GitHub Actions 暂时只构建 iOS，其他平台任务保留但停用。
- 新增 `waitForStreamStart`，返回 `StreamStartResult.ready/timeout/invalid`。每个异步等待独立计时，检查所选文件首分片是否完成校验且可读；超时保留流并允许重试，停流、切集、删除任务或销毁引擎会使旧等待失效。需要新增的原生导出 `lt_check_stream_start`。
- **切集复用任务**：`switchFile` 保留 torrent、peer 连接和已下载分片，撤销旧流需求并提高新集启动分片优先级，不再删除磁盘数据或重建任务；切回可复用旧数据，磁盘占用可能累积。清理策略变更见下方。
- 显式转换 libtorrent 状态到公共 C/Dart 枚举，修复下载中错误显示为 `Finished`、选中分片完成错误显示为 `Seeding` 的问题。
- 重复添加磁力或 `.torrent` 文件、解析到同一原生 handle 时，复用已有 torrent ID。保留原保存路径和状态，拒绝冲突的 `streamOnly` 模式，避免多个任务 ID 共用 handle 导致生命周期策略冲突。
- 阻塞 HTTP 读取的缺失分片会提升为立即 deadline，即使此前已经按较晚的播放 deadline 预取。修复容器探测或播放器缓冲快于估算播放速率时，缺片未及时提升紧急程度的问题。
- 修复启动阶段 Range 探测互相取消：每个 HTTP GET 独立持有并合并下载窗口和 deadline，断开连接只撤回自身需求；HEAD 不改变播放状态或调度。Range 偏移变化不再被当作用户拖动，状态跟随第一个活动 GET。
- 新增可选 HTTP Range、等待和完成日志，通过 `TB_LOG_FILE=stderr` 或可写文件路径启用。原生回归覆盖 HEAD 隔离、并发探测、共享分片传递以及断开／删除后的清理。
- **清理行为变更**：`stopStream` 和 `stopAllStreamsForTorrent` 只撤销播放需求，保留任务和文件；`keepTorrent` 保留兼容但忽略其值。删除需显式调用 `removeTorrent`／`disposeTorrent`／`disposeAll`。
- **显式切集策略**：首次使用 `startStream`，切集使用 `switchFile` 替换全部旧流并保留数据。两种模式的 `startStream` 都允许不同文件并发播放，不会隐式切集或清理。已缓存数据持续可复用和上传。
- **下载与播放合并**：新增 `downloadFile`、`hasDownloads`、`getFileDownloads` 和 `FileDownloadInfo`。C++ 按文件维护完整下载基础优先级，与活动流需求合并；切集、拖动和停流不会取消下载。普通下载任务保留原文件选择，进度只统计校验通过的字节和完整边界分片。
- 按流合并优先级和 deadline，并向相同／不同文件的多个流分发分片事件。生命周期操作串行化；直接 `removeTorrent` 会停止全部关联流，不递归触发删除。
- `dispose()` 保留下载任务文件（包括已完成任务）、删除临时任务；移除旧的停流删除重建路径。下载任务持久化与恢复见本节新增接口。
- 要求配套重编译原生库，新增导出 `lt_download_file`、`lt_has_downloads`、`lt_get_file_downloads`。旧原生 stop 入口也采用仅停流语义。
- 从分支提交 `586a129`、`88eb0ab` 移植有界分片缓存和滑动流窗口，保留独立拥有的缓冲区以及 2.0.0 的 Android／Range 修复。
- 恢复 FFI 文件列表中的文件大小；未提供恢复状态时，校验已有本地文件。
- 串行处理缓存淘汰和元数据／启动优先级修改；流式播放时保留空闲 seed 连接。
- 使用 libtorrent 2.1.1 导出的 ABI 配置重编译，恢复全平台 CI 定义及 iOS/macOS 构建脚本；调试阶段的实际启用范围见本版本首条。
- 要求配套重编译的原生产物或显式 Release URL；macOS 最低版本为 11.0，iOS 为 13.0。
- 新增原生缓存／HTTP 回归测试、FFI 导出检查和不依赖公共网络的 Flutter 冒烟测试。

## 2.0.0

- **Android 16／16 KB 内存页支持**：使用链接参数 `-Wl,-z,max-page-size=16384` 重编译 `liblibtorrent_flutter.so`，适配 Android 16（API 36+）设备和 Google Play Console 要求。
- **修复 Android“无法识别文件格式”**：`parse_range` 曾将尾部 Range（如 `Range: bytes=-500`）错误解析为 `start=0, end=500`，返回文件开头而不是结尾，破坏 `media_kit` 等播放器的 moov／ID3 元数据探测；现已修复。
- **固定磁盘 I/O 后端**：Android 使用 `posix_disk_io`，桌面使用 `mmap_disk_io`，覆盖 libtorrent 2.1.x 默认的 `pread_disk_io`，规避其在 Android 文件系统上的已知可靠性问题。
- **修复 URL／MIME 路由**：将文件名附加到流 URL 路径而不是查询参数中，并补齐 MIME 类型，改善各平台格式识别。

## 1.9.9

- **修复 Android ARM64 的 SEGV_ACCERR**：`on_piece_read` 只从 libtorrent alert 缓冲区复制一次到本地 vector，再用于 `read_results` 和分片缓存。此前二次读取 alert 的 `data` 指针会被 Android MTE 检测为释放后使用。
- **修复 `handle_connection` 中的 TorrReader 泄漏**：移除遮蔽外层变量的 `TorrReader* reader` 重复声明，使 `close_reader` 能正确执行，避免每个 HTTP 连接泄漏 reader。

## 1.9.8

- **修复 `lt_start_stream` 的 Android ARM64 SIGSEGV**：在启动路径调用 `ts.pieces.get_bit()` 前检查 `has_metadata` 和 `pieces.empty()`。Android ARM64 上，`handle.resume()` 后立即查询分片状态可能得到内部缓冲区为空的 bitfield，导致访问地址 `0x4`。

## 1.9.7

- **修复 Android ARM64 SIGSEGV**：`fill_status` 访问 `st.pieces` 前检查空值和元数据；拆分 `lt_get_all_statuses` 中数组下标自增，避免内存访问错误。

## 1.9.6

- **禁用 WebTorrent（`webtorrent=OFF`）**：所有平台关闭 WebTorrent 构建选项，以提高稳定性并减小二进制体积。

## 1.9.5

- **修复 Windows CMake 流程语法**：补齐 `windows/CMakeLists.txt` 中缺失的 `endif()`，解决配置和生成失败。

## 1.9.4

- **修复 Windows／Linux 预编译库解压路径**：ZIP 直接解压到 `prebuilt/windows/x64/`、`prebuilt/linux/x64/` 等架构目录，解决回退到 `FindLibtorrentRasterbar.cmake` 后的构建错误。
- **WebTorrent／OpenSSL 集成**：确认 Windows、Linux、macOS、iOS 和 Android 的完整 WebTorrent（`webtorrent=ON`）及 Mozilla CA 根证书包支持。

## 1.9.3

- **升级到 libtorrent 2.1.1**：启用 WebTorrent（`webtorrent=ON`）、OpenSSL 3.2.1 和已弃用接口。
- **WebTorrent 支持**：各平台内置 `libdatachannel`、`libjuice`、`usrsctp` 等 WebRTC 依赖，覆盖 Linux x64/arm64、macOS universal2、Windows x64/arm64、Android arm64/v7a/x86_64 和 iOS arm64 XCFramework。
- **C++17 兼容性**：将 `torrent_bridge.cpp` 中非标准的 `std::string_view.to_string()` 改为 `std::string(sv)`，修复 Clang/GCC 在 iOS、macOS、Linux、Android 上的编译失败。

## 1.9.2

- 回退到稳定的 libtorrent 1.8.5 基线，修复稳定性和 CI 问题。

## 1.8.5

- 仅更新包元信息以改善 pub.dev 评分，无代码修改。

## 1.8.4

- **运行时调参**：`configureSession()` 现在会将配置立即应用到活动流。此前只更新会话默认值和保存的配置，已有流直到重启才使用新的连接上限。现在持有流锁后重新应用 `cache_size`、`reader_read_ahead` 和 `torrent_handle::set_max_connections(connectionsLimit)`，播放期间调整滑块即可生效。

## 1.8.3

- **修复单任务连接上限未生效**：`BtConfig.connectionsLimit`（默认 25，沿用 TorrServer）此前只保存到缓存结构，未传给 libtorrent，导致任务继承会话上限 500。高 seed 数任务可能建立 200 多条连接并出现队头阻塞。`lt_start_stream`、`lt_set_cache_settings` 现在调用 `torrent_handle::set_max_connections`；会话默认连接上限从 500 降为 200，示例新增 5～200 的单任务连接数滑块。

## 1.8.2

- **磁盘预读**：`serve_range` 在发送当前分片之前，先发起后续 1～2 个分片的磁盘读取。发送结束时，下一片的 `read_piece_alert` 通常已经进入结果表，减少每片约 5～15 ms 的读盘等待，让 socket 和磁盘 I/O 重叠执行，改善慢硬盘、大分片及播放器快速读取时的吞吐。

## 1.8.1

- **Linux 独立二进制**：OpenSSL 静态链接到 `liblibtorrent_flutter.so`，不再要求用户系统安装 `libssl3`／`libcrypto3`。运行时只依赖 glibc 系统常见的 libc、libstdc++、libpthread。CI 在链接后通过 `ldd` 检查，若 NEEDED 中仍出现 libssl/libcrypto 则失败；与其他平台的 OpenSSL 静态链接方式一致。
- **引擎调优**，改善中等规模 swarm 的吞吐：
  - `predictive_piece_announce=500ms`：提前宣告即将完成校验的分片，让其他 peer 更早向本端请求，改善互惠及 unchoke 排序。
  - `allowed_enc_level=pe_both`：支持时协商全流 RC4（MSE），而不只是头部加密，减少明文 BT 受到 DPI 限速的情况。
  - `prefer_udp_trackers=true`：同一主机优先 UDP Tracker；历史记录称 scrape 响应约快 10 倍，并改善冷启动。
  - **公共 Tracker 兜底**：磁力只有 info-hash、没有 Tracker 时，补充 8 个公共 UDP Tracker，减少等待 DHT 冷启动（可能需 30～60 秒）的时间。

## 1.8.0

- **新增三个桌面架构构建产物**：
  - **macOS**：dylib 改为真正的 universal2（arm64+x86_64）。此前 `prebuilt/macos/universal/liblibtorrent_flutter.dylib` 实际仅含 arm64，在 Intel Mac 上无法加载。现在在 `macos-14` runner 上分别构建两套 OpenSSL、libtorrent 和桥接库，再用 `lipo -create` 合并；部署目标为 `10.14`。
  - **Linux arm64**：覆盖树莓派 4/5、AWS Graviton、Asahi Linux 等；在 GitHub 的 `ubuntu-22.04-arm` 原生 runner 上生成 `linux-native-lib-arm64`，与 x64 组成构建矩阵。CMake 按 `CMAKE_HOST_SYSTEM_PROCESSOR` 选择架构（`aarch64` → arm64）。
  - **Windows arm64**：覆盖 Snapdragon X、Surface Pro X；在 `windows-2022` 上使用 vcpkg 的 `arm64-windows-static` 和 `cmake -A ARM64` 交叉编译。CMake 通过 `CMAKE_GENERATOR_PLATFORM`／`PROCESSOR_ARCHITECTURE` 选择架构。
- **Release 文件名调整**：
  - `linux-native-lib.zip` 拆为 `linux-native-lib-x64.zip` 和 `linux-native-lib-arm64.zip`。
  - `windows-native-lib.zip` 拆为 `windows-native-lib-x64.zip` 和 `windows-native-lib-arm64.zip`。
  - `macos-native-lib.zip` 不变，内含一个 universal2 dylib。
  - Android 的 `android-native-lib-<abi>.zip` 和 iOS 的 `ios-native-lib.zip` 名称不变。
- 移除仓库中 `prebuilt/macos/universal/` 下仅含 arm64 的旧 dylib，使后续构建下载真正的 universal2，而不误用旧缓存。

## 1.7.10

- **包体调整**：pub.dev 包不再携带约 200 MB 的预编译原生库，避免超过 100 MB 限制。首次构建时，平台脚本按依赖解析出的包版本从 GitHub Release 下载：
  - **Android**：`android/build.gradle` 下载 arm64-v8a、armeabi-v7a、x86_64 的 `android-native-lib-<abi>.zip` 到 `prebuilt/android/<abi>/`。可用 `-PlibtorrentFlutterAbis=arm64-v8a` 限定架构，或 `-PlibtorrentFlutterSkipDownload=true` 禁用下载。
  - **Windows**：`windows/CMakeLists.txt` 下载 `windows-native-lib.zip` 到 `prebuilt/windows/x64/`；可用 `-DLIBTORRENT_FLUTTER_SKIP_DOWNLOAD=ON` 禁用。
  - **Linux**：`linux/CMakeLists.txt` 下载 `linux-native-lib.zip` 到 `prebuilt/linux/x64/`，禁用方式同 Windows。
  - **macOS**：podspec 下载 `macos-native-lib.zip`，将 dylib 放在 podspec 旁；环境变量 `LIBTORRENT_FLUTTER_SKIP_DOWNLOAD=1` 可禁用下载。
  - **iOS**：podspec 下载 `ios-native-lib.zip`，将 XCFramework 解压到 podspec 旁；禁用方式同 macOS。
  - 当时的记录说明：下载失败或禁用下载时回退源码构建，可自行提供 `prebuilt/` 文件或使用 CMake/Gradle 构建路径；当前各平台的回退条件请看 README。

## 1.7.9

- **iOS 加密**：XCFramework 启用 MSE/PE，与 Android 1.7.6+ 对齐。此前 `-Dencryption=OFF`、`-DTORRENT_USE_SSL=0` 使加密设置无效，无法连接拒绝明文连接的 seedbox。启用后可连接更多 peer，改善冷启动和持续下载。
- **iOS 构建流程**：为设备 arm64（`ios64-xcrun`）和模拟器 arm64/x86_64（`iossimulator-xcrun`）静态交叉编译 OpenSSL 3.2.1。libtorrent CMake 使用 `-DOPENSSL_ROOT_DIR`、`-Dencryption=ON`；桥接编译移除 `-DTORRENT_USE_SSL=0`，与安装后的 `config.hpp` 中 `TORRENT_USE_OPENSSL`／`TORRENT_USE_LIBCRYPTO` 保持一致，避免 ABI 不匹配导致加密失效。
- **iOS 优化**：三套架构的桥接编译由 `-O2` 升为 `-O3 -flto`，改善分片校验和事件分发。
- **iOS 静态链接**：将 `libssl.a`、`libcrypto.a` 的对象合并到各架构的 `liblibtorrent_flutter.a`，下游仍只需链接一个静态库。
- **App Store 历史说明**：当时记录认为 BT 的 MSE 属于美国 EAR §740.17(b)(1) 的互操作标准加密豁免，可在 `Info.plist` 声明 `ITSAppUsesNonExemptEncryption=false` 并免于年度自分类报告。此条保留历史发布说明，不代表对当前应用申报适用性的判断。
- **流式播放**：`serve_range` 优先级管线从当前片+2 扩展到当前片+16，deadline 间隔 80 ms，保持 peer 请求队列充足；当前片仍拥有最早 deadline。
- **拖动**：HTTP 拖动处理简化为 `clear_piece_deadlines()` 加目标片 `set_piece_deadline(seek_piece, 0)`，后续由正常 16 片梯度调度接管；移除旧版 4 片、300 ms 间隔造成的约 1.2 秒额外等待及重复重建 picker。
- **设置**：`request_queue_time` 从 3 秒降为 1 秒，让优先级变化后在途请求更快消退。
- **设置**：`whole_pieces_threshold` 从 20 改为 0，允许同一分片的块从多个 peer 并行下载，改善多 peer 场景的拖动响应。
- **设置**：`piece_extent_affinity` 从 false 改为 true，让同一个 peer 尽量下载连续区域，减少分片完成时间波动。
- **设置**：`strict_end_game_mode` 从 true 改为 false，允许对未完成分片尾部的块发起重复请求，避免单个慢块拖住整片。
- **设置**：`max_queued_disk_bytes` 从 16 MB 增至 64 MB，容纳拖动后多个 peer 同时到达的突发写入。

## 1.7.8

- **回退**：撤销 1.7.6～1.7.7 的流式引擎改动，包括扩大优先级窗口、预读调整、`notifySeek` 和 FIN 检测，回到 1.7.4 的流式逻辑，并保留 1.7.5 的 `is_ephemeral` 修复。
- **保留**：1.7.6 的 Android 加密构建（OpenSSL 3.2.1、`-Dencryption=ON`、`-O3 -flto`）。

## 1.7.7

- **拖动**：新增 `notifySeek()`，Dart 可直接通知原生引擎，绕过 HTTP 推断。mpv 等播放器可能持有约 150 MB 的解复用缓存，缓存内跳转不发新 Range 请求，旧实现因此可能延迟 25 秒以上；新接口用于缩短这类响应时间。
- **拖动**：HTTP 发送循环增加非阻塞 FIN/RST 检测，使用 `WSAPoll`／`poll` 和 `MSG_PEEK recv` 检测播放器关闭连接，改善缓存外拖动恢复。
- **拖动**：发生拖动时，将非尾部分片优先级重置为 `dont_download`，避免旧位置与新位置争抢带宽。
- **流式播放**：优先级前瞻从当前片+2 扩大到当前片+8，前 3 片优先级 6，其余为 4，并扩大 deadline 间隔，在保持下载管线充足的同时优先服务当前片。
- **流式播放**：磁盘预读从 3 片扩展到 5 片，减少分片间 I/O 等待。

## 1.7.6

- **Android 构建**：启用 `-Dencryption=ON`，静态链接各 ABI 的 OpenSSL 3.2.1，使 `pe_enabled`／`pe_forced` 能协商加密连接，改善偏好或要求加密的 swarm 中的 peer 可用性。
- **Android 构建**：为 arm64-v8a、armeabi-v7a、x86_64 交叉编译 `libssl.a`、`libcrypto.a`，设备无需额外提供 OpenSSL 运行库。
- **Android 构建**：从 `-O2` 升级为 `-O3` 和链接时优化 `-flto`，提高分片校验、事件处理和流式吞吐。
- **Android 构建**：移除桥接层的 `-DTORRENT_USE_SSL=0`，使 C++ 加密策略不再被忽略。
- **Android 构建**：armeabi-v7a 的 OpenSSL 交叉编译增加 `-fPIC`，修复 32 位 ARM 汇编重定位链接错误。
- **流式播放**：每片内存复制由 3 次减至 1 次，`ReadResult` 使用 `shared_ptr<vector<char>>`，减少 alert、缓存和 socket 之间的重复复制。
- **流式播放**：优先级前瞻从当前片+2 扩大到当前片+5，让更多 peer 提前下载，减少不稳定连接下的停顿。
- **流式播放**：磁盘预读从 1 片扩展到 3 片，在发送当前片时预读后续已下载分片。
- **流式播放**：socket 发送缓冲从 2 MB 增至 8 MB，减少高速网络上的内核缓冲等待。

## 1.7.5

- **修复**：`metadata_received_alert` 增加 `is_ephemeral` 判断，只对流式任务在元数据到达后暂停／清零优先级，不再影响普通下载。

## 1.7.4

- **流式播放**：移除 `sequential_download`，完全由 `set_piece_deadline` 驱动分片顺序，改善拖动恢复和 swarm 利用率。
- **流式播放**：增加热点分片缓存，`on_piece_read` 填充内存 `CachePiece`，`read_piece_data` 优先查缓存，减少容器探测、重叠 Range 和小幅回退的重复读盘。
- **流式播放**：保留最近播放的 3 个分片，不立即降为 `dont_download`，支持播放器重读和小幅回退。
- **流式播放**：根据文件大小估算码率并调整起播分片数和缓冲显示。1.2 GB 文件约使用 2 个关键分片，4 GB 文件为 1～2 个，替代固定 5 个的方式。
- **流式播放**：尾部（moov）优先级从 `top_priority` 降为 5，deadline 为 1000 ms；头部起播片优先级 7、deadline 0～100 ms，以优先调度头部。
- **拖动**：清空后向保留窗口，将旧已播放分片立即设为 `dont_download`，释放调度资源给新位置。

## 1.7.3

- **崩溃修复**：关闭流时，HTTP 工作线程仍在使用 `TorrReader` 和互斥锁，`cache->close()` 却提前销毁它们，导致 `SIGABRT`。现在先等待线程结束，再关闭缓存。
- **修复**：缓存已经关闭时，`close_reader()` 跳过清理，避免重复释放 reader 互斥锁。
- **修复**：`read_piece_data()` 的等待条件检查 `active`，关闭时立即唤醒，不再最多阻塞 10 秒。
- **修复**：`lt_destroy_session` 不再持有 `streams_mu` 等待线程退出，避免与 alert 线程死锁。
- **健壮性**：线程入口、关闭路径和缓存清理增加 try-catch，覆盖 `handle_connection`、客户端线程、`lt_stop_stream`、`lt_destroy_session`、`close_reader` 和 `TorrCache::close`，减少意外关闭竞争造成的异常退出。

## 1.7.2

- **崩溃修复**：停止流时，已 detach 的 HTTP 线程仍访问已销毁 `StreamEngine` 的互斥锁，导致 `pthread_mutex_destroy called on a destroyed mutex` 和 `SIGABRT`。现在跟踪并等待客户端线程退出，同时强制关闭 socket。
- **修复**：避免关闭流时重复关闭 HTTP 监听 socket。
- **修复**：`lt_destroy_session` 在销毁流引擎前正确关闭监听 socket 并等待客户端线程结束。

## 1.7.1

- **流式播放**：以 lt2http 风格的直接存储读取替换 TorrServer 异步缓存管线，减少中间 RAM 缓存路径造成的拖动延迟。
- **流式播放**：优先级窗口从 25 片梯度缩小到当前片+2，让带宽更集中于播放器当前需求。
- **流式播放**：缓存全部用于前向数据，积极淘汰已播放分片，使 64 MB 缓存也能播放 60 GB 文件。
- **修复**：`clear_priority_impl()` 曾在没有 reader 时将全部分片设为 `dont_download`，导致播放器 HTTP 请求间隙下载被中止；现已修复。
- **修复**：保护当前读取片及下一片不被缓存淘汰，避免分片边界处反复淘汰和下载。
- **修复**：移除每次分片读写都启动的 `clean_pieces()`／`get_removable_pieces()` 分离线程，避免覆盖 `serve_range` 优先级并中止下载。
- **Windows 构建**：DLL 从 9.4 MB 缩小至 146 KB。

## 1.7.0

- **macOS 修复**：原 dylib 通过 `/opt/homebrew/opt/openssl@3/lib/...` 硬编码引用 `libssl.3.dylib`／`libcrypto.3.dylib`，用户设备不存在该路径时启动崩溃。现改为 `@loader_path/`，并随插件提供 OpenSSL。
- **macOS 构建**：CMake 增加 `install_name_tool` 的 POST_BUILD 步骤，自动改写 OpenSSL 加载路径。
- **macOS 构建**：新增 `build_macos.sh`，一键构建 dylib、复制 Homebrew OpenSSL、修复动态库互相引用，并放入 `macos/`、`prebuilt/macos/universal/`。
- **macOS 打包**：podspec 同时包含主 dylib、`libssl.3.dylib` 和 `libcrypto.3.dylib`。
- **流式播放**：拖动时为 5 个分片设置 deadline 0，取消 unchoke 数量限制，并优化 `cancel_non_critical()` 时机。
- **引擎**：`unchoke_slots_limit` 从 4 改为 -1（不限），修复连接很多 peer 却只允许少数上传的问题。

## 1.6.9

- **引擎**：重写 C++ 流式引擎，包括优先级系统、HTTP 服务和分片管理。
- **流式播放**：采用五级优先级梯度（NOW=7、NEXT=6、READAHEAD=5、BACK=1、SKIP=0），只下载播放窗口及头尾元数据。
- **流式播放**：使用 `set_piece_deadline()` 进行紧急下载，可从多个 peer 请求分片并取消慢请求。
- **流式播放**：自适应预读窗口为 3～50 片，流畅播放时扩大，拖动时重置。
- **流式播放**：保留 8 片后向缓冲，支持快速回退。
- **流式播放**：通过 `maxCacheBytes` 配置 RAM 分片缓存，从智能电视的 128 MB 到桌面的 2 GB；滑动淘汰时保护播放位置附近的安全区。
- **拖动**：HTTP 连接由独立线程处理，新连接可关闭旧 socket 进行抢占，不阻塞 accept 循环。
- **拖动**：调用 `clear_piece_deadlines()` 停止旧位置下载，转向新目标。
- **拖动**：新位置设置 6 片、200 ms 间隔的紧急 deadline。
- **拖动**：`seek_generation` 计数器用于快速终止旧的分片等待。
- **性能**：使用条件变量等待分片，避免忙轮询。
- **性能**：通过 `piece_finished_alert` 跟踪分片完成，不依赖状态轮询。
- **性能**：缓存保护播放位置附近 5 片，拖动时保留附近已有缓存。
- **调优**：`max_failcount=3`，减少拖动过程中 peer 被过早丢弃。
- **调优**：每 30 秒进行 5% 的 `peer_turnover`，更快替换慢 peer。
- **调优**：启动时使用 `connection_speed=200`、`torrent_connect_boost=200` 加快连接建立。
- **API**：`startStream()` 新增 `maxCacheBytes`，0 表示默认约 128 MB。
- **API**：为 `StreamInfo` 增加向后兼容的 `bufferPct` getter。
- **API**：新增 `StreamState`（idle、buffering、ready、seeking、error）以及 `StreamInfo.streamState`。
- **API**：`StreamInfo` 新增 `bufferSeconds`、`bufferPieces`、`readaheadWindow`、`activePeers`、`downloadRate`。
- **兼容性**：保留 `addMagnet()`、`startStream()`、`stopStream()`、`disposeTorrent()` 等已有接口。

## 1.6.8

- **iOS**：XCFramework 包含设备 arm64-iphoneos 和模拟器 arm64+x86_64-iphonesimulator，覆盖 Apple Silicon 和 Intel Mac 模拟器。
- **iOS**：用 `.xcframework` 替换胖 `.a`，采用 Apple 推荐的多平台静态库封装方式。
- **iOS**：podspec 改为 `vendored_frameworks`，按 SDK 设置 `-force_load` 链接参数。
- **CI**：分别编译设备 arm64、模拟器 arm64、模拟器 x86_64 的 libtorrent 和 torrent_bridge，再通过 `xcodebuild -create-xcframework` 打包。
- **发布**：从 pub 包移除 Android x86_64 预编译库，以满足 100 MB 包体限制。

## 1.6.6

iOS 修复。

## 1.6.6

- **iOS 修复**：podspec 添加 `SystemConfiguration` 框架依赖，解决 Release 构建中 `Undefined symbol: _SCNetworkReachabilityCreateWithAddress` 链接错误。

## 1.6.5

- **关键修复**：并发的头尾 HTTP 连接曾覆盖全局请求 ID 并互相中止，使 VLC、mpv、ExoPlayer 等多连接播放器不断循环。改用 seek-generation 计数器，仅在识别到拖动时中止旧连接。
- **修复**：尾部／元数据 Range 请求不再抢占 `read_head`，优先级循环保持关注播放位置。
- **流式播放**：新增 48 项的 LRU 分片缓存，减少近期分片的重复读盘。
- **流式播放**：开流时预取头尾；前 5 片使用交错 deadline，尾部约 512 KB 用于 MKV cues、MP4 moov 等容器元数据。
- **流式播放**：动态预读窗口为 8～40 片，根据下载速度调整，目标缓冲约 15 秒。
- **流式播放**：响应中使用 6 片交错 deadline 前瞻。
- **拖动**：清理过期缓存但保留尾部缓存。
- **拖动**：目标附近队列从 2 片扩大到 5 片。
- **引擎**：修复 `suggest_mode` 错用布尔值，改用正确枚举。
- **引擎**：修复 `torrent_connect_boost` 超过文档上限 255。
- **引擎**：`mixed_mode_algorithm` 设为 `peer_proportional`，避免 NAT 后的 uTP peer 得不到调度。
- **引擎**：增加 DHT 引导节点、`piece_extent_affinity` 和 `auto_sequential`。
- **引擎**：调整发送缓冲水位、peer 替换及 socket 缓冲大小。
- **HTTP 服务**：增加 DLNA 响应头，兼容智能电视。
- **HTTP 服务**：支持 CORS OPTIONS 预检。
- **HTTP 服务**：发送缓冲 2 MB，单次发送块 1 MB。
- **HTTP 服务**：Range 请求头解析不区分大小写。
- **HTTP 服务**：优先级循环间隔自适应，缓冲时 100 ms，稳定时 250 ms。

## 1.6.4

- **关键修复**：服务端曾返回 `Connection: keep-alive`，却在每次请求后关闭 socket；VLC 复用已关闭连接后发生 broken pipe 并黑屏。改用 `Connection: close`，明确每个 Range 请求使用新连接。
- **HTTP 服务**：HEAD 返回响应头后正常关闭，不再进入数据传输路径。
- **HTTP 服务**：移除单次连接处理外多余的 `while` 循环，明确“一连接一请求”的模型。

## 1.6.3

- **流式播放**：起播 deadline 改为 0、500、1000 ms 等交错时限，不再全部为 0，使调度优先集中到首片，而非等待 8 片。
- **流式播放**：关键窗口从 6 片降至 2 片，热点窗口从 9 降至 5，预读从 15 降至 6，减少非紧急请求分散带宽。
- **流式播放**：缓冲百分比改按 2 片关键窗口计算，不再等待 6 片才报告就绪。
- **拖动**：改为 2 片接近零的交错 deadline，再加 2 片以 200 ms 交错；替代 4 片同时 deadline 0 的方式。
- **拖动**：冷却时间从 1000 ms 缩短到 100 ms，优先级循环更快恢复。
- **拖动**：等待超时从 120 秒降至 30 秒，更快结束无响应 peer 的等待。
- **性能**：优先级循环从每 200 ms 改为每 100 ms。
- **性能**：libtorrent 的 `tick_interval` 从默认 500 ms 改为 100 ms，加快调度响应。
- **HTTP 服务**：响应中的前瞻从 5 片缩为 3 片，deadline 间隔从 10 ms 改为 100 ms，减少与当前片的带宽竞争。

## 1.6.2

- **关键修复**：缓存淘汰不再调用 `force_recheck()`，避免整 torrent 重新校验并把正在播放的分片也标为未知，导致流中断。
- **流式播放**：预读窗口上限为缓存容量的 50%，避免下载过远、缓存溢出并淘汰当前位置数据。
- **流式播放**：即使缓存压力较大，也保护当前读取位置附近 5 片不被淘汰。

## 1.6.1

- **流式播放**：每次 `wait_for_piece` 前，`serve_range` 先为后续 5 片设置优先级 7 和交错 deadline，使拖动后能并行下载，避免逐片串行等待。
- **流式播放**：拖动冷却从 3 秒降为 1 秒；`serve_range` 的前瞻覆盖冷却间隙，优先级循环可更早恢复较宽窗口。

## 1.6.0

- **流式播放**：采用 torrest-cpp 的优先级只升级模式重写调度，热点窗口使用 `i*10ms` 的交错 deadline。
- **流式播放**：关键窗口从 3 片扩大到 5 片，deadline 为 0 ms，改善起播。
- **流式播放**：拖动时清空全部分片优先级，仅对目标及后续 3 片设置 deadline 0，3 秒冷却后恢复优先级循环。
- **流式播放**：`wait_for_piece` 超时从 15 秒增至 120 秒，减少慢启动时的“Stream ends prematurely”。
- **流式播放**：预读从 30 片缩为 15 片，集中下载当前位置附近的数据。
- **引擎**：增加 `no_recheck_incomplete_resume`，跳过恢复时的文件重新校验，缩短启动时间。
- **引擎**：增加 `allow_multiple_connections_per_ip`，支持 seedbox、VPN 和共享 NAT 下的多个 peer。
- **引擎**：设置 `peer_connect_timeout=3s`，加快握手超时处理。
- **引擎**：调整 `piece_timeout` 2→5 秒、`request_timeout` 2→4 秒、`peer_timeout` 5→10 秒，减少 peer 频繁断开；参考 libtorrent issue #7666、torrest 和 Elementum。
- **引擎**：`whole_pieces_threshold` 从 5 增至 20，促使高速 peer 优先完成整个分片而非分散下载块。

## 1.5.0

- **流式播放**：取消启动时推测性预取文件末尾 4 MB，改由播放器通过 HTTP Range 按需获取 MP4 moov、MKV cues 等元数据。
- **流式播放**：只预取头部，前 8 片全部使用 deadline 0，替代 30 ms 交错间隔，以集中头部下载。
- **流式播放**：移除每 200 ms 为末尾两片重新设置优先级的锚点逻辑，减少与当前位置的竞争。
- **性能**：当时记录的起播时间由约 30～60 秒降至约 5～15 秒，归因于减少头尾下载的带宽竞争。

## 1.4.0

- **平台**：新增 iOS 支持。

## 1.3.0

- **平台**：新增 iOS 支持，包括 arm64 真机、x86_64 模拟器和通用静态库。
- **CI**：增加 `build-ios`，交叉编译 libtorrent 和 torrent_bridge 静态 `.a`，并用 `lipo` 合并。
- **打包**：iOS podspec 使用 `-force_load`，使全部 FFI 符号可通过 `DynamicLibrary.process()` 访问。

## 1.2.0

- **许可证**：改用 GPL v3（OSI 认可）。
- **流式播放**：开流时同时预取头部和尾部各 4 MB，以获取容器元数据／索引，减少完整缓冲前的拖动等待。原记录将尾部结构统称为 moov；准确地说，moov 属于 MP4，MKV 使用不同的元数据和索引结构。
- **流式播放**：头部 deadline 间隔从 45 ms 缩短为 30 ms，加快起播。
- **API**：新增 `disposeTorrent(id)`，一次调用停止流、删除任务和文件。
- **API**：新增 `disposeAll()`，清理全部任务和流，便于退出时使用。
- **API**：`startStream()` 新增 `maxCacheBytes`，控制滑动 RAM 缓存上限。
- **API**：`init()` 新增 `defaultSavePath`，默认使用系统临时目录，无需额外目录权限配置。
- **API**：`addMagnet()`、`addTorrentFile()` 的 `savePath` 改为可选，默认使用 `defaultSavePath`。
- **README**：重写文档，补充完整 API 说明和示例。

## 1.1.0

- **流式播放**：移除与 `set_piece_deadline` 冲突的 `sequential_download` 标志。
- **流式播放**：预读从 150 片缩为 30 片，减少过度预缓冲。
- **流式播放**：拖动时执行 `clear_piece_deadlines()`，并采用 1 秒冷却，改善响应。
- **流式播放**：重新启用入站和出站 uTP，以连接更多 NAT 后的 peer。
- **缓存**：支持配置 RAM 缓存上限，并按百分比滑动淘汰，保留 10% 安全区且至少保护 5 片。
- **保存路径**：全部平台默认使用系统临时目录。
- **CI**：每次构建自动创建 GitHub Release，附带压缩后的原生库。
- **pubspec**：新增 `repository`、`issue_tracker` 和 `topics` 字段，改善 pub.dev 可发现性。

## 1.0.0

- 首次发布。
- 通过 Dart FFI 绑定原生 libtorrent 2.0。
- 内置支持字节 Range 的 HTTP 流服务。
- 支持 Windows、Linux、macOS 和 Android，提供预编译二进制，无需自行构建。
- 启动时自动获取公共 Tracker 列表。
- 支持磁力链接和 `.torrent` 文件。
- 支持按文件开流，并可自动选择最大的文件。
- 提供 `torrentUpdates`、`streamUpdates` 状态流，用于响应式 UI。
- 支持 DHT、PEX、LSD peer 发现。
