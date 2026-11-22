package dev.andropkg.system;

import android.accounts.Account;
import android.accounts.AccountManager;
import android.accounts.AuthenticatorDescription;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import android.os.Bundle;
import android.util.Log;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * android.accounts.IAccountManager for the one app: its own account authenticators (services
 * with android.accounts.AccountAuthenticator meta-data) and the accounts it stores (password,
 * user data, auth tokens), persisted next to its data as AccountManagerService's accounts.db is.
 * Apps like Yandex Music keep their login there (Yandex Passport). Authenticators of other apps
 * (Google) don't exist here; requests that need an authenticator's UI aren't supported yet.
 */
final class AccountManagerImpl {
    static final String TAG = "andropkg-accounts";

    private static final class Entry {
        String password;
        Map<String, String> userData = new LinkedHashMap<>();
        Map<String, String> tokens = new LinkedHashMap<>();
    }

    private File file;
    private Map<Account, Entry> accounts;
    private AuthenticatorDescription[] authenticators;

    /** Loaded on first use: services exist before the app's data directory is known. */
    private synchronized Map<Account, Entry> db() {
        if (accounts == null) {
            accounts = new LinkedHashMap<>();
            File dir = new File(AndroPkgSystem.hostDataDir + "_accounts");
            dir.mkdirs();
            file = new File(dir, "accounts");
            load();
        }
        return accounts;
    }

    // ---- persistence ----

