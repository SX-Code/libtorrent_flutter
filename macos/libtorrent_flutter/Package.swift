// swift-tools-version: 5.9

import Foundation
import PackageDescription

let packageRoot = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
let nativeBinary = packageRoot.appendingPathComponent(
    "Binary/libtorrent_flutter.xcframework"
)
let hasNativeBinary = FileManager.default.fileExists(atPath: nativeBinary.path)
var targetDependencies: [Target.Dependency] = [
    .product(name: "FlutterFramework", package: "FlutterFramework")
]
var targets: [Target] = []

if hasNativeBinary {
    targets.append(
        .binaryTarget(
            name: "libtorrent_flutter_native",
            path: "Binary/libtorrent_flutter.xcframework"
        )
    )
    targetDependencies.append(.target(name: "libtorrent_flutter_native"))
}

targets.append(
    .target(
        name: "libtorrent_flutter",
        dependencies: targetDependencies,
        linkerSettings: hasNativeBinary
            ? [.unsafeFlags(["-Xlinker", "-u", "-Xlinker", "_lt_version"])]
            : []
    )
)

let package = Package(
    name: "libtorrent_flutter",
    platforms: [
        .macOS("11.0")
    ],
    products: [
        .library(name: "libtorrent-flutter", targets: ["libtorrent_flutter"])
    ],
    dependencies: [
        .package(name: "FlutterFramework", path: "../FlutterFramework")
    ],
    targets: targets
)
