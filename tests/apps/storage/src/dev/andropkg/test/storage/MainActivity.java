package dev.andropkg.test.storage;

import android.app.Activity;
import android.content.ContentValues;
import android.content.Context;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteOpenHelper;
import android.os.Bundle;
import android.util.Log;
import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.security.KeyStore;

/** M3 test: SQLite, SharedPreferences, files, and HTTP over loopback (okhttp + sockets). */
public class MainActivity extends Activity {
    static void log(String s) {
        Log.i("StorageTest", s);
        System.out.println("[storage] " + s);
    }

    static final class Db extends SQLiteOpenHelper {
        Db(Context c) { super(c, "notes.db", null, 1); }
        @Override public void onCreate(SQLiteDatabase db) {
            db.execSQL("CREATE TABLE notes (_id INTEGER PRIMARY KEY, text TEXT)");
        }
        @Override public void onUpgrade(SQLiteDatabase db, int o, int n) {}
    }

    @Override protected void onCreate(Bundle b) {
        super.onCreate(b);
        try {
            sqlite();
            prefs();
            files();
            keystore();
            new Thread(MainActivity::http).start();
        } catch (Exception e) {
            log("FAILED " + e);
            throw new RuntimeException(e);
        }
    }

    void sqlite() {
        SQLiteDatabase db = new Db(this).getWritableDatabase();
        db.delete("notes", null, null);
        for (String t : new String[] {"alpha", "beta", "гамма"}) {
            ContentValues v = new ContentValues();
            v.put("text", t);
            db.insert("notes", null, v);
        }
        StringBuilder sb = new StringBuilder();
        try (Cursor c = db.query("notes", null, "text != ?", new String[] {"beta"}, null, null, "_id")) {
            while (c.moveToNext()) sb.append(c.getString(1)).append(';');
        }
        log("sqlite " + db.getVersion() + " rows: " + sb);
        db.close();
    }

    void prefs() {
        SharedPreferences p = getSharedPreferences("settings", MODE_PRIVATE);
        int runs = p.getInt("runs", 0) + 1;
        p.edit().putInt("runs", runs).putString("name", "andropkg").commit();
        log("prefs name=" + p.getString("name", null) + " runs=" + runs);
    }

    void files() throws Exception {
        File f = new File(getFilesDir(), "hello.txt");
        try (FileOutputStream out = new FileOutputStream(f)) {
            out.write("файл ok".getBytes(StandardCharsets.UTF_8));
        }
        log("file " + f.length() + " bytes, cache dir exists=" + getCacheDir().isDirectory());
    }

    /** AndroidKeyStore: an AES key that survives restarts (apps encrypt credentials with it). */
    void keystore() {
        try {
            java.security.KeyStore ks = java.security.KeyStore.getInstance("AndroidKeyStore");
            ks.load(null);
            boolean existed = ks.containsAlias("test-key");
            if (!existed) {
                javax.crypto.KeyGenerator g = javax.crypto.KeyGenerator.getInstance("AES", "AndroidKeyStore");
                g.init(new android.security.keystore.KeyGenParameterSpec.Builder("test-key",
                        android.security.keystore.KeyProperties.PURPOSE_ENCRYPT
                                | android.security.keystore.KeyProperties.PURPOSE_DECRYPT)
                        .setBlockModes("GCM").setEncryptionPaddings("NoPadding").build());
                g.generateKey();
            }
            javax.crypto.SecretKey key = (javax.crypto.SecretKey) ks.getKey("test-key", null);
            javax.crypto.Cipher c = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
            c.init(javax.crypto.Cipher.ENCRYPT_MODE, key);
            byte[] iv = c.getIV();
            byte[] ct = c.doFinal("secret".getBytes(StandardCharsets.UTF_8));
            javax.crypto.Cipher d = javax.crypto.Cipher.getInstance("AES/GCM/NoPadding");
            d.init(javax.crypto.Cipher.DECRYPT_MODE, key, new javax.crypto.spec.GCMParameterSpec(128, iv));
            String back = new String(d.doFinal(ct), StandardCharsets.UTF_8);
            if (!ks.containsAlias("test-ec")) {
                java.security.KeyPairGenerator kg = java.security.KeyPairGenerator.getInstance("EC", "AndroidKeyStore");
                kg.initialize(new android.security.keystore.KeyGenParameterSpec.Builder("test-ec",
                        android.security.keystore.KeyProperties.PURPOSE_SIGN).setDigests("SHA-256").build());
                kg.generateKeyPair();
            }
            KeyStore.PrivateKeyEntry pe = (KeyStore.PrivateKeyEntry) ks.getEntry("test-ec", null);
            java.security.Signature sg = java.security.Signature.getInstance("SHA256withECDSA");
            sg.initSign(pe.getPrivateKey());
            sg.update(ct);
            byte[] sig = sg.sign();
            sg.initVerify(pe.getCertificate());
            sg.update(ct);
            boolean verified = sg.verify(sig);
            log("keystore existed=" + existed + " roundtrip=" + back.equals("secret") + " ec=" + verified
                    + " aliases=" + java.util.Collections.list(ks.aliases()).size());
        } catch (Throwable e) {
            log("keystore FAILED " + e);
            e.printStackTrace();
        }
    }

    static void http() {
        try (ServerSocket server = new ServerSocket(0)) {
            Thread t = new Thread(() -> {
                try (Socket s = server.accept()) {
                    BufferedReader in = new BufferedReader(new InputStreamReader(s.getInputStream()));
                    while (!in.readLine().isEmpty()) {}
                    OutputStream out = s.getOutputStream();
                    out.write("HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello"
                            .getBytes(StandardCharsets.US_ASCII));
                    out.flush();
                } catch (Exception e) {
                    log("server FAILED " + e);
                    e.printStackTrace();
                }
            });
            t.start();
            HttpURLConnection c = (HttpURLConnection) new URL(
                    "http://localhost:" + server.getLocalPort() + "/").openConnection();
            BufferedReader r = new BufferedReader(new InputStreamReader(c.getInputStream()));
            log("http " + c.getResponseCode() + " body=" + r.readLine());
            t.join();
        } catch (Exception e) {
            log("http FAILED " + e);
        }
        log("done");
        System.exit(0);
    }
}
