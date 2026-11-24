import Foundation
import Security

/// A signing identity from the Security framework: a certificate with its private key (an
/// imported .p12 on iOS, the keychain on the Mac).
public struct SecIdentitySigner: MachOSigner.Identity {
    let key: SecKey
    public let certificates: [Certificate]

    /// `chain`: the certificates above the leaf (Apple WWDR, ...), when the identity lacks them.
    public init(identity: SecIdentity, chain: [Certificate] = []) throws {
        var cert: SecCertificate?
        var key: SecKey?
        guard SecIdentityCopyCertificate(identity, &cert) == errSecSuccess, let cert,
              SecIdentityCopyPrivateKey(identity, &key) == errSecSuccess, let key else {
            throw ConverterError.unsupported("identity without a certificate or key")
        }
        self.key = key
        let leaf = try Certificate(der: Array(SecCertificateCopyData(cert) as Data))
        certificates = [leaf] + chain.filter { $0.der != leaf.der && $0.organizationalUnit != leaf.organizationalUnit }
    }

    /// The identity in a PKCS#12 file (a certificate exported with its key).
    public static func pkcs12(_ data: Data, password: String, chain: [Certificate] = []) throws -> SecIdentitySigner {
        var items: CFArray?
        let status = SecPKCS12Import(data as CFData, [kSecImportExportPassphrase as String: password] as CFDictionary, &items)
        guard status == errSecSuccess, let list = items as? [[String: Any]], let first = list.first,
              let identity = first[kSecImportItemIdentity as String] else {
            throw ConverterError.unsupported(status == errSecAuthFailed ? "wrong password" : "not a certificate with its key (\(status))")
        }
        var more: [Certificate] = chain
        if let certs = first[kSecImportItemCertChain as String] as? [SecCertificate] {
            more = certs.dropFirst().compactMap { try? Certificate(der: Array(SecCertificateCopyData($0) as Data)) } + chain
        }
        return try SecIdentitySigner(identity: identity as! SecIdentity, chain: more)
    }

    public func signSHA256RSA(_ data: [UInt8]) throws -> [UInt8] {
        var error: Unmanaged<CFError>?
        guard let sig = SecKeyCreateSignature(key, .rsaSignatureMessagePKCS1v15SHA256, Data(data) as CFData, &error) else {
            throw ConverterError.unsupported("signing: \(error!.takeRetainedValue())")
        }
        return Array(sig as Data)
    }
}
