import Foundation

/// Just enough DER (ASN.1) to build a CMS signature and read certificates.
enum Der {
    // MARK: writing

    static func tlv(_ tag: UInt8, _ content: [UInt8]) -> [UInt8] {
        var out: [UInt8] = [tag]
        let n = content.count
        if n < 0x80 {
            out.append(UInt8(n))
        } else {
            var len: [UInt8] = []
            var v = n
            while v > 0 { len.insert(UInt8(v & 0xFF), at: 0); v >>= 8 }
            out.append(0x80 | UInt8(len.count))
            out += len
        }
        return out + content
    }

    static func sequence(_ items: [UInt8]...) -> [UInt8] { tlv(0x30, items.flatMap { $0 }) }
    static func set(_ items: [UInt8]...) -> [UInt8] { tlv(0x31, items.flatMap { $0 }) }
    static func octets(_ b: [UInt8]) -> [UInt8] { tlv(0x04, b) }
    static let null: [UInt8] = [0x05, 0x00]

    static func integer(_ v: Int) -> [UInt8] {
        var b: [UInt8] = []
        var x = v
        repeat { b.insert(UInt8(x & 0xFF), at: 0); x >>= 8 } while x > 0
        if b[0] & 0x80 != 0 { b.insert(0, at: 0) }
        return tlv(0x02, b)
    }

    static func oid(_ dotted: String) -> [UInt8] {
        let parts = dotted.split(separator: ".").map { UInt64($0)! }
        var b: [UInt8] = [UInt8(parts[0] * 40 + parts[1])]
        for p in parts.dropFirst(2) {
            var chunk: [UInt8] = [UInt8(p & 0x7F)]
            var v = p >> 7
            while v > 0 { chunk.insert(UInt8(v & 0x7F) | 0x80, at: 0); v >>= 7 }
            b += chunk
        }
        return tlv(0x06, b)
    }

    static func utcTime(_ date: Date) -> [UInt8] {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.timeZone = TimeZone(identifier: "UTC")
        f.dateFormat = "yyMMddHHmmss'Z'"
        return tlv(0x17, Array(f.string(from: date).utf8))
    }

    /// A SET OF: DER orders its elements by their encodings.
    static func setOf(_ items: [[UInt8]]) -> [UInt8] {
        tlv(0x31, items.sorted { $0.lexicographicallyPrecedes($1) }.flatMap { $0 })
    }

    // MARK: reading

    struct Node {
        let tag: UInt8
        let start: Int        // of the whole TLV
        let contentStart: Int
        let contentEnd: Int
        let end: Int          // past the TLV (an indefinite length's end-of-contents too)
        var contentRange: Range<Int> { contentStart..<contentEnd }
        var range: Range<Int> { start..<end }
    }

    /// The TLV at `offset` in `b`. BER's indefinite lengths are read too (Apple's code
    /// signatures use them for the CMS ContentInfo).
    static func node(_ b: [UInt8], at offset: Int) throws -> Node {
        guard offset + 2 <= b.count else { throw ConverterError.malformed("DER: truncated") }
        let tag = b[offset]
        var i = offset + 1
        var len = Int(b[i]); i += 1
        if len == 0x80 {  // indefinite: children up to an end-of-contents (00 00)
            guard tag & 0x20 != 0 else { throw ConverterError.malformed("DER: indefinite primitive") }
            var j = i
            while true {
                guard j + 2 <= b.count else { throw ConverterError.malformed("DER: unterminated") }
                if b[j] == 0, b[j + 1] == 0 { return Node(tag: tag, start: offset, contentStart: i, contentEnd: j, end: j + 2) }
                j = try node(b, at: j).end
            }
        }
        if len & 0x80 != 0 {
            let n = len & 0x7F
            guard n > 0, n <= 4, i + n <= b.count else { throw ConverterError.malformed("DER: bad length") }
            len = 0
            for _ in 0..<n { len = len << 8 | Int(b[i]); i += 1 }
        }
        guard i + len <= b.count else { throw ConverterError.malformed("DER: truncated content") }
        return Node(tag: tag, start: offset, contentStart: i, contentEnd: i + len, end: i + len)
    }

