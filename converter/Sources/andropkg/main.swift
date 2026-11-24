import Foundation
import AndroPkgCore
import Security

let usage = """
usage:
  andropkg wrap-elf <lib.so> -o <out.dylib> [--platform ios|macos|ios-sim]
                    [--install-name <name>] [--no-svc-patch] [--sign <identity> | --adhoc]
  andropkg elf-info <lib.so>
  andropkg prepare-native <app.apk>... -o <dir> [--platform ios|macos] [--identity <name> | --adhoc]
                    [--identifier <bundle-id>]
      (the on-device installer's path: unzip, wrap and sign without codesign)
"""

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write(Data("andropkg: \(msg)\n".utf8))
    exit(1)
}

func readElf(_ path: String) -> ElfFile {
    guard let data = FileManager.default.contents(atPath: path) else { fail("cannot read \(path)") }
    do { return try ElfFile(bytes: [UInt8](data)) } catch { fail("\(path): \(error)") }
}

func codesign(_ path: String, identity: String) {
    let p = Process()
    p.executableURL = URL(fileURLWithPath: "/usr/bin/codesign")
    p.arguments = ["--force", "--sign", identity, "--timestamp=none", path]
    do { try p.run() } catch { fail("codesign: \(error)") }
    p.waitUntilExit()
    if p.terminationStatus != 0 { fail("codesign failed with status \(p.terminationStatus)") }
}

var args = Array(CommandLine.arguments.dropFirst())
guard let command = args.first else { fail(usage) }
args.removeFirst()

switch command {
case "wrap-elf":
    var input: String?, output: String?, installName: String?, sign: String?
    var platform = ElfWrapper.Platform.iOS
    var patch = true
    var it = args.makeIterator()
    while let a = it.next() {
        switch a {
        case "-o": output = it.next()
        case "--platform":
            guard let v = it.next(), let p = ElfWrapper.Platform(rawValue: v) else { fail("bad --platform") }
            platform = p
        case "--install-name": installName = it.next()
        case "--no-svc-patch": patch = false
        case "--sign": sign = it.next()
        case "--adhoc": sign = "-"
        default:
            if a.hasPrefix("-") || input != nil { fail("unexpected argument \(a)\n\(usage)") }
            input = a
        }
    }
    guard let input, let output else { fail(usage) }
    let elf = readElf(input)
    let name = installName ?? "@rpath/" + URL(fileURLWithPath: output).lastPathComponent
    do {
        let r = try ElfWrapper.wrap(elf, options: .init(platform: platform, installName: name, patchSyscalls: patch))
        try Data(r.machO).write(to: URL(fileURLWithPath: output))
        r.segmentMap.forEach { print("  " + $0) }
        print("  rewrote \(r.patches.count) instruction(s)" +
              (r.patches.isEmpty ? "" : ": " + r.patches.map(\.description).joined(separator: " ")))
    } catch { fail("\(input): \(error)") }
    if let sign { codesign(output, identity: sign) }
    print("wrote \(output)")

case "prepare-native":
    var apks: [String] = [], out: String?, identityName: String?, signingID: String?
    var platform = ElfWrapper.Platform.iOS
    var it = args.makeIterator()
    while let a = it.next() {
        switch a {
        case "-o": out = it.next()
        case "--platform":
            guard let v = it.next(), let p = ElfWrapper.Platform(rawValue: v) else { fail("bad --platform") }
            platform = p
        case "--identity": identityName = it.next()
        case "--adhoc": identityName = nil
        // The signing identifier (default: the library's name). On iOS, a library outside the app
        // bundle must carry the app's bundle identifier: amfid matches it against the app's
        // provisioning profile (TEAM.<identifier>).
        case "--identifier": signingID = it.next()
        default: apks.append(a)
        }
    }
    guard let out, !apks.isEmpty else { fail(usage) }
    var identity: MachOSigner.Identity?
    if let identityName { identity = keychainIdentity(identityName) }
    do {
        let r = try NativeLibraries.prepare(apks: apks, out: out, platform: platform, identity: identity,
                                                identifier: signingID)
        for (name, err) in r.failed { print("warning: \(name) not converted: \(err)") }
        print("native libs: \(r.converted.count) converted, \(r.unchanged.count) unchanged, \(r.failed.count) failed into \(out)")
    } catch { fail("\(error)") }

case "macho-certs":  // the certificate chain in a Mach-O's signature
    guard let input = args.first, let data = FileManager.default.contents(atPath: input) else { fail(usage) }
    do {
        for c in try embeddedCertificates(machO: [UInt8](data)) { print("\(c.der.count) bytes, OU=\(c.organizationalUnit ?? "-")") }
    } catch { fail("\(error)") }

case "elf-info":
    guard let input = args.first else { fail(usage) }
    let elf = readElf(input)
    for ph in elf.programHeaders {
        print(String(format: "type 0x%08x flags %@%@%@ off 0x%06llx vaddr 0x%06llx filesz 0x%06llx memsz 0x%06llx align 0x%llx",
                     ph.type, ph.readable ? "r" : "-", ph.writable ? "w" : "-", ph.executable ? "x" : "-",
                     ph.offset, ph.vaddr, ph.filesz, ph.memsz, ph.align))
    }

default:
    fail(usage)
}

/// A code-signing identity from the keychain whose certificate name contains `name`, with its
/// chain (the Apple WWDR intermediate).
func keychainIdentity(_ name: String) -> MachOSigner.Identity {
    let q: [String: Any] = [kSecClass as String: kSecClassIdentity, kSecMatchLimit as String: kSecMatchLimitAll,
                            kSecReturnRef as String: true]
    var items: CFTypeRef?
    guard SecItemCopyMatching(q as CFDictionary, &items) == errSecSuccess, let list = items as? [SecIdentity] else {
        fail("no identities in the keychain")
    }
    for id in list {
        var cert: SecCertificate?
        SecIdentityCopyCertificate(id, &cert)
        guard let cert, let summary = SecCertificateCopySubjectSummary(cert) as String?, summary.contains(name) else { continue }
        var trust: SecTrust?
        SecTrustCreateWithCertificates(cert, SecPolicyCreateBasicX509(), &trust)
        var chain: [Certificate] = []
        if let trust {
            _ = SecTrustEvaluateWithError(trust, nil)
            let certs = (SecTrustCopyCertificateChain(trust) as? [SecCertificate]) ?? []
            chain = certs.dropFirst().compactMap { try? Certificate(der: Array(SecCertificateCopyData($0) as Data)) }
        }
        do { return try SecIdentitySigner(identity: id, chain: chain) } catch { fail("\(error)") }
    }
    fail("no identity matching \(name)")
}
