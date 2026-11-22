package dev.andropkg.system;

import android.os.IBinder;
import android.os.IInterface;
import android.os.Parcel;
import java.io.FileDescriptor;

/**
 * An activity's token. On a device it is a proxy to the system server's ActivityRecord; the
 * framework treats a local Binder token as an activity it doesn't manage (ActivityGroup era):
 * ActivityThread.scheduleRelaunchActivityIfPossible skips "token instanceof Binder", so
 * Activity.recreate() did nothing. This is an IBinder that is not a Binder.
 */
final class ActivityToken implements IBinder {
    private final String name;

    ActivityToken(String name) {
        this.name = name;
    }

    @Override public String getInterfaceDescriptor() { return null; }
    @Override public boolean pingBinder() { return true; }
    @Override public boolean isBinderAlive() { return true; }
    @Override public IInterface queryLocalInterface(String descriptor) { return null; }
    @Override public void dump(FileDescriptor fd, String[] args) {}
    @Override public void dumpAsync(FileDescriptor fd, String[] args) {}
    @Override public boolean transact(int code, Parcel data, Parcel reply, int flags) { return false; }
    @Override public void linkToDeath(DeathRecipient recipient, int flags) {}
    @Override public boolean unlinkToDeath(DeathRecipient recipient, int flags) { return true; }
    @Override public String toString() { return "ActivityToken{" + name + "}"; }
}
