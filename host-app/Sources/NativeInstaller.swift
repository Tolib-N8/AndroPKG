// andropkg host — converting and signing an app's native libraries on the device.
//
// iOS loads a library only when it is signed by the app's own team. The user imports the
// certificate AndroPkg was signed with (a .p12 with its private key, exported from the Mac's
// Keychain or a signing tool); the libraries of an app are then converted (ELF -> Mach-O
// wrapper, AndroPkgCore) and signed with it (MachOSigner) the first time it opens, into
// Application Support/andropkg/native/<app>, where the runtime finds them
// (-Dandropkg.nativeLibraryDir).
import AndroPkgCore
import Foundation
import Security

@objc(AndropkgNativeInstaller)
public final class NativeInstaller: NSObject {
    private static let service = "dev.andropkg.signing"

    private static func supportDir(_ sub: String) -> URL {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = support.appendingPathComponent("andropkg").appendingPathComponent(sub)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    private static var p12URL: URL { supportDir("signing").appendingPathComponent("certificate.p12") }

    /// The libraries' directory of an app (named as its data directory).
    @objc public static func nativeDir(forApp name: String) -> String {
        supportDir("native").appendingPathComponent(name).path
    }

    /// The certificate chain AndroPkg itself was signed with (leaf first).
    private static func ownCertificates() -> [Certificate] {
        guard let exe = Bundle.main.executableURL, let data = try? Data(contentsOf: exe, options: .alwaysMapped) else { return [] }
        return (try? embeddedCertificates(machO: Array(data))) ?? []
    }

    /// The team AndroPkg was signed by (the libraries must be signed by it too).
    @objc public static var appTeam: String? { ownCertificates().first?.organizationalUnit }

    @objc public static var hasCertificate: Bool {
        FileManager.default.fileExists(atPath: p12URL.path) && password() != nil
    }

    private static func identity() throws -> SecIdentitySigner {
        guard let data = try? Data(contentsOf: p12URL), let pw = password() else {
            throw ConverterError.unsupported("no certificate imported")
        }
        return try SecIdentitySigner.pkcs12(data, password: pw, chain: Array(ownCertificates().dropFirst()))
    }

    /// Imports a .p12; nil on success, else what is wrong (in Russian, for the user).
    @objc public static func importCertificate(_ path: String, password pw: String) -> String? {
        guard let data = FileManager.default.contents(atPath: path) else { return "Не удалось прочитать файл." }
        let signer: SecIdentitySigner
        do {
            signer = try SecIdentitySigner.pkcs12(data, password: pw)
        } catch {
            return "\(error)".contains("wrong password") ? "Неверный пароль." : "Это не сертификат с закрытым ключом (\(error))."
        }
        let team = signer.certificates.first?.organizationalUnit
        if let app = appTeam, team != app {
            return "Сертификат команды \(team ?? "?"), а AndroPkg подписан командой \(app). Нужен сертификат, которым подписан AndroPkg."
        }
        do {
            try data.write(to: p12URL, options: .completeFileProtection)
        } catch {
            return "Не удалось сохранить: \(error.localizedDescription)"
        }
        setPassword(pw)
        return nil
    }

    /// Whether the app's libraries are converted and signed with the current certificate.
    @objc public static func isPrepared(app name: String) -> Bool {
        let marker = URL(fileURLWithPath: nativeDir(forApp: name)).appendingPathComponent(".signer")
        guard let signer = try? String(contentsOf: marker, encoding: .utf8) else { return false }
        return signer == currentSignerID()
    }

    private static func currentSignerID() -> String? {
        guard let data = try? Data(contentsOf: p12URL) else { return nil }
        // Changes with the certificate, and with the signing format (signed again then).
        return "v3:" + data.base64EncodedString().suffix(64)
    }

    /// Converts and signs the libraries of the APK (or split package directory); nil on
    /// success, else the error. `progress(name, done, total)` is called before each library.
    @objc public static func prepare(apk: String, app name: String,
                                     progress: @escaping (String, Int, Int) -> Void) -> String? {
        var apks = [apk]
        var isDir: ObjCBool = false
        if FileManager.default.fileExists(atPath: apk, isDirectory: &isDir), isDir.boolValue {
            apks = ((try? FileManager.default.contentsOfDirectory(atPath: apk)) ?? [])
                .filter { $0.hasSuffix(".apk") }.sorted().map { apk + "/" + $0 }
        }
        let out = nativeDir(forApp: name)
        // Signed by another certificate (or an older format): all signed again.
        if !isPrepared(app: name) { try? FileManager.default.removeItem(atPath: out) }
        do {
            // Signed as AndroPkg (its bundle identifier): amfid checks a developer-signed library
            // outside the app bundle against the app's provisioning profile, whose
            // application-identifier is TEAM.<bundle id>; a library's own name fails that
            // ("non-matching application-identifier", AMFI -415).
            let report = try NativeLibraries.prepare(apks: apks, out: out, platform: .iOS, identity: try identity(),
                                                     identifier: Bundle.main.bundleIdentifier, progress: progress)
            for (lib, err) in report.failed { NSLog("andropkg: %@ not converted: %@", lib, err) }
            NSLog("andropkg: native libs of %@: %d converted, %d unchanged, %d failed", name,
                  report.converted.count, report.unchanged.count, report.failed.count)
            try currentSignerID()?.write(to: URL(fileURLWithPath: out).appendingPathComponent(".signer"),
                                         atomically: true, encoding: .utf8)
            return nil
        } catch {
            return "\(error)"
        }
    }

    // The .p12's password, in the keychain.
    private static func password() -> String? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecReturnData as String: true]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }

    private static func setPassword(_ pw: String) {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service]
        SecItemDelete(q as CFDictionary)
        var add = q
        add[kSecValueData as String] = Data(pw.utf8)
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly
        SecItemAdd(add as CFDictionary, nil)
    }
}
