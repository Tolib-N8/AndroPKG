import CryptoKit
import Foundation

/// Signs a thin 64-bit Mach-O as codesign does, without codesign (which iOS doesn't have): an
/// embedded signature with a SHA-256 CodeDirectory, an empty requirements set and a CMS
/// signature by a certificate. Ad-hoc without one. On iOS, a library loads only when signed by
/// the app's own team, so the on-device installer signs the libraries it converts with the
/// certificate the app was signed with.
public enum MachOSigner {
    /// Signs data with the certificate's private key (CMS: RSA PKCS#1 v1.5 with SHA-256).
    public protocol Identity {
        /// The signing certificate first, then its chain (the Apple WWDR intermediate, ...).
        var certificates: [Certificate] { get }
        func signSHA256RSA(_ data: [UInt8]) throws -> [UInt8]
    }

    static let pageShift: UInt8 = 12               // hashed in 4 KB pages, as codesign does
    static let cmsReserve = 9000                    // room for the CMS blob (certificates ~4.5 KB)

    /// `identifier`: the signing identifier (codesign's default: the file name).
    public static func sign(_ input: [UInt8], identifier: String, identity: Identity?) throws -> [UInt8] {
        var b = input
        let r = ByteReader(bytes: b)
        guard try r.u32(0) == 0xFEED_FACF else { throw ConverterError.malformed("not a thin 64-bit Mach-O") }
        let ncmds = Int(try r.u32(16)), sizeofcmds = Int(try r.u32(20))
        var off = 32
        var linkedit: Int?, text: (fileoff: UInt64, filesize: UInt64)?
        for _ in 0..<ncmds {
            let cmd = try r.u32(off), size = Int(try r.u32(off + 4))
            if cmd == 0x1D { throw ConverterError.unsupported("already signed") }
            if cmd == 0x19 {
                let name = String(decoding: b[(off + 8)..<(off + 24)].prefix { $0 != 0 }, as: UTF8.self)
                if name == "__LINKEDIT" { linkedit = off }
                if name == "__TEXT" { text = (try r.u64(off + 40), try r.u64(off + 48)) }
            }
            off += size
        }
        guard let linkedit, let text else { throw ConverterError.malformed("no __LINKEDIT or __TEXT") }
        let cmdsEnd = 32 + sizeofcmds
        guard b[cmdsEnd..<(cmdsEnd + 16)].allSatisfy({ $0 == 0 }) else {
            throw ConverterError.layout("no room for LC_CODE_SIGNATURE")
        }

        // The signature goes at the end, 16-byte aligned; its size is fixed up front (the header
        // that describes it is hashed too).
        let teamID = identity?.certificates.first?.organizationalUnit
        let codeLimit = (b.count + 15) & ~15
        let nCodeSlots = (codeLimit + (1 << pageShift) - 1) >> pageShift
        let identSize: Int = identifier.utf8.count + 1
        let teamSize: Int = teamID.map { $0.utf8.count + 1 } ?? 0
        let hashOffset: Int = 88 + identSize + teamSize + 32 * 2      // code slot 0, after 2 special slots
        let cdSize: Int = hashOffset + 32 * nCodeSlots
        let requirements: [UInt8] = be32(0xFADE_0C01) + be32(12) + be32(0)   // empty set
        let cmsSize = identity == nil ? 0 : cmsReserve
        let blobsSize: Int = cdSize + requirements.count + 8 + cmsSize
        let sigSize: Int = (12 + 3 * 8 + blobsSize + 15) & ~15

        b += [UInt8](repeating: 0, count: codeLimit - b.count)
        // LC_CODE_SIGNATURE
        put32(&b, cmdsEnd, 0x1D); put32(&b, cmdsEnd + 4, 16)
        put32(&b, cmdsEnd + 8, UInt32(codeLimit)); put32(&b, cmdsEnd + 12, UInt32(sigSize))
        put32(&b, 16, UInt32(ncmds + 1)); put32(&b, 20, UInt32(sizeofcmds + 16))
        // __LINKEDIT covers the signature.
        let leFileoff = Int(try ByteReader(bytes: b).u64(linkedit + 40))
        let leFilesize = UInt64(codeLimit + sigSize - leFileoff)
        put64(&b, linkedit + 48, leFilesize)
        put64(&b, linkedit + 32, (leFilesize + 0x3FFF) & ~0x3FFF)   // vmsize

        // CodeDirectory (version 0x20400: with the executable segment).
        var cd: [UInt8] = []
        let identOffset = 88
        let teamOffset = teamID == nil ? 0 : identOffset + identSize
        cd += be32(0xFADE_0C02) + be32(UInt32(cdSize)) + be32(0x20400)
        cd += be32(identity == nil ? 0x2 : 0)                       // flags: CS_ADHOC
        cd += be32(UInt32(hashOffset)) + be32(UInt32(identOffset))
        cd += be32(2) + be32(UInt32(nCodeSlots)) + be32(UInt32(codeLimit))
        cd += [32, 2, 0, pageShift] + be32(0)                       // hashSize, SHA-256, platform, page; spare2
        cd += be32(0) + be32(UInt32(teamOffset))                    // scatter, team
        cd += be32(0) + be64(0)                                     // spare3, codeLimit64
        cd += be64(text.fileoff) + be64(text.filesize) + be64(0)    // execSeg base, limit, flags
        cd += Array(identifier.utf8) + [0]
        if let teamID { cd += Array(teamID.utf8) + [0] }
        cd += Array(SHA256.hash(data: requirements))                // slot -2: requirements
        cd += [UInt8](repeating: 0, count: 32)                      // slot -1: Info.plist (none)
        var page = 0
        while page < codeLimit {
            cd += Array(SHA256.hash(data: b[page..<min(page + (1 << pageShift), codeLimit)]))
            page += 1 << pageShift
        }
        precondition(cd.count == cdSize)

        var cms: [UInt8] = []
        if let identity { cms = try cmsSignature(cd: cd, identity: identity) }
        guard cms.count <= cmsReserve else { throw ConverterError.layout("CMS signature too large (\(cms.count))") }

        // The SuperBlob: CodeDirectory, requirements, CMS wrapper.
        let cmsBlob = be32(0xFADE_0B01) + be32(UInt32(8 + cms.count)) + cms
        let blobs: [(UInt32, [UInt8])] = [(0, cd), (2, requirements), (0x10000, cmsBlob)]
        var sb: [UInt8] = []
        var blobOff = 12 + 8 * blobs.count
        var index: [UInt8] = []
        var body: [UInt8] = []
        for (type, blob) in blobs {
            index += be32(type) + be32(UInt32(blobOff))
            body += blob
            blobOff += blob.count
        }
        sb = be32(0xFADE_0CC0) + be32(UInt32(12 + index.count + body.count)) + be32(UInt32(blobs.count)) + index + body
        b += sb
        b += [UInt8](repeating: 0, count: codeLimit + sigSize - b.count)
        return b
    }