    private synchronized void load() {
        if (!file.exists()) return;
        try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
            int n = in.readInt();
            for (int i = 0; i < n; i++) {
                Account a = new Account(in.readUTF(), in.readUTF());
                Entry e = new Entry();
                e.password = in.readBoolean() ? in.readUTF() : null;
                readMap(in, e.userData);
                readMap(in, e.tokens);
                accounts.put(a, e);
            }
        } catch (Exception ex) {
            Log.w(TAG, "unreadable " + file, ex);
        }
    }

    private synchronized void save() {
        File tmp = new File(file.getPath() + ".tmp");
        try (DataOutputStream out = new DataOutputStream(new FileOutputStream(tmp))) {
            out.writeInt(accounts.size());
            for (Map.Entry<Account, Entry> me : accounts.entrySet()) {
                out.writeUTF(me.getKey().name);
                out.writeUTF(me.getKey().type);
                Entry e = me.getValue();
                out.writeBoolean(e.password != null);
                if (e.password != null) out.writeUTF(e.password);
                writeMap(out, e.userData);
                writeMap(out, e.tokens);
            }
        } catch (Exception ex) {
            Log.e(TAG, "cannot write " + tmp, ex);
            return;
        }
        if (!tmp.renameTo(file)) Log.e(TAG, "cannot replace " + file);
    }

    private static void readMap(DataInputStream in, Map<String, String> m) throws java.io.IOException {
        int n = in.readInt();
        for (int i = 0; i < n; i++) {
            String k = in.readUTF();
            m.put(k, in.readBoolean() ? in.readUTF() : null);
        }
    }

    private static void writeMap(DataOutputStream out, Map<String, String> m) throws java.io.IOException {
        out.writeInt(m.size());
        for (Map.Entry<String, String> e : m.entrySet()) {
            out.writeUTF(e.getKey());
            out.writeBoolean(e.getValue() != null);
            if (e.getValue() != null) out.writeUTF(e.getValue());
        }
    }

    /** As AccountManagerService: apps listening for account changes get these broadcasts. */
    private void changed() {
        save();
        for (String action : new String[] {AccountManager.LOGIN_ACCOUNTS_CHANGED_ACTION,
                "android.accounts.action.VISIBLE_ACCOUNTS_CHANGED"}) {
            try {
                Context c = (Context) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "currentApplication");
                if (c != null) c.sendBroadcast(new Intent(action).setPackage(c.getPackageName()));
            } catch (Throwable t) {
                Log.w(TAG, "broadcast " + action, t);
            }
        }
    }

    // ---- authenticators ----

    public synchronized AuthenticatorDescription[] getAuthenticatorTypes(int userId) {
        if (authenticators == null) authenticators = readAuthenticators();
        return authenticators;
    }

    private static AuthenticatorDescription[] readAuthenticators() {
        List<AuthenticatorDescription> out = new ArrayList<>();
        Context c = (Context) Hidden.callStatic(Hidden.cls("android.app.ActivityThread"), "currentApplication");
        if (c == null) return new AuthenticatorDescription[0];
        Resources res = c.getResources();
        String ns = "http://schemas.android.com/apk/res/android";
        for (ServiceInfo s : AndroPkgSystem.<ServiceInfo>infos("services")) {
            int xml = s.metaData == null ? 0 : s.metaData.getInt(AccountManager.AUTHENTICATOR_META_DATA_NAME, 0);
            if (xml == 0) continue;
            try (XmlResourceParser p = res.getXml(xml)) {
                int ev;
                while ((ev = p.next()) != XmlResourceParser.END_DOCUMENT) {
                    if (ev != XmlResourceParser.START_TAG || !"account-authenticator".equals(p.getName())) continue;
                    int typeRes = p.getAttributeResourceValue(ns, "accountType", 0);
                    String type = typeRes != 0 ? res.getString(typeRes) : p.getAttributeValue(ns, "accountType");
                    if (type == null) break;
                    out.add(new AuthenticatorDescription(type, c.getPackageName(),
                            p.getAttributeResourceValue(ns, "label", 0), p.getAttributeResourceValue(ns, "icon", 0),
                            p.getAttributeResourceValue(ns, "smallIcon", 0),
                            p.getAttributeResourceValue(ns, "accountPreferences", 0),
                            p.getAttributeBooleanValue(ns, "customTokens", false)));
                    Log.i(TAG, "authenticator " + type + " (" + s.name + ")");
                    break;
                }
            } catch (Exception e) {
                Log.w(TAG, "bad authenticator meta-data in " + s.name, e);
            }
        }
        return out.toArray(new AuthenticatorDescription[0]);
    }

    // ---- accounts ----

    private synchronized Account[] ofType(String type) {
        List<Account> out = new ArrayList<>();
        for (Account a : db().keySet()) if (type == null || type.equals(a.type)) out.add(a);
        return out.toArray(new Account[0]);
    }

    public Account[] getAccountsAsUser(String type, int userId, String op) { return ofType(type); }
    public Account[] getAccountsForPackage(String pkg, int uid, String op) { return ofType(null); }
    public Account[] getAccountsByTypeForPackage(String type, String pkg, String op) { return ofType(type); }

    public synchronized Map<Account, Integer> getAccountsAndVisibilityForPackage(String pkg, String type) {
        Map<Account, Integer> m = new HashMap<>();
        for (Account a : ofType(type)) m.put(a, AccountManager.VISIBILITY_VISIBLE);
        return m;
    }

    public synchronized boolean addAccountExplicitly(Account a, String password, Bundle extras, String op) {
        if (a == null || db().containsKey(a)) return false;
        Entry e = new Entry();
        e.password = password;
        if (extras != null) for (String k : extras.keySet()) e.userData.put(k, String.valueOf(extras.get(k)));
        db().put(a, e);
        changed();
        return true;
    }

    public boolean addAccountExplicitlyWithVisibility(Account a, String password, Bundle extras,
                                                      Map<String, Integer> visibility, String op) {
        return addAccountExplicitly(a, password, extras, op);
    }

    public synchronized boolean removeAccountExplicitly(Account a) {
        if (db().remove(a) == null) return false;
        changed();
        return true;
    }

    public void removeAccountAsUser(Object response, Account a, boolean expectActivityLaunch, int userId) {
        Bundle b = new Bundle();
        b.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, removeAccountExplicitly(a));
        result(response, b);
    }

    public synchronized void renameAccount(Object response, Account a, String newName) {
        Entry e = db().remove(a);
        Account renamed = new Account(newName, a.type);
        if (e != null) db().put(renamed, e);
        changed();
        Bundle b = new Bundle();
        b.putString(AccountManager.KEY_ACCOUNT_NAME, renamed.name);
        b.putString(AccountManager.KEY_ACCOUNT_TYPE, renamed.type);
        result(response, b);
    }

    public synchronized String getPassword(Account a) {
        Entry e = db().get(a);
        return e == null ? null : e.password;
    }

    public synchronized void setPassword(Account a, String password) {
        Entry e = db().get(a);
        if (e == null) return;
        e.password = password;
        e.tokens.clear();  // (as AccountManagerService: a new password invalidates the tokens)
        save();
    }

    public void clearPassword(Account a) { setPassword(a, null); }

    public synchronized String getUserData(Account a, String key) {
        Entry e = db().get(a);
        return e == null ? null : e.userData.get(key);
    }

    public synchronized void setUserData(Account a, String key, String value) {
        Entry e = db().get(a);
        if (e == null) return;
        if (value == null) e.userData.remove(key); else e.userData.put(key, value);
        save();
    }

    public synchronized String peekAuthToken(Account a, String type) {
        Entry e = db().get(a);
        return e == null ? null : e.tokens.get(type);
    }

    public synchronized void setAuthToken(Account a, String type, String token) {
        Entry e = db().get(a);
        if (e == null) return;
        if (token == null) e.tokens.remove(type); else e.tokens.put(type, token);
        save();
    }

    public synchronized void invalidateAuthToken(String accountType, String token) {
        boolean any = false;
        for (Map.Entry<Account, Entry> me : db().entrySet()) {
            if (!me.getKey().type.equals(accountType)) continue;
            any |= me.getValue().tokens.values().removeIf(t -> t != null && t.equals(token));
        }
        if (any) save();
    }

    /** A cached token answers at once; minting one would need the authenticator (not yet). */
    public void getAuthToken(Object response, Account a, String type, boolean notifyOnFailure,
                             boolean expectActivityLaunch, Bundle options) {
        String token = peekAuthToken(a, type);
        if (token == null) {
            error(response, AccountManager.ERROR_CODE_UNSUPPORTED_OPERATION, "no cached token");
            return;
        }
        Bundle b = new Bundle();
        b.putString(AccountManager.KEY_ACCOUNT_NAME, a.name);
        b.putString(AccountManager.KEY_ACCOUNT_TYPE, a.type);
        b.putString(AccountManager.KEY_AUTHTOKEN, token);
        result(response, b);
    }

    public void getAccountsByFeatures(Object response, String type, String[] features, String op) {
        Bundle b = new Bundle();
        b.putParcelableArray(AccountManager.KEY_ACCOUNTS, ofType(type));
        result(response, b);
    }

    public void getAccountByTypeAndFeatures(Object response, String type, String[] features, String op) {
        Account[] as = ofType(type);
        Bundle b = new Bundle();
        if (as.length == 1) {
            b.putString(AccountManager.KEY_ACCOUNT_NAME, as[0].name);
            b.putString(AccountManager.KEY_ACCOUNT_TYPE, as[0].type);
        }
        result(response, b);
    }

    public void hasFeatures(Object response, Account a, String[] features, int userId, String op) {
        Bundle b = new Bundle();
        b.putBoolean(AccountManager.KEY_BOOLEAN_RESULT, true);
        result(response, b);
    }

    public synchronized int getAccountVisibility(Account a, String pkg) {
        return db().containsKey(a) ? AccountManager.VISIBILITY_VISIBLE : AccountManager.VISIBILITY_NOT_VISIBLE;
    }

    public boolean setAccountVisibility(Account a, String pkg, int visibility) { return true; }

    public synchronized Map<String, Integer> getPackagesAndVisibilityForAccount(Account a) {
        Map<String, Integer> m = new HashMap<>();
        if (db().containsKey(a)) m.put(AndroPkgSystem.appInfo.packageName, AccountManager.VISIBILITY_VISIBLE);
        return m;
    }

    public synchronized boolean accountAuthenticated(Account a) { return db().containsKey(a); }
    public synchronized boolean someUserHasAccount(Account a) { return db().containsKey(a); }
    public boolean hasAccountAccess(Account a, String pkg, android.os.UserHandle user) { return true; }

    // ---- IAccountManagerResponse (an in-process binder: the stub itself) ----

    private static void result(Object response, Bundle b) {
        if (response == null) return;
        try {
            Hidden.call(response, "onResult", b);
        } catch (Throwable t) {
            Log.w(TAG, "onResult", t);
        }
    }

    private static void error(Object response, int code, String msg) {
        if (response == null) return;
        try {
            Hidden.call(response, "onError", code, msg);
        } catch (Throwable t) {
            Log.w(TAG, "onError", t);
        }
    }
}
