/* PASTIME: ContentProvider that publishes ROM files under the Pastime
 * root as content:// URIs, so external emulators we launch via Intent
 * can read them under modern Android's scoped-storage rules.
 *
 * Minimal-by-design: we don't want AndroidX FileProvider (Phoenix isn't
 * an AndroidX project), and the surface area we actually need is tiny —
 * one openFile + one query + an MIME helper.
 *
 * URI shape: content://<auth>/file/<percent-encoded-absolute-path>.
 * The path is encoded as a single opaque segment with `Uri.encode(path)`
 * (which encodes '/', '?', '#', '%', '+', and unicode), then
 * appendEncodedPath()'d.  This is the only safe round-trip for absolute
 * paths through Uri's hierarchical-path machinery — appendPath() splits
 * on '/' and inconsistently escapes reserved chars across API levels,
 * producing URIs that misparse for ROMs in subdirectories or with
 * '?'/'#' in the filename.
 *
 * Read-only: openFile ignores the requested mode and always opens
 * MODE_READ_ONLY.  External emulators have no business writing back
 * through us; saves go to their own dirs.
 *
 * Authority: ${applicationId}.romprovider, declared in AndroidManifest.
 * exported=false + per-Intent FLAG_GRANT_READ_URI_PERMISSION (with a
 * ClipData attached for extra-passed URIs); only the activity we hand
 * the URI to can resolve it.
 */
package com.retroarch.browser.pastime;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;

public class PastimeRomProvider extends ContentProvider {

  /* URI: content://<auth>/file/<encoded-path> */
  private static final String FILE_SEGMENT = "file";

  /* Defense-in-depth confinement.  Every storage tier our root resolver
   * picks (full external, scoped app-external, app-private) ends with
   * a "/Pastime/Roms/" subpath, so confining on that substring catches
   * all three tiers without re-deriving the tier walker in Java —
   * AND keeps the provider blind to the rest of the Pastime tree
   * (Bios/, Saves/, States/, the cache, etc.). */
  private static final String ROMS_MARKER = "/Pastime/Roms/";

  /* Build a content:// URI the receiving emulator can open.  See class
   * comment for why we encode the whole path as one opaque segment. */
  public static Uri uriForFile(Context context, File file) {
    String auth = context.getPackageName() + ".romprovider";
    /* Uri.encode(s) (single arg) encodes everything outside the
     * unreserved set, including '/', '?', '#', '+', '%', and unicode.
     * That gives us a single segment that round-trips losslessly. */
    String encoded = Uri.encode(file.getAbsolutePath());
    return new Uri.Builder()
        .scheme("content")
        .authority(auth)
        .appendEncodedPath(FILE_SEGMENT)
        .appendEncodedPath(encoded)
        .build();
  }

  @Override
  public boolean onCreate() {
    return true;
  }

  /* Decode the embedded path back out.  Returns null on a malformed URI
   * so callers convert to FileNotFoundException with context. */
  private static File fileFromUri(Uri uri) {
    if (uri == null) return null;
    String encodedPath = uri.getEncodedPath();
    if (encodedPath == null) return null;
    /* Expect: "/file/<encoded-abs-path>".  Anything else is malformed. */
    String prefix = "/" + FILE_SEGMENT + "/";
    if (!encodedPath.startsWith(prefix)) return null;
    String tail = encodedPath.substring(prefix.length());
    if (tail.isEmpty()) return null;
    String decoded = Uri.decode(tail);
    if (decoded == null || decoded.isEmpty()) return null;
    return new File(decoded);
  }

  /* Confinement check used by openFile/query.  Canonicalises (resolves
   * symlinks) and rejects anything outside Pastime/Roms/.  A textual-
   * containment alternative without I/O is used by getType() for
   * cheaper probing-resistance. */
  private static File resolveSafely(Uri uri) throws FileNotFoundException {
    File f = fileFromUri(uri);
    if (f == null || !f.exists()) {
      throw new FileNotFoundException("No such ROM: " + uri);
    }
    String canon;
    try {
      canon = f.getCanonicalPath();
    } catch (java.io.IOException e) {
      throw new FileNotFoundException("Cannot canonicalise: " + uri);
    }
    if (!canon.contains(ROMS_MARKER)) {
      throw new FileNotFoundException("URI outside Pastime/Roms/: " + uri);
    }
    return f;
  }

  @Override
  public ParcelFileDescriptor openFile(Uri uri, String mode)
      throws FileNotFoundException {
    File f = resolveSafely(uri);
    return ParcelFileDescriptor.open(f, ParcelFileDescriptor.MODE_READ_ONLY);
  }

  /* OpenableColumns supports receiving apps that call
   * getContentResolver().query() before openFile() — common pattern in
   * emulators that want the original filename for save-state naming. */
  @Override
  public Cursor query(Uri uri, String[] projection, String selection,
                      String[] selectionArgs, String sortOrder) {
    File f;
    try {
      f = resolveSafely(uri);
    } catch (FileNotFoundException e) {
      return null;
    }

    String[] cols = projection != null ? projection
        : new String[] { OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE };
    Object[] vals = new Object[cols.length];
    for (int i = 0; i < cols.length; i++) {
      if (OpenableColumns.DISPLAY_NAME.equals(cols[i])) {
        vals[i] = f.getName();
      } else if (OpenableColumns.SIZE.equals(cols[i])) {
        vals[i] = f.length();
      } else {
        vals[i] = null;
      }
    }
    MatrixCursor c = new MatrixCursor(cols, 1);
    c.addRow(vals);
    return c;
  }

  @Override
  public String getType(Uri uri) {
    /* getType() is callable without a URI grant by any app on the
     * device, so unconfined it'd be a path-existence MIME oracle.
     * Reject by textual prefix match (no I/O — we don't want to
     * incentivize crafted URIs as a stat-probing tool). */
    File f = fileFromUri(uri);
    if (f == null) return null;
    /* Textual confinement: must contain /Pastime/Roms/ and must not
     * contain ".." anywhere — the latter rules out probes that pivot
     * out of the ROM tree via backtracking segments without us having
     * to canonicalise (which would itself be a stat-probe oracle). */
    String abs = f.getAbsolutePath();
    if (!abs.contains(ROMS_MARKER) || abs.contains("/..")) return null;
    String name = f.getName();
    int dot = name.lastIndexOf('.');
    if (dot < 0 || dot == name.length() - 1) return "application/octet-stream";
    String ext = name.substring(dot + 1).toLowerCase(java.util.Locale.ROOT);
    String mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(ext);
    return mime != null ? mime : "application/octet-stream";
  }

  /* Read-only provider — write paths intentionally inert. */
  @Override public Uri insert(Uri uri, ContentValues values) { return null; }
  @Override public int delete(Uri uri, String s, String[] a) { return 0; }
  @Override public int update(Uri uri, ContentValues v, String s, String[] a) {
    return 0;
  }
}
