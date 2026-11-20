package dev.andropkg.system;

/**
 * android.os.IUserManager: one user (0), the device owner, running and unlocked — the iOS
 * app is only running while the phone is unlocked enough to run it.
 */
final class UserManagerImpl {
    private Object owner;

    private synchronized Object owner() {
        if (owner == null) {
            Class<?> ui = Hidden.cls("android.content.pm.UserInfo");
            int flags = (int) Hidden.getField(ui, "FLAG_PRIMARY") | (int) Hidden.getField(ui, "FLAG_ADMIN")
                    | (int) Hidden.getField(ui, "FLAG_FULL") | (int) Hidden.getField(ui, "FLAG_SYSTEM")
                    | (int) Hidden.getField(ui, "FLAG_MAIN");
            owner = Hidden.newInstance(ui, 0, "Owner", flags);
        }
        return owner;
    }

    public boolean isUserUnlocked(int userId) { return userId == 0; }
    public boolean isUserUnlockingOrUnlocked(int userId) { return userId == 0; }
    public boolean isUserRunning(int userId) { return userId == 0; }
    public boolean isUserForeground(int userId) { return userId == 0; }
    public boolean isUserVisible(int userId) { return userId == 0; }
    public Object getUserInfo(int userId) { return userId == 0 ? owner() : null; }
    public Object getPrimaryUser() { return owner(); }
    public int getMainUserId() { return 0; }
    public int[] getProfileIds(int userId, boolean enabledOnly) { return new int[] {userId}; }
    public java.util.List<Object> getUsers(boolean excludePartial, boolean excludeDying, boolean excludePreCreated) {
        return java.util.Collections.singletonList(owner());
    }
    public java.util.List<Object> getProfiles(int userId, boolean enabledOnly) {
        return java.util.Collections.singletonList(owner());
    }
    public boolean isManagedProfile(int userId) { return false; }
    public boolean isRestricted(int userId) { return false; }
    public boolean isDemoUser(int userId) { return false; }
    public boolean hasUserRestriction(String key, int userId) { return false; }
    public android.os.Bundle getUserRestrictions(int userId) { return new android.os.Bundle(); }
    public android.os.Bundle getApplicationRestrictions(String packageName) { return new android.os.Bundle(); }
    public android.os.Bundle getApplicationRestrictionsForUser(String packageName, int userId) {
        return new android.os.Bundle();
    }
    public String getUserName() { return "Owner"; }
    public long getUserCreationTime(int userId) { return 0L; }
}
