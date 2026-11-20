package dev.andropkg.test.multi;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;

public class NotesProvider extends ContentProvider {
    @Override public boolean onCreate() { Trace.log("NotesProvider.onCreate"); return true; }
    @Override public Cursor query(Uri uri, String[] p, String s, String[] a, String o) {
        MatrixCursor c = new MatrixCursor(new String[] {"_id", "text"});
        c.addRow(new Object[] {1, "first note"});
        c.addRow(new Object[] {2, "second note"});
        return c;
    }
    @Override public String getType(Uri uri) { return "vnd.android.cursor.dir/note"; }
    @Override public Uri insert(Uri uri, ContentValues v) { return null; }
    @Override public int delete(Uri uri, String s, String[] a) { return 0; }
    @Override public int update(Uri uri, ContentValues v, String s, String[] a) { return 0; }
}
