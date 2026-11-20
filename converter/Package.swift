// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "AndroPkgConverter",
    platforms: [.macOS(.v14), .iOS(.v17)],
    products: [
        // Shared by the macOS CLI and (later) the on-device container app.
        .library(name: "AndroPkgCore", targets: ["AndroPkgCore"]),
        .executable(name: "andropkg", targets: ["andropkg"]),
    ],
    targets: [
        .target(name: "AndroPkgCore"),
        .executableTarget(name: "andropkg", dependencies: ["AndroPkgCore"]),
        .testTarget(name: "AndroPkgCoreTests", dependencies: ["AndroPkgCore"]),
    ]
)
