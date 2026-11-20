// M2b smoke test: framework resources (Resources/AssetManager on libandroidfw) read straight
// from Android 14's framework-res.apk.
import android.content.res.Resources;
import android.content.res.XmlResourceParser;
import org.xmlpull.v1.XmlPullParser;

public class ResourcesTest {
    public static void main(String[] args) {
        try {
            Resources r = Resources.getSystem();
            System.out.println("strings: ok=\"" + r.getString(android.R.string.ok) + "\" cancel=\""
                    + r.getString(android.R.string.cancel) + "\" yes=\"" + r.getString(android.R.string.yes) + "\"");
            System.out.println("color holo_blue_dark=#" + Integer.toHexString(r.getColor(android.R.color.holo_blue_dark, null)));
            System.out.println("dimen app_icon_size=" + r.getDimension(android.R.dimen.app_icon_size) + "px");
            System.out.println("config: " + r.getConfiguration());
            XmlResourceParser p = r.getLayout(android.R.layout.simple_list_item_1);
            for (int ev = p.next(); ev != XmlPullParser.END_DOCUMENT; ev = p.next()) {
                if (ev == XmlPullParser.START_TAG) {
                    StringBuilder sb = new StringBuilder("layout <" + p.getName());
                    for (int i = 0; i < p.getAttributeCount(); i++) {
                        sb.append(' ').append(p.getAttributeName(i)).append('=').append(p.getAttributeValue(i));
                    }
                    System.out.println(sb.append('>'));
                }
            }
            p.close();
            System.out.println("resources ok");
        } catch (Throwable t) {
            System.out.println("FAILED: " + t);
            t.printStackTrace(System.out);
        }
    }
}
