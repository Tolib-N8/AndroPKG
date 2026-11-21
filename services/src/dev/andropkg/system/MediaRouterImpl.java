package dev.andropkg.system;

import android.media.MediaRoute2Info;
import android.media.RoutingSessionInfo;
import java.util.Collections;
import java.util.List;

/**
 * android.media.IMediaRouterService: the device's own audio output as the one system route, in
 * the one system session — what MediaRouter2 requires to exist (it refuses to start otherwise).
 * No remote routes (Cast, Bluetooth discovery).
 */
final class MediaRouterImpl {
    static final String PROVIDER = "com.android.server.media/.SystemMediaRoute2Provider";
    static final String ROUTE = "DEFAULT_ROUTE";
    private final MediaRoute2Info route;
    private final RoutingSessionInfo session;

    MediaRouterImpl() {
        MediaRoute2Info.Builder b = new MediaRoute2Info.Builder(ROUTE, "Phone")
                .addFeature(MediaRoute2Info.FEATURE_LIVE_AUDIO)
                .addFeature(MediaRoute2Info.FEATURE_LIVE_VIDEO)
                .setVolumeHandling(MediaRoute2Info.PLAYBACK_VOLUME_VARIABLE)
                .setVolumeMax(15)
                .setVolume(10)
                .setConnectionState(MediaRoute2Info.CONNECTION_STATE_CONNECTED);
        Hidden.call(b, "setType", MediaRoute2Info.TYPE_BUILTIN_SPEAKER);
        Hidden.call(b, "setSystemRoute", true);
        Hidden.call(b, "setProviderId", PROVIDER);
        route = b.build();
        RoutingSessionInfo.Builder s = new RoutingSessionInfo.Builder("SYSTEM_SESSION", "")
                .addSelectedRoute(ROUTE)
                .setVolumeHandling(MediaRoute2Info.PLAYBACK_VOLUME_VARIABLE)
                .setVolumeMax(15)
                .setVolume(10);
        Hidden.call(s, "setProviderId", PROVIDER);
        Hidden.call(s, "setSystemSession", true);
        session = s.build();
    }

    public List<MediaRoute2Info> getSystemRoutes() { return Collections.singletonList(route); }
    public RoutingSessionInfo getSystemSessionInfo() { return session; }
    public RoutingSessionInfo getSystemSessionInfoForPackage(String pkg) { return session; }
    public List<RoutingSessionInfo> getRemoteSessions(Object manager) { return Collections.emptyList(); }
}
