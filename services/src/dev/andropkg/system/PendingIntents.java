package dev.andropkg.system;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Process;
import android.util.Log;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;

/**
 * PendingIntents (IIntentSender) of the app, as ActivityManagerService's PendingIntentController
 * keeps them: one sender per (type, intent, requestCode), with FLAG_NO_CREATE / CANCEL_CURRENT /
 * UPDATE_CURRENT / ONE_SHOT / IMMUTABLE. Sending starts the activity or service, or broadcasts.
 */
final class PendingIntents {
    private static final String TAG = "andropkg";
    static final int TYPE_BROADCAST = 1, TYPE_ACTIVITY = 2, TYPE_SERVICE = 4, TYPE_FOREGROUND_SERVICE = 5;
    private static final int FLAG_ONE_SHOT = 1 << 30, FLAG_NO_CREATE = 1 << 29, FLAG_CANCEL_CURRENT = 1 << 28,
            FLAG_UPDATE_CURRENT = 1 << 27, FLAG_IMMUTABLE = 1 << 26;

    private static final class Key {
        final int type;
        final Intent.FilterComparison intent;
        final int requestCode;

        Key(int type, Intent intent, int requestCode) {
            this.type = type;
            this.intent = new Intent.FilterComparison(intent);
            this.requestCode = requestCode;
        }

        @Override public boolean equals(Object o) {
            return o instanceof Key && ((Key) o).type == type && ((Key) o).requestCode == requestCode
                    && ((Key) o).intent.equals(intent);
        }

        @Override public int hashCode() {
            return Objects.hash(type, intent, requestCode);
        }
    }

    /** android.content.IIntentSender. */
    final class Sender {
        final Key key;
        final Intent[] intents;
        final int flags;
        Object iface;
        boolean canceled;

        Sender(Key key, Intent[] intents, int flags) {
            this.key = key;
            this.intents = intents;
            this.flags = flags;
        }

        // send(int code, Intent intent, String resolvedType, IBinder whitelistToken,
        //      IIntentReceiver finishedReceiver, String requiredPermission, Bundle options)
        public void send(int code, Intent fillIn, String resolvedType, IBinder whitelistToken, Object finishedReceiver,
                String requiredPermission, Bundle options) {
            PendingIntents.this.send(this, code, fillIn, finishedReceiver);
        }
    }

    private final Map<Key, Sender> senders = new HashMap<>();
    private final Broadcasts broadcasts;
    private final ActiveServices services;

    PendingIntents(Broadcasts broadcasts, ActiveServices services) {
        this.broadcasts = broadcasts;
        this.services = services;
    }

    synchronized Object get(int type, Intent[] intents, int requestCode, int flags) {
        if (intents == null || intents.length == 0) return null;
        Intent last = intents[intents.length - 1];
        Key key = new Key(type, last, requestCode);
        Sender s = senders.get(key);
        if (s != null && (flags & FLAG_CANCEL_CURRENT) != 0) {
            s.canceled = true;
            senders.remove(key);
            s = null;
        }
        if (s != null) {
            if ((flags & FLAG_UPDATE_CURRENT) != 0) s.intents[s.intents.length - 1].replaceExtras(last.getExtras());
            return s.iface;
        }
        if ((flags & FLAG_NO_CREATE) != 0) return null;
        Intent[] copy = new Intent[intents.length];
        for (int i = 0; i < intents.length; i++) copy[i] = new Intent(intents[i]);
        s = new Sender(key, copy, flags);
        s.iface = LocalService.create("android.content.IIntentSender", s).iface;
        senders.put(key, s);
        return s.iface;
    }

    private Sender find(Object target) {
        if (target == null) return null;
        IBinder b = ((android.os.IInterface) target).asBinder();
        synchronized (this) {
            for (Sender s : senders.values()) {
                if (((android.os.IInterface) s.iface).asBinder() == b) return s;
            }
        }
        return null;
    }

    int send(Object target, int code, Intent fillIn, Object finishedReceiver) {
        Sender s = find(target);
        return s == null ? -96 /* START_CANCELED */ : send(s, code, fillIn, finishedReceiver);
    }

    int send(Sender s, int code, Intent fillIn, Object finishedReceiver) {
        if (s.canceled) return -96;
        Intent intent = new Intent(s.intents[s.intents.length - 1]);
        if (fillIn != null && (s.flags & FLAG_IMMUTABLE) == 0) intent.fillIn(fillIn, 0);
        if ((s.flags & FLAG_ONE_SHOT) != 0) cancel(s.iface);
        Log.i(TAG, "pending intent sent: type " + s.key.type + " " + intent);
        switch (s.key.type) {
            case TYPE_BROADCAST:
                broadcasts.send(intent, false, finishedReceiver);
                return 0;
            case TYPE_ACTIVITY:
                for (int i = 0; i < s.intents.length; i++) {
                    Intent a = i == s.intents.length - 1 ? intent : s.intents[i];
                    ActivityInfo info = ActivityStack.resolve(a);
                    if (info != null) AndroPkgSystem.stack.start(info, new Intent(a), null, null, -1);
                }
                break;
            case TYPE_SERVICE:
            case TYPE_FOREGROUND_SERVICE:
                services.start(intent);
                break;
            default:
                break;
        }
        if (finishedReceiver != null) {
            Hidden.call(finishedReceiver, "performReceive", intent, 0, null, null, false, false, 0);
        }
        return 0;
    }

    synchronized void cancel(Object target) {
        Sender s = find(target);
        if (s == null) return;
        s.canceled = true;
        senders.remove(s.key);
    }

    Intent intentOf(Object target) {
        Sender s = find(target);
        return s == null ? null : new Intent(s.intents[s.intents.length - 1]);
    }

    /** ActivityManager.PendingIntentInfo(creatorPackage, creatorUid, immutable, type). */
    Object infoOf(Object target) {
        Sender s = find(target);
        if (s == null) return null;
        return Hidden.newInstance(Hidden.cls("android.app.ActivityManager$PendingIntentInfo"),
                AndroPkgSystem.appInfo.packageName, Process.myUid(), (s.flags & FLAG_IMMUTABLE) != 0, s.key.type);
    }
}