    /// The CMS SignedData over the CodeDirectory (detached), as codesign makes it: signed
    /// attributes with the content type, signing time, the digest and Apple's CDHash attributes.
    static func cmsSignature(cd: [UInt8], identity: Identity) throws -> [UInt8] {
        guard let leaf = identity.certificates.first else { throw ConverterError.unsupported("no certificate") }
        let sha256 = Der.sequence(Der.oid("2.16.840.1.101.3.4.2.1"))
        let digest = Array(SHA256.hash(data: cd))
        let cdhashPlist = """
            <?xml version="1.0" encoding="UTF-8"?>
            <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
            <plist version="1.0">
            <dict>
            \t<key>cdhashes</key>
            \t<array>
            \t\t<data>
            \t\t\(Data(digest.prefix(20)).base64EncodedString())
            \t\t</data>
            \t</array>
            </dict>
            </plist>

            """
        func attribute(_ oid: String, _ value: [UInt8]) -> [UInt8] { Der.sequence(Der.oid(oid), Der.set(value)) }
        let attrs = [
            attribute("1.2.840.113549.1.9.3", Der.oid("1.2.840.113549.1.7.1")),   // content type: data
            attribute("1.2.840.113549.1.9.5", Der.utcTime(Date())),                  // signing time
            attribute("1.2.840.113549.1.9.4", Der.octets(digest)),                   // message digest
            attribute("1.2.840.113635.100.9.1", Der.octets(Array(cdhashPlist.utf8))), // CDHashes (plist)
            attribute("1.2.840.113635.100.9.2", Der.sequence(Der.oid("2.16.840.1.101.3.4.2.1"), Der.octets(digest))),
        ]
        let signedAttrs = Der.setOf(attrs)                          // what is signed: the SET
        let signature = try identity.signSHA256RSA(signedAttrs)
        let signerInfo = Der.sequence(
            Der.integer(1),
            Der.sequence(leaf.issuer, leaf.serial),
            sha256,
            [0xA0] + signedAttrs.dropFirst(),                       // [0] IMPLICIT: the same SET
            Der.sequence(Der.oid("1.2.840.113549.1.1.1"), Der.null),
            Der.octets(signature))
        let signedData = Der.sequence(
            Der.integer(1),
            Der.set(sha256),
            Der.sequence(Der.oid("1.2.840.113549.1.7.1")),
            Der.tlv(0xA0, identity.certificates.flatMap(\.der)),
            Der.set(signerInfo))
        return Der.sequence(Der.oid("1.2.840.113549.1.7.2"), Der.tlv(0xA0, signedData))
    }
}

private func be32(_ v: UInt32) -> [UInt8] { [UInt8(v >> 24), UInt8(v >> 16 & 0xFF), UInt8(v >> 8 & 0xFF), UInt8(v & 0xFF)] }
private func be64(_ v: UInt64) -> [UInt8] { be32(UInt32(v >> 32)) + be32(UInt32(v & 0xFFFF_FFFF)) }
private func put32(_ b: inout [UInt8], _ o: Int, _ v: UInt32) {
    for i in 0..<4 { b[o + i] = UInt8(truncatingIfNeeded: v >> (8 * UInt32(i))) }
}
private func put64(_ b: inout [UInt8], _ o: Int, _ v: UInt64) {
    for i in 0..<8 { b[o + i] = UInt8(truncatingIfNeeded: v >> (8 * UInt64(i))) }
}
