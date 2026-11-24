import Compression
import Foundation

/// Reads entries of a zip archive (an APK): stored or deflated, no zip64.
public struct ZipArchive {
    public struct Entry {
        public let name: String
        let method: UInt16
        let compressedSize: Int
        public let size: Int
        let localHeader: Int
    }

    let data: Data
    public let entries: [Entry]

    public init(path: String) throws {
        guard let d = try? Data(contentsOf: URL(fileURLWithPath: path), options: .alwaysMapped) else {
            throw ConverterError.io("cannot read \(path)")
        }
        data = d
        func u16(_ o: Int) -> Int { Int(d[d.startIndex + o]) | Int(d[d.startIndex + o + 1]) << 8 }
        func u32(_ o: Int) -> Int { u16(o) | u16(o + 2) << 16 }
        // End of central directory: at the end, before a comment of up to 64 KB.
        var eocd = -1
        var i = d.count - 22
        while i >= max(0, d.count - 22 - 0xFFFF) {
            if u32(i) == 0x0605_4B50 { eocd = i; break }
            i -= 1
        }
        guard eocd >= 0 else { throw ConverterError.malformed("\(path): not a zip archive") }
        let count = u16(eocd + 10)
        var p = u32(eocd + 16)
        var list: [Entry] = []
        for _ in 0..<count {
            guard p + 46 <= d.count, u32(p) == 0x0201_4B50 else { throw ConverterError.malformed("\(path): bad central directory") }
            let nameLen = u16(p + 28), extraLen = u16(p + 30), commentLen = u16(p + 32)
            let name = String(decoding: d[(d.startIndex + p + 46)..<(d.startIndex + p + 46 + nameLen)], as: UTF8.self)
            list.append(Entry(name: name, method: UInt16(u16(p + 10)), compressedSize: u32(p + 20),
                              size: u32(p + 24), localHeader: u32(p + 42)))
            p += 46 + nameLen + extraLen + commentLen
        }
        entries = list
    }

    public func read(_ e: Entry) throws -> [UInt8] {
        let d = data
        func u16(_ o: Int) -> Int { Int(d[d.startIndex + o]) | Int(d[d.startIndex + o + 1]) << 8 }
        let start = e.localHeader + 30 + u16(e.localHeader + 26) + u16(e.localHeader + 28)
        guard start + e.compressedSize <= d.count else { throw ConverterError.truncated(offset: start) }
        let raw = d[(d.startIndex + start)..<(d.startIndex + start + e.compressedSize)]
        switch e.method {
        case 0:
            return Array(raw)
        case 8:  // raw deflate (COMPRESSION_ZLIB is raw deflate, without a zlib header)
            var out = [UInt8](repeating: 0, count: e.size)
            let n = raw.withUnsafeBytes { src in
                out.withUnsafeMutableBytes { dst in
                    compression_decode_buffer(dst.baseAddress!.assumingMemoryBound(to: UInt8.self), e.size,
                                              src.baseAddress!.assumingMemoryBound(to: UInt8.self), e.compressedSize,
                                              nil, COMPRESSION_ZLIB)
                }
            }
            guard n == e.size else { throw ConverterError.malformed("\(e.name): inflated \(n) of \(e.size) bytes") }
            return out
        default:
            throw ConverterError.unsupported("\(e.name): compression method \(e.method)")
        }
    }
}

/// An APK's arm64 native libraries, converted for andropkg (scripts/prepare-native-libs.sh does
/// the same on the Mac): lib/arm64-v8a/libfoo.so -> <out>/libfoo.so (the ELF, for the loader)
/// + <out>/libfoo.so.dylib (its signed Mach-O wrapper) + <out>/libfoo.dylib -> libfoo.so (the
/// name System.mapLibraryName gives on this runtime).
public enum NativeLibraries {
    public struct Report {
        public var converted: [String] = []
        public var unchanged: [String] = []
        public var failed: [(String, String)] = []
    }

    /// Whether any of the APKs has arm64 native libraries.
    public static func present(in apks: [String]) -> Bool {
        apks.contains { (try? ZipArchive(path: $0))?.entries.contains(where: isLibrary) ?? false }
    }

    static func isLibrary(_ e: ZipArchive.Entry) -> Bool {
        e.name.hasPrefix("lib/arm64-v8a/") && e.name.hasSuffix(".so") && !e.name.dropFirst(14).contains("/")
    }

    public static func prepare(apks: [String], out: String, platform: ElfWrapper.Platform,
                               identity: MachOSigner.Identity?, identifier: String? = nil,
                               progress: ((String, Int, Int) -> Void)? = nil) throws -> Report {
        let fm = FileManager.default
        try fm.createDirectory(atPath: out, withIntermediateDirectories: true)
        var libs: [(ZipArchive, ZipArchive.Entry)] = []
        for apk in apks {
            let zip = try ZipArchive(path: apk)
            libs += zip.entries.filter(isLibrary).map { (zip, $0) }
        }
        var report = Report()
        for (n, (zip, e)) in libs.enumerated() {
            let name = String(e.name.dropFirst(14))
            progress?(name, n, libs.count)
            let elfPath = out + "/" + name, dylibPath = elfPath + ".dylib"
            let link = out + "/" + String(name.dropLast(3)) + ".dylib"
            let bytes = try zip.read(e)
            try? fm.removeItem(atPath: link)
            try fm.createSymbolicLink(atPath: link, withDestinationPath: name)
            if fm.fileExists(atPath: dylibPath), let old = fm.contents(atPath: elfPath), old == Data(bytes) {
                report.unchanged.append(name)
                continue
            }
            try Data(bytes).write(to: URL(fileURLWithPath: elfPath))
            try fm.setAttributes([.posixPermissions: 0o755], ofItemAtPath: elfPath)
            do {
                let elf = try ElfFile(bytes: bytes)
                let wrapped = try ElfWrapper.wrap(elf, options: .init(platform: platform, installName: "@rpath/\(name).dylib",
                                                                     patchSyscalls: true))
                let signed = try MachOSigner.sign(wrapped.machO, identifier: identifier ?? name, identity: identity)
                try Data(signed).write(to: URL(fileURLWithPath: dylibPath))
                report.converted.append(name)
            } catch {
                // Loading it then fails like a missing library, which apps often handle.
                try? fm.removeItem(atPath: elfPath); try? fm.removeItem(atPath: dylibPath); try? fm.removeItem(atPath: link)
                report.failed.append((name, "\(error)"))
            }
        }
        return report
    }
}
