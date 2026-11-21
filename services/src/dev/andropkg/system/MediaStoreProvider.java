package dev.andropkg.system;

import android.content.ContentValues;
import android.content.Intent;
import android.content.pm.ProviderInfo;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.database.sqlite.SQLiteDatabase;
import android.database.sqlite.SQLiteQueryBuilder;
import android.graphics.BitmapFactory;
import android.media.MediaMetadataRetriever;
import android.net.Uri;
import android.os.Binder;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.os.Process;
import android.util.Log;
import android.webkit.MimeTypeMap;
import java.io.File;
import java.io.FileNotFoundException;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;

/**
 * The "media" content provider (MediaStore): an index of shared storage (/storage/emulated/0,
 * the iOS app's Documents) in an in-memory SQLite database, built on first use and on
 * MediaStore scan requests. Audio tags come from MediaMetadataRetriever (AVFoundation),
 * image sizes from BitmapFactory. Apps query it with their own projections, selections and
 * sort orders, through views shaped like MediaStore's tables:
 *   content://media/<volume>/{file, audio/media, audio/albums, audio/artists, audio/genres,
 *                              audio/playlists, images/media, video/media, downloads}[/<id>]
 */
final class MediaStoreProvider {
    static final String AUTHORITY = "media";
    private static final String TAG = "andropkg";
    private static final String ROOT = StorageManagerImpl.PRIMARY;
    private static final int TYPE_NONE = 0, TYPE_IMAGE = 1, TYPE_AUDIO = 2, TYPE_VIDEO = 3;

    private SQLiteDatabase db;
    private long generation;
    private long scannedAt;

    // ---- index ---------------------------------------------------------------------------------
    private synchronized SQLiteDatabase db() {
        if (db == null) {
            db = SQLiteDatabase.create(null);
            db.execSQL("CREATE TABLE files (_id INTEGER PRIMARY KEY, _data TEXT UNIQUE, _display_name TEXT,"
                    + " _size INTEGER, mime_type TEXT, media_type INTEGER, title TEXT, date_added INTEGER,"
                    + " date_modified INTEGER, datetaken INTEGER, relative_path TEXT, bucket_id INTEGER,"
                    + " bucket_display_name TEXT, volume_name TEXT DEFAULT 'external_primary', is_music INTEGER DEFAULT 0,"
                    + " is_alarm INTEGER DEFAULT 0, is_notification INTEGER DEFAULT 0, is_ringtone INTEGER DEFAULT 0,"
                    + " is_podcast INTEGER DEFAULT 0, is_audiobook INTEGER DEFAULT 0, is_recording INTEGER DEFAULT 0,"
                    + " is_pending INTEGER DEFAULT 0, is_trashed INTEGER DEFAULT 0, is_favorite INTEGER DEFAULT 0,"
                    + " is_download INTEGER DEFAULT 0, is_drm INTEGER DEFAULT 0, artist TEXT, artist_id INTEGER,"
                    + " album TEXT, album_id INTEGER, album_artist TEXT, composer TEXT, genre TEXT, genre_id INTEGER,"
                    + " track INTEGER, cd_track_number TEXT, disc_number TEXT, year INTEGER, duration INTEGER,"
                    + " bitrate INTEGER, width INTEGER, height INTEGER, orientation INTEGER DEFAULT 0,"
                    + " owner_package_name TEXT, generation_added INTEGER, generation_modified INTEGER)");
            db.execSQL("CREATE VIEW audio_media AS SELECT *, _id AS audio_id FROM files WHERE media_type=2");
            db.execSQL("CREATE VIEW images_media AS SELECT * FROM files WHERE media_type=1");
            db.execSQL("CREATE VIEW video_media AS SELECT * FROM files WHERE media_type=3");
            db.execSQL("CREATE VIEW downloads AS SELECT * FROM files WHERE is_download=1");
            db.execSQL("CREATE VIEW albums AS SELECT album_id AS _id, album_id, album, album AS album_key,"
                    + " COALESCE(album_artist, artist) AS artist, artist_id, COUNT(*) AS numsongs,"
                    + " COUNT(*) AS numsongs_by_artist, MIN(year) AS minyear, MAX(year) AS maxyear,"
                    + " MIN(year) AS first_year, MAX(year) AS last_year, NULL AS album_art"
                    + " FROM files WHERE media_type=2 GROUP BY album_id");
            db.execSQL("CREATE VIEW artists AS SELECT artist_id AS _id, artist, artist AS artist_key,"
                    + " COUNT(DISTINCT album_id) AS number_of_albums, COUNT(*) AS number_of_tracks"
                    + " FROM files WHERE media_type=2 GROUP BY artist_id");
            db.execSQL("CREATE VIEW genres AS SELECT genre_id AS _id, genre AS name FROM files"
                    + " WHERE media_type=2 AND genre IS NOT NULL GROUP BY genre_id");
            db.execSQL("CREATE TABLE playlists (_id INTEGER PRIMARY KEY, name TEXT, _data TEXT,"
                    + " date_added INTEGER, date_modified INTEGER)");
            scan();
        }
        return db;
    }

