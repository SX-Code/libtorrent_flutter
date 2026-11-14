// swift-tools-version: 5.9

import PackageDescription

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
    targets: [
        .target(
            name: "libtorrent_flutter",
            dependencies: [
                .product(name: "FlutterFramework", package: "FlutterFramework")
            ]
        )
    ]
)
