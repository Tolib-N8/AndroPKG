package dev.andropkg.system;

import android.os.IBinder;

/**
 * android.media.IAudioService: a phone with a speaker, normal ringer mode and stream volumes
 * the app can read and set (the device's own volume buttons scale the real output). Audio
 * focus is always granted: on iOS, the system arbitrates between apps.
 */
final class AudioServiceImpl {
    // AudioSystem.DEFAULT_STREAM_VOLUME / MAX_STREAM_VOLUME for streams 0..11.
    private static final int[] MAX = {5, 7, 7, 15, 7, 7, 15, 7, 15, 15, 15, 15};
    private final int[] volume = {4, 5, 5, 11, 6, 5, 7, 5, 11, 11, 11, 11};
    private int ringerMode = 2;  // RINGER_MODE_NORMAL

    private static boolean valid(int stream) { return stream >= 0 && stream < MAX.length; }

    public synchronized int getStreamVolume(int stream) { return valid(stream) ? volume[stream] : 0; }
    public int getStreamMaxVolume(int stream) { return valid(stream) ? MAX[stream] : 0; }
    public int getStreamMinVolume(int stream) { return stream == 0 ? 1 : 0; }
    public synchronized int getLastAudibleStreamVolume(int stream) { return getStreamVolume(stream); }

    public synchronized void setStreamVolume(int stream, int index, int flags, String pkg) {
        if (valid(stream)) volume[stream] = Math.max(0, Math.min(MAX[stream], index));
    }

    public void setStreamVolumeWithAttribution(int stream, int index, int flags, String pkg, String tag) {
        setStreamVolume(stream, index, flags, pkg);
    }

    // direction: ADJUST_LOWER -1, ADJUST_SAME 0, ADJUST_RAISE 1, ADJUST_MUTE -100, ADJUST_UNMUTE 100
    public synchronized void adjustStreamVolume(int stream, int direction, int flags, String pkg) {
        if (valid(stream) && Math.abs(direction) == 1) setStreamVolume(stream, volume[stream] + direction, flags, pkg);
    }

    public void adjustStreamVolumeWithAttribution(int stream, int direction, int flags, String pkg, String tag) {
        adjustStreamVolume(stream, direction, flags, pkg);
    }

    public boolean isStreamMute(int stream) { return false; }
    public boolean isMasterMute() { return false; }
    public boolean isVolumeFixed() { return false; }
    public boolean isStreamAffectedByRingerMode(int stream) { return stream == 2 || stream == 5; }
    public boolean isValidRingerMode(int mode) { return mode >= 0 && mode <= 2; }
    public synchronized int getRingerModeExternal() { return ringerMode; }
    public synchronized int getRingerModeInternal() { return ringerMode; }
    public synchronized void setRingerModeExternal(int mode, String caller) { ringerMode = mode; }
    public synchronized void setRingerModeInternal(int mode, String caller) { ringerMode = mode; }
    public int getMode() { return 0; /* MODE_NORMAL */ }
    public int getUiSoundsStreamType() { return 1; /* STREAM_SYSTEM */ }
    public boolean shouldVibrate(int type) { return false; }

    // requestAudioFocus(AudioAttributes, int durationHint, IBinder cb, IAudioFocusDispatcher fd,
    //     String clientId, String callingPackage, String attributionTag, int flags,
    //     IAudioPolicyCallback pcb, int sdk)
    public int requestAudioFocus(Object attributes, int durationHint, IBinder cb, Object dispatcher, String clientId,
            String callingPackage, String attributionTag, int flags, Object policyCallback, int sdk) {
        return 1;  // AUDIOFOCUS_REQUEST_GRANTED
    }

    public int abandonAudioFocus(Object dispatcher, String clientId, Object attributes, String callingPackage) {
        return 1;
    }

    public int getFocusRampTimeMs(int focusGain, Object attributes) { return 0; }
}