    /** (Re)indexes shared storage; keeps ids of files already known. */
    private synchronized void scan() {
        long t0 = System.currentTimeMillis();
        List<File> found = new ArrayList<>();
        walk(new File(ROOT), found, 0);
        generation++;
        db.beginTransaction();
        try {
            for (File f : found) index(f);
            List<String> gone = new ArrayList<>();
            try (Cursor c = db.rawQuery("SELECT _data FROM files", null)) {
                while (c.moveToNext()) if (!new File(c.getString(0)).exists()) gone.add(c.getString(0));
            }
            for (String p : gone) db.delete("files", "_data=?", new String[] {p});
            db.setTransactionSuccessful();
        } finally {
            db.endTransaction();
        }
        scannedAt = System.currentTimeMillis();
        Log.i(TAG, "media store: " + found.size() + " files indexed in " + (scannedAt - t0) + " ms");
    }

    private static void walk(File dir, List<File> out, int depth) {
        File[] list = dir.listFiles();
        if (list == null || depth > 12) return;
        for (File f : list) {
            String name = f.getName();
            if (name.startsWith(".")) continue;
            if (f.isDirectory()) {
                if (depth == 0 && name.equals("Android")) continue;  // app-private trees
                if (new File(f, ".nomedia").exists()) continue;
                walk(f, out, depth + 1);
            } else if (mediaType(mime(name)) != TYPE_NONE || dir.getName().equals("Download") && depth == 1) {
                out.add(f);
            }
        }
    }

    static String mime(String name) {
        int dot = name.lastIndexOf('.');
        if (dot < 0) return null;
        String ext = name.substring(dot + 1).toLowerCase(Locale.ROOT);
        String m = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
        if (m == null && ext.equals("opus")) m = "audio/ogg";
        return m;
    }

    private static int mediaType(String mime) {
        if (mime == null) return TYPE_NONE;
        if (mime.startsWith("audio/")) return TYPE_AUDIO;
        if (mime.startsWith("image/")) return TYPE_IMAGE;
        if (mime.startsWith("video/")) return TYPE_VIDEO;
        return TYPE_NONE;
    }

    private void index(File f) {
        String path = f.getPath();
        long modified = f.lastModified() / 1000;
        try (Cursor c = db.rawQuery("SELECT date_modified FROM files WHERE _data=?", new String[] {path})) {
            if (c.moveToFirst() && c.getLong(0) == modified) return;  // unchanged
        }
        String name = f.getName();
        String mime = mime(name);
        int type = mediaType(mime);
        String parent = f.getParent();
        String rel = parent.length() > ROOT.length() ? parent.substring(ROOT.length() + 1) + "/" : "";
        ContentValues v = new ContentValues();
        v.put("_data", path);
        v.put("_display_name", name);
        v.put("_size", f.length());
        v.put("mime_type", mime);
        v.put("media_type", type);
        int dot = name.lastIndexOf('.');
        v.put("title", dot > 0 ? name.substring(0, dot) : name);
        v.put("date_added", modified);
        v.put("date_modified", modified);
        v.put("datetaken", f.lastModified());
        v.put("relative_path", rel);
        v.put("bucket_id", parent.toLowerCase(Locale.ROOT).hashCode());
        v.put("bucket_display_name", new File(parent).getName());
        v.put("is_download", rel.startsWith("Download/") ? 1 : 0);
        v.put("generation_added", generation);
        v.put("generation_modified", generation);
        if (type == TYPE_AUDIO) audioTags(f, v);
        if (type == TYPE_IMAGE) {
            BitmapFactory.Options o = new BitmapFactory.Options();
            o.inJustDecodeBounds = true;
            BitmapFactory.decodeFile(path, o);
            if (o.outWidth > 0) {
                v.put("width", o.outWidth);
                v.put("height", o.outHeight);
            }
        }
        db.insertWithOnConflict("files", null, v, SQLiteDatabase.CONFLICT_REPLACE);
    }

