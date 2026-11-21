package dev.andropkg.system;

import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkInfo;
import android.net.NetworkRequest;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Message;
import android.os.Messenger;
import android.util.Log;
import java.util.HashMap;
import java.util.Map;

/**
 * android.net.IConnectivityManager: one always-connected, validated, unmetered Wi-Fi network.
 * The host OS owns real connectivity (sockets just work), so apps that check "is there a
 * network?" before fetching should see one. Network callbacks get onAvailable right away.
 */
final class ConnectivityManagerImpl {
    private static final String TAG = "andropkg";
    private static final int NET_ID = 100;
    private static final int TYPE_WIFI = 1;
    // ConnectivityManager.CALLBACK_AVAILABLE (handled by ConnectivityManager.CallbackHandler).
    private static final int CALLBACK_AVAILABLE = 2;

    private final Network network;
    private final NetworkCapabilities caps;
    private final Map<NetworkRequest, Messenger> callbacks = new HashMap<>();
    private int nextRequestId = 1;

    ConnectivityManagerImpl() {
        try {
            network = Network.class.getDeclaredConstructor(int.class).newInstance(NET_ID);
        } catch (ReflectiveOperationException e) {
            throw new RuntimeException(e);
        }
        caps = new NetworkCapabilities();
        Hidden.call(caps, "addTransportType", NetworkCapabilities.TRANSPORT_WIFI);
        for (int c : new int[] {
                NetworkCapabilities.NET_CAPABILITY_INTERNET, NetworkCapabilities.NET_CAPABILITY_VALIDATED,
                NetworkCapabilities.NET_CAPABILITY_NOT_METERED, NetworkCapabilities.NET_CAPABILITY_NOT_RESTRICTED,
                NetworkCapabilities.NET_CAPABILITY_TRUSTED, NetworkCapabilities.NET_CAPABILITY_NOT_VPN,
                NetworkCapabilities.NET_CAPABILITY_NOT_ROAMING, NetworkCapabilities.NET_CAPABILITY_NOT_CONGESTED,
                NetworkCapabilities.NET_CAPABILITY_NOT_SUSPENDED, NetworkCapabilities.NET_CAPABILITY_FOREGROUND}) {
            Hidden.call(caps, "addCapability", c);
        }
        Hidden.call(caps, "setLinkDownstreamBandwidthKbps", 100_000);
        Hidden.call(caps, "setLinkUpstreamBandwidthKbps", 50_000);
    }

    private NetworkInfo info() {
        NetworkInfo ni = (NetworkInfo) Hidden.newInstance(NetworkInfo.class, TYPE_WIFI, 0, "WIFI", "");
        Hidden.call(ni, "setDetailedState", NetworkInfo.DetailedState.CONNECTED, null, null);
        Hidden.call(ni, "setIsAvailable", true);
        return ni;
    }

    private boolean isOurs(Network n) {
        return n == null || n.equals(network);
    }

    public Network getActiveNetwork() { return network; }
    public Network getActiveNetworkForUid(int uid, boolean ignoreBlocked) { return network; }
    public NetworkInfo getActiveNetworkInfo() { return info(); }
    public NetworkInfo getActiveNetworkInfoForUid(int uid, boolean ignoreBlocked) { return info(); }
    public NetworkInfo getNetworkInfo(int type) { return type == TYPE_WIFI ? info() : null; }
    public NetworkInfo getNetworkInfoForUid(Network n, int uid, boolean ignoreBlocked) {
        return isOurs(n) ? info() : null;
    }
    public NetworkInfo[] getAllNetworkInfo() { return new NetworkInfo[] {info()}; }
    public Network[] getAllNetworks() { return new Network[] {network}; }
    public NetworkCapabilities getNetworkCapabilities(Network n, String pkg, String tag) {
        return isOurs(n) ? new NetworkCapabilities(caps) : null;
    }
    public LinkProperties getLinkProperties(Network n) {
        if (!isOurs(n)) return null;
        LinkProperties lp = new LinkProperties();
        try {
            Hidden.call(lp, "setInterfaceName", "wlan0");
        } catch (RuntimeException ignored) {
        }
        return lp;
    }
    public LinkProperties getLinkPropertiesForType(int type) {
        return type == TYPE_WIFI ? getLinkProperties(network) : null;
    }
    public boolean isActiveNetworkMetered() { return false; }
    public int getRestrictBackgroundStatusByCaller() { return 1; /* RESTRICT_BACKGROUND_STATUS_DISABLED */ }
    public boolean isNetworkSupported(int type) { return type == TYPE_WIFI; }

    private NetworkRequest newRequest(NetworkCapabilities need, String type) {
        try {
            @SuppressWarnings({"unchecked", "rawtypes"})
            Object t = Enum.valueOf((Class) Class.forName("android.net.NetworkRequest$Type"), type);
            int id;
            synchronized (this) { id = nextRequestId++; }
            return (NetworkRequest) Hidden.newInstance(NetworkRequest.class,
                    need != null ? need : new NetworkCapabilities(), -1, id, t);
        } catch (ClassNotFoundException e) {
            throw new RuntimeException(e);
        }
    }

    /** The one network satisfies a request unless it asks for a transport other than Wi-Fi. */
    private boolean satisfies(NetworkCapabilities need) {
        if (need == null) return true;
        int[] transports = (int[]) Hidden.call(need, "getTransportTypes");
        if (transports.length == 0) return true;
        for (int t : transports) if (t == NetworkCapabilities.TRANSPORT_WIFI) return true;
        return false;
    }

    private void track(NetworkRequest req, NetworkCapabilities need, Messenger messenger) {
        if (messenger == null) return;
        synchronized (callbacks) { callbacks.put(req, messenger); }
        if (!satisfies(need)) return;
        Message msg = Message.obtain();
        msg.what = CALLBACK_AVAILABLE;
        Bundle b = new Bundle();
        b.putParcelable("NetworkRequest", req);
        b.putParcelable("Network", network);
        b.putParcelable("NetworkCapabilities", new NetworkCapabilities(caps));
        b.putParcelable("LinkProperties", getLinkProperties(network));
        msg.setData(b);
        try {
            messenger.send(msg);
        } catch (Exception e) {
            Log.w(TAG, "connectivity: callback delivery failed", e);
        }
    }

    public NetworkRequest listenForNetwork(NetworkCapabilities need, Messenger messenger, IBinder binder,
            int callbackFlags, String pkg, String tag) {
        NetworkRequest req = newRequest(need, "LISTEN");
        track(req, need, messenger);
        return req;
    }

    public NetworkRequest requestNetwork(int asUid, NetworkCapabilities need, int reqType, Messenger messenger,
            int timeoutMs, IBinder binder, int legacyType, int callbackFlags, String pkg, String tag) {
        // reqType: 1 = TRACK_DEFAULT (registerDefaultNetworkCallback), 2 = REQUEST, 3 = BACKGROUND_REQUEST,
        // 4 = TRACK_SYSTEM_DEFAULT, 5 = LISTEN_FOR_BEST.
        String type;
        switch (reqType) {
            case 1: type = "TRACK_DEFAULT"; break;
            case 3: type = "BACKGROUND_REQUEST"; break;
            case 4: type = "TRACK_SYSTEM_DEFAULT"; break;
            case 5: type = "LISTEN_FOR_BEST"; break;
            default: type = "REQUEST"; break;
        }
        NetworkRequest req = newRequest(need, type);
        track(req, need, messenger);
        return req;
    }

    public void releaseNetworkRequest(NetworkRequest req) {
        synchronized (callbacks) { callbacks.remove(req); }
    }
}
