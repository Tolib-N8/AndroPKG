package dev.andropkg.system;

import android.security.keystore.KeyGenParameterSpec;
import android.util.Log;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.security.Key;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.KeyPairGeneratorSpi;
import java.security.KeyStore;
import java.security.KeyStoreException;
import java.security.KeyStoreSpi;
import java.security.PrivateKey;
import java.security.Provider;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.Security;
import java.security.Signature;
import java.security.cert.Certificate;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.spec.AlgorithmParameterSpec;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.PKCS8EncodedKeySpec;
import java.security.spec.RSAKeyGenParameterSpec;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Calendar;
import java.util.Collections;
import java.util.Date;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Locale;
import java.util.Map;
import java.util.TimeZone;
import javax.crypto.KeyGeneratorSpi;
import javax.crypto.SecretKey;
import javax.crypto.spec.SecretKeySpec;
import javax.security.auth.x500.X500Principal;

/**
 * The "AndroidKeyStore" JCA provider, in software. On a device keys live in keystore2/KeyMint
 * (TEE); here they are ordinary keys kept in a file outside the app's data directory, so apps that
 * encrypt credentials with a keystore key (Spotify, androidx.security, Tink) find it again after a
 * restart. KeyStore, KeyGenerator (AES, HMAC, DESede) and KeyPairGenerator (RSA, EC) with
 * KeyGenParameterSpec; keys come back as plain SecretKeySpec / PKCS#8 keys, usable with any Cipher,
 * Mac or Signature. RSA/EC keys get a self-signed certificate, as without attestation on a device.
 */
public final class AndroidKeyStore {
    static final String TAG = "andropkg-keystore";
    static final String NAME = "AndroidKeyStore";

    private static final int SECRET = 0, PRIVATE = 1, TRUSTED = 2;

    private static final class Entry {
        int kind;
        String algorithm;
        byte[] key;  // raw secret key, or PKCS#8 private key
        List<byte[]> chain = new ArrayList<>();  // DER certificates
        long created;
    }

    private static File file;
    private static final Map<String, Entry> entries = new LinkedHashMap<>();

    public static synchronized void install(File dir) {
        if (Security.getProvider(NAME) != null) return;
        dir.mkdirs();
        file = new File(dir, "keys");
        load();
        Provider p = new Provider(NAME, 1.0, "andropkg software AndroidKeyStore") {};
        p.put("KeyStore." + NAME, Store.class.getName());
        for (String a : new String[] {"AES", "DESede", "HmacSHA1", "HmacSHA224", "HmacSHA256",
                "HmacSHA384", "HmacSHA512"}) {
            p.put("KeyGenerator." + a, SecretGenerator.class.getName() + "$" + a.replace("DESede", "Des"));
        }
        p.put("KeyPairGenerator.RSA", PairGenerator.class.getName() + "$Rsa");
        p.put("KeyPairGenerator.EC", PairGenerator.class.getName() + "$Ec");
        Security.addProvider(p);
    }

    // ---- persistence ----

