# Actions 产物放置说明

本目录用于本地验证。目录占位文件和本说明提交到 Git，编译产物与下载文件保持忽略。下载与当前源码提交对应的 Actions Artifact ZIP 后，按表格解压。

以下路径均相对于插件根目录，不是 Flutter 应用根目录。

| ZIP | 解压到 |
| --- | --- |
| ios-native-lib.zip | prebuilt/ios/libtorrent_flutter.xcframework/ |
| macos-native-lib.zip | prebuilt/macos/（ZIP 内自带 universal/） |
| android-native-lib-arm64-v8a.zip | prebuilt/android/arm64-v8a/ |
| android-native-lib-armeabi-v7a.zip | prebuilt/android/armeabi-v7a/ |
| android-native-lib-x86_64.zip | prebuilt/android/x86_64/ |
| linux-native-lib-x64.zip | prebuilt/linux/x64/ |
| linux-native-lib-arm64.zip | prebuilt/linux/arm64/ |
| windows-native-lib-x64.zip | prebuilt/windows/x64/ |
| windows-native-lib-arm64.zip | prebuilt/windows/arm64/ |

不要额外套一层 Artifact 名称目录，也不要仅把 ZIP 放入目录。
iOS 解压后，Info.plist 应直接位于 prebuilt/ios/libtorrent_flutter.xcframework/ 下。
Android 默认需要三个 ABI 的库都到位；Linux/Windows 只需准备待测试的架构。

## Apple 平台还需要复制到 CocoaPods 使用的位置

拿到产物并解压后，在插件根目录执行对应命令（macOS/Linux shell）：

```bash
# iOS：复制整个 XCFramework 到 ios/，首次安装时执行
cp -R prebuilt/ios/libtorrent_flutter.xcframework ios/

# macOS：复制通用动态库到 macos/
cp prebuilt/macos/universal/liblibtorrent_flutter.dylib macos/
```

以后更新 iOS 产物时，先移走旧的 ios/libtorrent_flutter.xcframework，再复制完整新目录，避免残留。
未预先创建 ios/libtorrent_flutter.xcframework 空目录，以免影响 CocoaPods 的缺库自动下载判断。

## 本地运行

Flutter 应用通过 path 指向此插件目录后，在应用目录执行 flutter pub get 和 flutter run。
替换原生库时，停止应用并执行 flutter clean、flutter pub get，再重新运行，热重载不会更新原生库。
只需准备要测试的平台；不需要发布 Release，也不需要本地重新编译 libtorrent。
运行应用本身仍需对应平台的 Flutter 开发环境。