    /// The children of a constructed node.
    static func children(_ b: [UInt8], _ n: Node) throws -> [Node] {
        var out: [Node] = []
        var i = n.contentStart
        while i < n.contentEnd {
            let c = try node(b, at: i)
            out.append(c)
            i = c.end
        }
        return out
    }
}

/// The parts of an X.509 certificate a CMS signer needs.
public struct Certificate: Sendable {
    public let der: [UInt8]
    let issuer: [UInt8]       // the Name, as encoded
    let serial: [UInt8]       // the INTEGER, as encoded
    let subject: [UInt8]      // the Name, as encoded
    /// The subject's organizational unit: an Apple signing certificate's team identifier.
    public let organizationalUnit: String?

    public init(der: [UInt8]) throws {
        self.der = der
        let cert = try Der.node(der, at: 0)
        let tbs = try Der.children(der, Der.children(der, cert)[0])
        var i = 0
        if tbs[0].tag == 0xA0 { i += 1 }  // [0] version
        serial = Array(der[tbs[i].range])
        issuer = Array(der[tbs[i + 2].range])
        let subject = tbs[i + 4]
        self.subject = Array(der[subject.range])
        let ouOid = Der.oid("2.5.4.11")
        var ou: String?
        for rdn in try Der.children(der, subject) {
            for atv in try Der.children(der, rdn) {
                let kv = try Der.children(der, atv)
                if kv.count == 2, Array(der[kv[0].range]) == ouOid {
                    ou = String(decoding: der[kv[1].contentRange], as: UTF8.self)
                }
            }
        }
        organizationalUnit = ou
    }
}

/// The certificates in a Mach-O's embedded CMS signature (the chain the app was signed with:
/// leaf, Apple WWDR intermediate, Apple root).
public func embeddedCertificates(machO b: [UInt8]) throws -> [Certificate] {
    let r = ByteReader(bytes: b)
    guard try r.u32(0) == 0xFEED_FACF else { throw ConverterError.malformed("not a 64-bit Mach-O") }
    let ncmds = Int(try r.u32(16))
    var off = 32
    var sig: Range<Int>?
    for _ in 0..<ncmds {
        let cmd = try r.u32(off), size = Int(try r.u32(off + 4))
        if cmd == 0x1D {
            let o = Int(try r.u32(off + 8)), n = Int(try r.u32(off + 12))
            guard o + n <= b.count else { throw ConverterError.truncated(offset: o) }
            sig = o..<(o + n)
        }
        off += size
    }
    guard let sig else { return [] }
    func be(_ i: Int) -> Int { Int(b[i]) << 24 | Int(b[i + 1]) << 16 | Int(b[i + 2]) << 8 | Int(b[i + 3]) }
    let count = be(sig.lowerBound + 8)
    for k in 0..<count {
        let type = be(sig.lowerBound + 12 + 8 * k), blobOff = sig.lowerBound + be(sig.lowerBound + 16 + 8 * k)
        guard type == 0x10000, be(blobOff) == 0xFADE_0B01 else { continue }
        let cms = Array(b[(blobOff + 8)..<(blobOff + be(blobOff + 4))])
        guard !cms.isEmpty else { return [] }  // ad-hoc
        // ContentInfo { oid, [0] { SignedData { version, digestAlgs, content, [0] certs, ... } } }
        let ci = try Der.children(cms, Der.node(cms, at: 0))
        let sd = try Der.children(cms, Der.children(cms, ci[1])[0])
        guard let certs = sd.first(where: { $0.tag == 0xA0 }) else { return [] }
        return orderedChain(try Der.children(cms, certs).map { try Certificate(der: Array(cms[$0.range])) })
    }
    return []
}

/// Leaf first, then each certificate's issuer (a CMS certificate set has no order).
public func orderedChain(_ certs: [Certificate]) -> [Certificate] {
    guard let leaf = certs.first(where: { c in !certs.contains { $0.der != c.der && $0.issuer == c.subject } }) else {
        return certs
    }
    var chain = [leaf]
    while let last = chain.last, last.issuer != last.subject,
          let up = certs.first(where: { $0.subject == last.issuer && !chain.map(\.der).contains($0.der) }) {
        chain.append(up)
    }
    return chain + certs.filter { c in !chain.contains { $0.der == c.der } }
}
