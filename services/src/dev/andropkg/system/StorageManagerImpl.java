package dev.andropkg.system;

import android.os.Process;
import android.os.UserHandle;
import android.os.storage.StorageVolume;
import java.io.File;

/**
 * android.os.storage.IStorageManager: one primary, emulated, always-mounted shared storage
 * volume at /storage/emulated/0 (the runtime maps it to a host directory; on iOS the app's
 * Documents, visible in the Files app).
 */
final class StorageManagerImpl {
    static final String PRIMARY = "/storage/emulated/0";
    private StorageVolume primary;

    private synchronized StorageVolume primary() {
        if (primary == null) {
            File path = new File(PRIMARY);
            // The host directory behind it (the runtime translates the path).
            path.mkdirs();
            for (String d : new String[] {"Download", "Documents", "Pictures", "Music", "Movies", "DCIM"}) {
                new File(path, d).mkdirs();
            }
            // StorageVolume(id, path, internalPath, description, primary, removable, emulated,
            //               externallyManaged, allowMassStorage, maxFileSize, owner, uuid, fsUuid, state)
            primary = (StorageVolume) Hidden.newInstance(StorageVolume.class, "emulated;0", path, path,
                    "Internal storage", true, false, true, false, false, 0L,
                    UserHandle.getUserHandleForUid(Process.myUid()), null, null, "mounted");
        }
        return primary;
    }

    public StorageVolume[] getVolumeList(int uid, String packageName, int flags) {
        return new StorageVolume[] {primary()};
    }

    public boolean isUserKeyUnlocked(int userId) { return true; }
    public boolean isCeStorageUnlocked(int userId) { return true; }
    public boolean isConvertibleToFBE() { return false; }
    public long getCacheQuotaBytes(String volumeUuid, int uid) { return 1L << 30; }
    public long getCacheSizeBytes(String volumeUuid, int uid) { return 0L; }
    public long getAllocatableBytes(String volumeUuid, int flags, String callingPackage) {
        return new File(PRIMARY).getUsableSpace();
    }
}