    private static void load() {
        entries.clear();
        if (!file.exists()) return;
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
            int n = in.readInt();
            for (int i = 0; i < n; i++) {
                String alias = in.readUTF();
                Entry e = new Entry();
                e.kind = in.readInt();
                e.algorithm = in.readUTF();
                e.created = in.readLong();
                e.key = readBytes(in);
                int c = in.readInt();
                for (int j = 0; j < c; j++) e.chain.add(readBytes(in));
                entries.put(alias, e);
            }
        } catch (Exception ex) {
            Log.w(TAG, "unreadable key store " + file, ex);
        }
    }

    private static void save() {
        File tmp = new File(file.getPath() + ".tmp");
        try (DataOutputStream out = new DataOutputStream(new FileOutputStream(tmp))) {
            out.writeInt(entries.size());
            for (Map.Entry<String, Entry> me : entries.entrySet()) {
                Entry e = me.getValue();
                out.writeUTF(me.getKey());
                out.writeInt(e.kind);
                out.writeUTF(e.algorithm);
                out.writeLong(e.created);
                writeBytes(out, e.key);
                out.writeInt(e.chain.size());
                for (byte[] c : e.chain) writeBytes(out, c);
            }
        } catch (Exception ex) {
            Log.e(TAG, "cannot write " + tmp, ex);
            return;
        }
        if (!tmp.renameTo(file)) Log.e(TAG, "cannot replace " + file);
    }

    private static byte[] readBytes(DataInputStream in) throws java.io.IOException {
        int n = in.readInt();
        if (n < 0) return null;
        byte[] b = new byte[n];
        in.readFully(b);
        return b;
    }

    private static void writeBytes(DataOutputStream out, byte[] b) throws java.io.IOException {
        if (b == null) {
            out.writeInt(-1);
            return;
        }
        out.writeInt(b.length);
        out.write(b);
    }

    private static synchronized void put(String alias, Entry e) {
        e.created = System.currentTimeMillis();
        entries.put(alias, e);
        save();
    }

    private static List<byte[]> encode(Certificate[] chain) throws Exception {
        List<byte[]> out = new ArrayList<>();
        if (chain != null) for (Certificate c : chain) out.add(c.getEncoded());
        return out;
    }

    private static Certificate[] decode(List<byte[]> chain) {
        try {
            CertificateFactory f = CertificateFactory.getInstance("X.509");
            Certificate[] out = new Certificate[chain.size()];
            for (int i = 0; i < out.length; i++) out[i] = f.generateCertificate(new ByteArrayInputStream(chain.get(i)));
            return out;
        } catch (Exception ex) {
            Log.w(TAG, "bad certificate", ex);
            return null;
        }
    }

    // ---- KeyStore ----

    public static final class Store extends KeyStoreSpi {
        @Override public Key engineGetKey(String alias, char[] password) {
            Entry e;
            synchronized (AndroidKeyStore.class) { e = entries.get(alias); }
            if (e == null || e.kind == TRUSTED) return null;
            if (e.kind == SECRET) return new SecretKeySpec(e.key, e.algorithm);
            try {
                return KeyFactory.getInstance(e.algorithm).generatePrivate(new PKCS8EncodedKeySpec(e.key));
            } catch (Exception ex) {
                Log.w(TAG, "bad private key " + alias, ex);
                return null;
            }
        }

        @Override public Certificate[] engineGetCertificateChain(String alias) {
            Entry e;
            synchronized (AndroidKeyStore.class) { e = entries.get(alias); }
            return e == null || e.kind != PRIVATE || e.chain.isEmpty() ? null : decode(e.chain);
        }

        @Override public Certificate engineGetCertificate(String alias) {
            Entry e;
            synchronized (AndroidKeyStore.class) { e = entries.get(alias); }
            if (e == null || e.chain.isEmpty()) return null;
            Certificate[] c = decode(e.chain.subList(0, 1));
            return c == null ? null : c[0];
        }

        @Override public Date engineGetCreationDate(String alias) {
            Entry e;
            synchronized (AndroidKeyStore.class) { e = entries.get(alias); }
            return e == null ? null : new Date(e.created);
        }

        @Override public void engineSetKeyEntry(String alias, Key key, char[] password, Certificate[] chain)
                throws KeyStoreException {
            Entry e = new Entry();
            e.algorithm = key.getAlgorithm();
            try {
                if (key instanceof SecretKey) {
                    e.kind = SECRET;
                } else if (key instanceof PrivateKey) {
                    e.kind = PRIVATE;
                    if (chain == null || chain.length == 0) throw new KeyStoreException("private key needs a chain");
                    e.chain = encode(chain);
                } else {
                    throw new KeyStoreException("unsupported key " + key.getClass().getName());
                }
                e.key = key.getEncoded();
                if (e.key == null) throw new KeyStoreException("key is not extractable");
            } catch (KeyStoreException ex) {
                throw ex;
            } catch (Exception ex) {
                throw new KeyStoreException(ex);
            }
            put(alias, e);
        }

        @Override public void engineSetKeyEntry(String alias, byte[] key, Certificate[] chain) throws KeyStoreException {
            throw new KeyStoreException("encoded key import is not supported");
        }

        @Override public void engineSetCertificateEntry(String alias, Certificate cert) throws KeyStoreException {
            Entry e = new Entry();
            e.kind = TRUSTED;
            e.algorithm = cert.getPublicKey().getAlgorithm();
            try {
                e.chain = encode(new Certificate[] {cert});
            } catch (Exception ex) {
                throw new KeyStoreException(ex);
            }
            put(alias, e);
        }

        // Any protection parameter (KeyProtection on Android), not only PasswordProtection.
        @Override public void engineSetEntry(String alias, KeyStore.Entry entry, KeyStore.ProtectionParameter p)
                throws KeyStoreException {
            if (entry instanceof KeyStore.PrivateKeyEntry) {
                KeyStore.PrivateKeyEntry pe = (KeyStore.PrivateKeyEntry) entry;
                engineSetKeyEntry(alias, pe.getPrivateKey(), null, pe.getCertificateChain());
            } else if (entry instanceof KeyStore.SecretKeyEntry) {
                engineSetKeyEntry(alias, ((KeyStore.SecretKeyEntry) entry).getSecretKey(), null, null);
            } else if (entry instanceof KeyStore.TrustedCertificateEntry) {
                engineSetCertificateEntry(alias, ((KeyStore.TrustedCertificateEntry) entry).getTrustedCertificate());
            } else {
                throw new KeyStoreException("unsupported entry " + entry);
            }
        }

        @Override public KeyStore.Entry engineGetEntry(String alias, KeyStore.ProtectionParameter p) {
            Entry e;
            synchronized (AndroidKeyStore.class) { e = entries.get(alias); }
            if (e == null) return null;
            if (e.kind == TRUSTED) return new KeyStore.TrustedCertificateEntry(engineGetCertificate(alias));
            Key k = engineGetKey(alias, null);
            if (k == null) return null;
            if (e.kind == SECRET) return new KeyStore.SecretKeyEntry((SecretKey) k);
            return new KeyStore.PrivateKeyEntry((PrivateKey) k, engineGetCertificateChain(alias));
        }

        @Override public void engineDeleteEntry(String alias) {
            synchronized (AndroidKeyStore.class) {
                if (entries.remove(alias) != null) save();
            }
        }

        @Override public Enumeration<String> engineAliases() {
            synchronized (AndroidKeyStore.class) { return Collections.enumeration(new ArrayList<>(entries.keySet())); }
        }

        @Override public boolean engineContainsAlias(String alias) {
            synchronized (AndroidKeyStore.class) { return entries.containsKey(alias); }
        }

        @Override public int engineSize() {
            synchronized (AndroidKeyStore.class) { return entries.size(); }
        }

        @Override public boolean engineIsKeyEntry(String alias) {
            synchronized (AndroidKeyStore.class) {
                Entry e = entries.get(alias);
                return e != null && e.kind != TRUSTED;
            }
        }

        @Override public boolean engineIsCertificateEntry(String alias) {
            synchronized (AndroidKeyStore.class) {
                Entry e = entries.get(alias);
                return e != null && e.kind == TRUSTED;
            }
        }

        @Override public String engineGetCertificateAlias(Certificate cert) {
            try {
                byte[] want = cert.getEncoded();
                synchronized (AndroidKeyStore.class) {
                    for (Map.Entry<String, Entry> me : entries.entrySet()) {
                        List<byte[]> c = me.getValue().chain;
                        if (!c.isEmpty() && java.util.Arrays.equals(c.get(0), want)) return me.getKey();
                    }
                }
            } catch (Exception ignored) {
            }
            return null;
        }

        @Override public void engineStore(OutputStream out, char[] password) {
            throw new UnsupportedOperationException("AndroidKeyStore can't be stored");
        }

        @Override public void engineLoad(InputStream in, char[] password) {
            if (in != null) throw new IllegalArgumentException("InputStream not supported");
        }
    }

    // ---- KeyGenerator ----

    public abstract static class SecretGenerator extends KeyGeneratorSpi {
        private final String algorithm;
        private final int defaultBits;
        private KeyGenParameterSpec spec;
        private SecureRandom random = new SecureRandom();

        SecretGenerator(String algorithm, int defaultBits) {
            this.algorithm = algorithm;
            this.defaultBits = defaultBits;
        }

        @Override protected void engineInit(SecureRandom r) {
            throw new IllegalStateException("AndroidKeyStore needs a KeyGenParameterSpec");
        }

        @Override protected void engineInit(int bits, SecureRandom r) {
            throw new IllegalStateException("AndroidKeyStore needs a KeyGenParameterSpec");
        }

        @Override protected void engineInit(AlgorithmParameterSpec params, SecureRandom r)
                throws java.security.InvalidAlgorithmParameterException {
            if (!(params instanceof KeyGenParameterSpec)) {
                throw new java.security.InvalidAlgorithmParameterException("expected KeyGenParameterSpec");
            }
            spec = (KeyGenParameterSpec) params;
            if (r != null) random = r;
        }

        @Override protected SecretKey engineGenerateKey() {
            if (spec == null) throw new IllegalStateException("not initialized");
            int bits = spec.getKeySize() > 0 ? spec.getKeySize() : defaultBits;
            byte[] key = new byte[algorithm.equals("DESede") ? 24 : bits / 8];
            random.nextBytes(key);
            Entry e = new Entry();
            e.kind = SECRET;
            e.algorithm = algorithm;
            e.key = key;
            put(spec.getKeystoreAlias(), e);
            return new SecretKeySpec(key, algorithm);
        }

        public static final class AES extends SecretGenerator { public AES() { super("AES", 128); } }
        public static final class Des extends SecretGenerator { public Des() { super("DESede", 168); } }
        public static final class HmacSHA1 extends SecretGenerator { public HmacSHA1() { super("HmacSHA1", 160); } }
        public static final class HmacSHA224 extends SecretGenerator { public HmacSHA224() { super("HmacSHA224", 224); } }
        public static final class HmacSHA256 extends SecretGenerator { public HmacSHA256() { super("HmacSHA256", 256); } }
        public static final class HmacSHA384 extends SecretGenerator { public HmacSHA384() { super("HmacSHA384", 384); } }
        public static final class HmacSHA512 extends SecretGenerator { public HmacSHA512() { super("HmacSHA512", 512); } }
    }

    // ---- KeyPairGenerator ----

    public abstract static class PairGenerator extends KeyPairGeneratorSpi {
        private final String algorithm;
        private KeyGenParameterSpec spec;
        private SecureRandom random = new SecureRandom();

        PairGenerator(String algorithm) { this.algorithm = algorithm; }

        @Override public void initialize(int keysize, SecureRandom r) {
            throw new IllegalArgumentException("AndroidKeyStore needs a KeyGenParameterSpec");
        }

        @Override public void initialize(AlgorithmParameterSpec params, SecureRandom r)
                throws java.security.InvalidAlgorithmParameterException {
            if (!(params instanceof KeyGenParameterSpec)) {
                throw new java.security.InvalidAlgorithmParameterException("expected KeyGenParameterSpec");
            }
            spec = (KeyGenParameterSpec) params;
            if (r != null) random = r;
        }

        @Override public KeyPair generateKeyPair() {
            if (spec == null) throw new IllegalStateException("not initialized");
            try {
                KeyPairGenerator g = KeyPairGenerator.getInstance(algorithm);
                AlgorithmParameterSpec ap = spec.getAlgorithmParameterSpec();
                int bits = spec.getKeySize();
                if (algorithm.equals("EC")) {
                    if (!(ap instanceof ECGenParameterSpec)) {
                        String curve = bits == 224 ? "secp224r1" : bits == 384 ? "secp384r1"
                                : bits == 521 ? "secp521r1" : "secp256r1";
                        ap = new ECGenParameterSpec(curve);
                    }
                } else if (!(ap instanceof RSAKeyGenParameterSpec)) {
                    ap = new RSAKeyGenParameterSpec(bits > 0 ? bits : 2048, RSAKeyGenParameterSpec.F4);
                }
                g.initialize(ap, random);
                KeyPair kp = g.generateKeyPair();
                X509Certificate cert = selfSigned(kp, spec);
                Entry e = new Entry();
                e.kind = PRIVATE;
                e.algorithm = algorithm;
                e.key = kp.getPrivate().getEncoded();
                e.chain.add(cert.getEncoded());
                put(spec.getKeystoreAlias(), e);
                return new KeyPair(cert.getPublicKey(), kp.getPrivate());
            } catch (RuntimeException ex) {
                throw ex;
            } catch (Exception ex) {
                throw new IllegalStateException("key pair generation failed", ex);
            }
        }

        public static final class Rsa extends PairGenerator { public Rsa() { super("RSA"); } }
        public static final class Ec extends PairGenerator { public Ec() { super("EC"); } }
    }

    // ---- self-signed X.509 certificate (minimal DER) ----

    static X509Certificate selfSigned(KeyPair kp, KeyGenParameterSpec spec) throws Exception {
        boolean ec = kp.getPrivate().getAlgorithm().equals("EC");
        byte[] sigAlg = ec ? seq(oid(1, 2, 840, 10045, 4, 3, 2))
                           : seq(oid(1, 2, 840, 113549, 1, 1, 11), new byte[] {0x05, 0x00});
        X500Principal subject = spec.getCertificateSubject() != null ? spec.getCertificateSubject()
                : new X500Principal("CN=fake");
        BigInteger serial = spec.getCertificateSerialNumber() != null ? spec.getCertificateSerialNumber()
                : BigInteger.ONE;
        Date notBefore = spec.getCertificateNotBefore() != null ? spec.getCertificateNotBefore() : new Date(0);
        Date notAfter = spec.getCertificateNotAfter() != null ? spec.getCertificateNotAfter()
                : new Date(2461449600000L);  // 2048-01-01, as KeyGenParameterSpec's default
        byte[] name = subject.getEncoded();
        byte[] tbs = seq(
                tlv(0xa0, tlv(0x02, new byte[] {2})),  // v3
                tlv(0x02, serial.toByteArray()),
                sigAlg, name,
                seq(time(notBefore), time(notAfter)),
                name,
                kp.getPublic().getEncoded());
        Signature s = Signature.getInstance(ec ? "SHA256withECDSA" : "SHA256withRSA");
        s.initSign(kp.getPrivate());
        s.update(tbs);
        byte[] sig = s.sign();
        byte[] bits = new byte[sig.length + 1];
        System.arraycopy(sig, 0, bits, 1, sig.length);
        byte[] der = seq(tbs, sigAlg, tlv(0x03, bits));
        return (X509Certificate) CertificateFactory.getInstance("X.509")
                .generateCertificate(new ByteArrayInputStream(der));
    }

    private static byte[] time(Date d) {
        Calendar c = Calendar.getInstance(TimeZone.getTimeZone("UTC"));
        c.setTime(d);
        int year = c.get(Calendar.YEAR);
        boolean utc = year >= 1950 && year < 2050;
        SimpleDateFormat f = new SimpleDateFormat(utc ? "yyMMddHHmmss'Z'" : "yyyyMMddHHmmss'Z'", Locale.US);
        f.setTimeZone(TimeZone.getTimeZone("UTC"));
        return tlv(utc ? 0x17 : 0x18, f.format(d).getBytes(java.nio.charset.StandardCharsets.US_ASCII));
    }

    private static byte[] oid(int... arcs) {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        b.write(arcs[0] * 40 + arcs[1]);
        for (int i = 2; i < arcs.length; i++) {
            int v = arcs[i];
            int n = 0;
            byte[] tmp = new byte[5];
            do {
                tmp[n++] = (byte) (v & 0x7f);
                v >>>= 7;
            } while (v != 0);
            for (int j = n - 1; j >= 0; j--) b.write(tmp[j] | (j > 0 ? 0x80 : 0));
        }
        return tlv(0x06, b.toByteArray());
    }

    private static byte[] seq(byte[]... parts) {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        for (byte[] p : parts) b.write(p, 0, p.length);
        return tlv(0x30, b.toByteArray());
    }

    private static byte[] tlv(int tag, byte[] value) {
        ByteArrayOutputStream b = new ByteArrayOutputStream();
        b.write(tag);
        int n = value.length;
        if (n < 0x80) {
            b.write(n);
        } else {
            int bytes = n > 0xffffff ? 4 : n > 0xffff ? 3 : n > 0xff ? 2 : 1;
            b.write(0x80 | bytes);
            for (int i = bytes - 1; i >= 0; i--) b.write(n >>> (8 * i));
        }
        b.write(value, 0, n);
        return b.toByteArray();
    }

    private AndroidKeyStore() {}
}
