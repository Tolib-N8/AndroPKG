import Foundation

/// Little-endian reader over an immutable byte buffer with bounds checking.
struct ByteReader {
    let bytes: [UInt8]

    func u8(_ o: Int) throws -> UInt8 {
        guard o >= 0, o < bytes.count else { throw ConverterError.truncated(offset: o) }
        return bytes[o]
    }

    func u16(_ o: Int) throws -> UInt16 { try UInt16(truncatingIfNeeded: le(o, 2)) }
    func u32(_ o: Int) throws -> UInt32 { try UInt32(truncatingIfNeeded: le(o, 4)) }
    func u64(_ o: Int) throws -> UInt64 { try le(o, 8) }

    private func le(_ o: Int, _ n: Int) throws -> UInt64 {
        guard o >= 0, o + n <= bytes.count else { throw ConverterError.truncated(offset: o) }
        var v: UInt64 = 0
        for i in 0..<n { v |= UInt64(bytes[o + i]) << (8 * UInt64(i)) }
        return v
    }

    func cString(_ o: Int) throws -> String {
        guard o >= 0, o < bytes.count else { throw ConverterError.truncated(offset: o) }
        var end = o
        while end < bytes.count, bytes[end] != 0 { end += 1 }
        return String(decoding: bytes[o..<end], as: UTF8.self)
    }
}

/// Little-endian growable byte buffer.
struct ByteWriter {
    private(set) var bytes: [UInt8] = []

    var count: Int { bytes.count }

    mutating func u8(_ v: UInt8) { bytes.append(v) }
    mutating func u16(_ v: UInt16) { put(UInt64(v), 2) }
    mutating func u32(_ v: UInt32) { put(UInt64(v), 4) }
    mutating func u64(_ v: UInt64) { put(v, 8) }

    private mutating func put(_ v: UInt64, _ n: Int) {
        for i in 0..<n { bytes.append(UInt8(truncatingIfNeeded: v >> (8 * UInt64(i)))) }
    }

    /// Fixed-width, NUL-padded name field (Mach-O segname/sectname are 16 bytes).
    mutating func fixedString(_ s: String, width: Int) {
        var b = Array(s.utf8.prefix(width))
        b.append(contentsOf: [UInt8](repeating: 0, count: width - b.count))
        bytes.append(contentsOf: b)
    }

    mutating func append(_ b: [UInt8]) { bytes.append(contentsOf: b) }
    mutating func append(_ b: ArraySlice<UInt8>) { bytes.append(contentsOf: b) }

    mutating func pad(to size: Int) {
        precondition(size >= bytes.count, "pad target \(size) < current \(bytes.count)")
        bytes.append(contentsOf: [UInt8](repeating: 0, count: size - bytes.count))
    }

    mutating func align(_ a: Int) { pad(to: alignUp(bytes.count, a)) }

    mutating func patchU32(at o: Int, _ v: UInt32) {
        for i in 0..<4 { bytes[o + i] = UInt8(truncatingIfNeeded: v >> (8 * UInt32(i))) }
    }
}

func alignUp(_ v: Int, _ a: Int) -> Int { (v + a - 1) / a * a }
func alignDown(_ v: Int, _ a: Int) -> Int { v / a * a }
func alignUp(_ v: UInt64, _ a: UInt64) -> UInt64 { (v + a - 1) / a * a }
func alignDown(_ v: UInt64, _ a: UInt64) -> UInt64 { v / a * a }

public enum ConverterError: Error, CustomStringConvertible {
    case truncated(offset: Int)
    case notElf
    case unsupported(String)
    case layout(String)
    case io(String)
    case malformed(String)

    public var description: String {
        switch self {
        case .truncated(let o): return "file truncated (read at offset \(o))"
        case .notElf: return "not an ELF64 little-endian AArch64 shared object"
        case .unsupported(let s): return "unsupported: \(s)"
        case .layout(let s): return "layout error: \(s)"
        case .io(let s): return "I/O error: \(s)"
        case .malformed(let s): return "malformed: \(s)"
        }
    }
}