    private static void audioTags(File f, ContentValues v) {
        v.put("is_music", 1);
        String artist = null, album = null, genre = null;
        MediaMetadataRetriever r = new MediaMetadataRetriever();
        try {
            r.setDataSource(f.getPath());
            String title = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_TITLE);
            if (title != null && !title.isEmpty()) v.put("title", title);
            artist = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_ARTIST);
            album = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_ALBUM);
            genre = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_GENRE);
            v.put("album_artist", r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_ALBUMARTIST));
            v.put("composer", r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_COMPOSER));
            v.put("duration", parseLong(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)));
            v.put("bitrate", parseLong(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_BITRATE)));
            String track = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_CD_TRACK_NUMBER);
            String disc = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DISC_NUMBER);
            v.put("cd_track_number", track);
            v.put("disc_number", disc);
            // MediaStore's "track" is disc * 1000 + track.
            v.put("track", parseLong(disc) * 1000 + parseLong(track));
            v.put("year", parseLong(r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_YEAR)));
        } catch (RuntimeException e) {
            // Unsupported format (Ogg, ...): file name only.
        } finally {
            try { r.release(); } catch (Exception ignored) {}
        }
        if (artist == null || artist.isEmpty()) artist = "<unknown>";
        if (album == null || album.isEmpty()) album = new File(f.getParent()).getName();
        v.put("artist", artist);
        v.put("album", album);
        v.put("genre", genre);
        v.put("artist_id", artist.toLowerCase(Locale.ROOT).hashCode() & 0x7fffffffL);
        v.put("album_id", (album + "\u0000" + f.getParent()).toLowerCase(Locale.ROOT).hashCode() & 0x7fffffffL);
        if (genre != null) v.put("genre_id", genre.toLowerCase(Locale.ROOT).hashCode() & 0x7fffffffL);
    }

    private static long parseLong(String s) {
        if (s == null) return 0;
        int slash = s.indexOf('/');
        try {
            return Long.parseLong((slash >= 0 ? s.substring(0, slash) : s).trim());
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    // ---- URIs ----------------------------------------------------------------------------------
    /** Table/view + extra selection for a media URI; null if unknown. */
    private static String[] route(Uri uri) {
        List<String> s = uri.getPathSegments();
        if (s.isEmpty()) return null;
        boolean internal = s.get(0).equals("internal");
        String where = internal ? "0" : null;  // nothing on the internal (system sounds) volume
        String table = null, id = null;
        String kind = s.size() > 1 ? s.get(1) : "";
        String sub = s.size() > 2 ? s.get(2) : "";
        switch (kind) {
            case "file": table = "files"; id = sub; break;
            case "downloads": table = "downloads"; id = sub; break;
            case "images": table = "images_media"; id = s.size() > 3 ? s.get(3) : null; break;
            case "video": table = "video_media"; id = s.size() > 3 ? s.get(3) : null; break;
            case "audio":
                String third = s.size() > 3 ? s.get(3) : null;
                switch (sub) {
                    case "media": table = "audio_media"; id = third; break;
                    case "albums": table = "albums"; id = third; break;
                    case "artists":
                        if (s.size() > 4 && s.get(4).equals("albums")) {
                            table = "albums";
                            where = and(where, "artist_id=" + Long.parseLong(third));
                        } else {
                            table = "artists";
                            id = third;
                        }
                        break;
                    case "genres":
                        if (s.size() > 4 && s.get(4).equals("members")) {
                            table = "audio_media";
                            where = and(where, "genre_id=" + Long.parseLong(third));
                        } else {
                            table = "genres";
                            id = third;
                        }
                        break;
                    case "playlists":
                        if (s.size() > 4 && s.get(4).equals("members")) {
                            table = "audio_media";
                            where = and(where, "0");
                        } else {
                            table = "playlists";
                            id = third;
                        }
                        break;
                    default: return null;
                }
                break;
            default: return null;
        }
        if (id != null && !id.isEmpty()) where = and(where, "_id=" + Long.parseLong(id));
        return new String[] {table, where};
    }

    private static String and(String a, String b) {
        return a == null ? b : "(" + a + ") AND (" + b + ")";
    }

    // ---- IContentProvider ----------------------------------------------------------------------
    public Cursor query(Object attribution, Uri uri, String[] projection, Bundle args, Object cancel) {
        String[] r = route(uri);
        if (r == null) {
            Log.w(TAG, "media store: unsupported query " + uri);
            return new MatrixCursor(projection != null ? projection : new String[] {"_id"});
        }
        String selection = args != null ? args.getString("android:query-arg-sql-selection") : null;
        String[] selArgs = args != null ? args.getStringArray("android:query-arg-sql-selection-args") : null;
        String sort = args != null ? args.getString("android:query-arg-sql-sort-order") : null;
        String limit = args != null && args.containsKey("android:query-arg-limit")
                ? String.valueOf(args.getInt("android:query-arg-limit")) : null;
        if (limit != null && args.containsKey("android:query-arg-offset")) {
            limit = args.getInt("android:query-arg-offset") + "," + limit;
        }
        SQLiteQueryBuilder qb = new SQLiteQueryBuilder();
        qb.setTables(r[0]);
        if (r[1] != null) qb.appendWhere(r[1]);
        synchronized (this) {
            try {
                return qb.query(db(), projection, selection, selArgs, null, null, sort, limit);
            } catch (RuntimeException e) {
                Log.w(TAG, "media store: query " + uri + " failed: " + e);
                throw e;
            }
        }
    }

    public String getType(Object attribution, Uri uri) {
        String[] r = route(uri);
        if (r == null) return null;
        if (r[1] != null && r[1].contains("_id=") && !r[0].equals("albums") && !r[0].equals("artists")) {
            try (Cursor c = query(attribution, uri, new String[] {"mime_type"}, null, null)) {
                if (c.moveToFirst()) return c.getString(0);
            }
        }
        switch (r[0]) {
            case "audio_media": return "vnd.android.cursor.dir/audio";
            case "images_media": return "vnd.android.cursor.dir/image";
            case "video_media": return "vnd.android.cursor.dir/video";
            case "albums": return "vnd.android.cursor.dir/albums";
            case "artists": return "vnd.android.cursor.dir/artists";
            default: return "vnd.android.cursor.dir/file";
        }
    }

    private File fileFor(Uri uri) throws FileNotFoundException {
        String[] r = route(uri);
        if (r != null && r[1] != null && r[1].contains("_id=")) {
            try (Cursor c = query(null, uri, new String[] {"_data"}, null, null)) {
                if (c.moveToFirst()) return new File(c.getString(0));
            } catch (RuntimeException ignored) {
            }
        }
        throw new FileNotFoundException("No media for " + uri);
    }

    public ParcelFileDescriptor openFile(Object attribution, Uri uri, String mode, Object cancel)
            throws FileNotFoundException {
        return ParcelFileDescriptor.open(fileFor(uri), ParcelFileDescriptor.parseMode(mode));
    }

    public AssetFileDescriptor openAssetFile(Object attribution, Uri uri, String mode, Object cancel)
            throws FileNotFoundException {
        return new AssetFileDescriptor(openFile(attribution, uri, mode, cancel), 0, AssetFileDescriptor.UNKNOWN_LENGTH);
    }

    public AssetFileDescriptor openTypedAssetFile(Object attribution, Uri uri, String mimeFilter, Bundle opts,
            Object cancel) throws FileNotFoundException {
        return openAssetFile(attribution, uri, "r", cancel);
    }

    public int delete(Object attribution, Uri uri, Bundle args) {
        try {
            File f = fileFor(uri);
            if (f.delete()) {
                synchronized (this) { db().delete("files", "_data=?", new String[] {f.getPath()}); }
                return 1;
            }
        } catch (FileNotFoundException ignored) {
        }
        return 0;
    }

    public Uri canonicalize(Object attribution, Uri uri) { return uri; }
    public Uri uncanonicalize(Object attribution, Uri uri) { return uri; }

    // call(): MediaStore.getVersion/getGeneration and scan requests.
    public Bundle call(Object attribution, String authority, String method, String arg, Bundle extras) {
        Bundle out = new Bundle();
        switch (method) {
            case "get_version":
                db();
                out.putString(Intent.EXTRA_TEXT, "andropkg:" + scannedAt);
                return out;
            case "get_generation":
                db();
                out.putLong(Intent.EXTRA_INDEX, generation);
                return out;
            case "scan_file":
            case "scan_volume":
                synchronized (this) {
                    if (db == null) db(); else scan();
                }
                return out;
            default:
                Log.w(TAG, "media store: call " + method + " not supported");
                return out;
        }
    }

    /** android.app.ContentProviderHolder handed out by IActivityManager.getContentProvider. */
    Object holder() {
        ProviderInfo info = new ProviderInfo();
        info.authority = AUTHORITY;
        info.name = "com.android.providers.media.MediaProvider";
        info.packageName = "com.android.providers.media.module";
        info.exported = true;
        info.applicationInfo = new android.content.pm.ApplicationInfo();
        info.applicationInfo.packageName = info.packageName;
        info.applicationInfo.uid = Process.SYSTEM_UID + 13;
        Object holder = Hidden.newInstance(Hidden.cls("android.app.ContentProviderHolder"), info);
        LocalService.LocalBinder binder = LocalService.create("android.content.IContentProvider", this);
        Hidden.setField(holder, "provider", binder.iface);
        Hidden.setField(holder, "connection", new Binder());
        Hidden.setField(holder, "noReleaseNeeded", true);
        return holder;
    }
}
