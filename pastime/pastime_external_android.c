/* Pastime — Android JNI bridge for external-emulator launch.
 *
 * The pure-C side (pastime_external.c) decides "this folder maps to
 * package X with these intent extras"; this file is the dumb conduit
 * that carries those fields to the Java side, where Intent / Uri /
 * ContentProvider / startActivity actually live.
 *
 * One JNI hop per launch, no per-frame work — caching the methodID
 * across the process is enough.  We don't keep a cached global ref to
 * the activity class because the activity already owns it (via
 * g_android->activity->clazz) and that lifetime is the process.
 */

#include <stdbool.h>
#include <stddef.h>
#include <jni.h>

#include "../verbosity.h"
#include "pastime_external.h"

/* We don't include platform_unix.h here — its top-of-file declarations
 * are storage definitions, not externs, and pulling them into a second
 * TU triggers duplicate-symbol link errors.  Decls below mirror what
 * platform_unix.c provides; pastime_jni_get_activity_clazz is a
 * Pastime-specific helper added there because we need the activity
 * jobject without seeing struct android_app's layout. */
extern JNIEnv *jni_thread_getenv(void);
extern jobject pastime_jni_get_activity_clazz(void);

/* Method signature for RetroActivityCommon.pastimeLaunchExternal:
 *   void pastimeLaunchExternal(
 *      String pkg, String component, String action,
 *      String category, String extraKey, String mimeType,
 *      boolean killFirst, String romPath)
 *
 * The Java side posts onto the UI thread and reports failure via Toast,
 * so this is fire-and-forget from C — we have no useful response to a
 * launch failure once we've handed off. */
#define PASTIME_LAUNCH_METHOD_NAME "pastimeLaunchExternal"
#define PASTIME_LAUNCH_METHOD_SIG \
   "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;" \
   "Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;" \
   "ZLjava/lang/String;)V"

#define PASTIME_INSTALLED_METHOD_NAME "pastimeIsPackageInstalled"
#define PASTIME_INSTALLED_METHOD_SIG  "(Ljava/lang/String;)Z"

static jmethodID g_method_id;     /* cached after first successful resolve */
static bool      g_method_missing; /* true means stop trying — Java side
                                    * doesn't define the helper */
static jmethodID g_installed_id;
static bool      g_installed_missing;

/* Helper: build a jstring or NULL, since CallVoidMethod accepts NULL
 * for object args and the Java side checks for null per field.
 *
 * Returns false (and clears the JNI exception) if NewStringUTF threw
 * OutOfMemoryError — the caller bails before any further JNI activity.
 * Calling JNI with a pending exception is undefined per the JNI spec,
 * so we must not chain further allocations after a failure. */
static bool make_jstring_checked(JNIEnv *env, const char *s, jstring *out)
{
   if (!s)
   {
      *out = NULL;
      return true;
   }
   *out = (*env)->NewStringUTF(env, s);
   if ((*env)->ExceptionCheck(env))
   {
      (*env)->ExceptionClear(env);
      *out = NULL;
      return false;
   }
   /* NewStringUTF can return NULL without raising on a malformed UTF-8
    * sequence (rare here — preset strings are ASCII), treat as failure. */
   return *out != NULL;
}

bool pastime_external_android_launch(
      const pastime_external_spec_t *spec, const char *rom_path)
{
   JNIEnv  *env;
   jobject  activity;
   jclass   activity_class;
   jstring  j_pkg = NULL, j_component = NULL, j_action = NULL;
   jstring  j_category = NULL, j_extra_key = NULL, j_mime = NULL;
   jstring  j_rom_path = NULL;
   bool     posted = false;

   if (!spec || !rom_path || !*rom_path)
      return false;
   if (g_method_missing)
      return false;
   if (!(activity = pastime_jni_get_activity_clazz()))
      return false;
   if (!(env = jni_thread_getenv()))
      return false;

   if (!g_method_id)
   {
      activity_class = (*env)->GetObjectClass(env, activity);
      if (!activity_class)
         return false;
      g_method_id = (*env)->GetMethodID(env, activity_class,
            PASTIME_LAUNCH_METHOD_NAME, PASTIME_LAUNCH_METHOD_SIG);
      (*env)->DeleteLocalRef(env, activity_class);
      if (!g_method_id)
      {
         /* Older APK without the Pastime helper — clear the pending
          * exception and remember not to keep checking on every launch. */
         if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
         g_method_missing = true;
         RARCH_ERR("[Pastime] Java helper "
               PASTIME_LAUNCH_METHOD_NAME " not found on the activity "
               "(stale APK?). External launches will fail.\n");
         return false;
      }
   }

   if (   !make_jstring_checked(env, spec->package,    &j_pkg)
       || !make_jstring_checked(env, spec->component,  &j_component)
       || !make_jstring_checked(env, spec->action,     &j_action)
       || !make_jstring_checked(env, spec->category,   &j_category)
       || !make_jstring_checked(env, spec->extra_key,  &j_extra_key)
       || !make_jstring_checked(env, spec->mime_type,  &j_mime)
       || !make_jstring_checked(env, rom_path,         &j_rom_path))
   {
      RARCH_ERR("[Pastime] OOM while marshalling external launch args\n");
      goto cleanup;
   }

   (*env)->CallVoidMethod(env, activity, g_method_id,
         j_pkg, j_component, j_action, j_category,
         j_extra_key, j_mime,
         (jboolean)(spec->kill_first ? JNI_TRUE : JNI_FALSE),
         j_rom_path);
   posted = true;

   if ((*env)->ExceptionCheck(env))
   {
      (*env)->ExceptionDescribe(env);
      (*env)->ExceptionClear(env);
      posted = false;
   }

cleanup:

   /* Local refs: the JNI spec auto-frees them when control returns to
    * Java, but we're in a long-lived native callback — release explicitly
    * so we don't leak across hundreds of launches in one session. */
   if (j_pkg)       (*env)->DeleteLocalRef(env, j_pkg);
   if (j_component) (*env)->DeleteLocalRef(env, j_component);
   if (j_action)    (*env)->DeleteLocalRef(env, j_action);
   if (j_category)  (*env)->DeleteLocalRef(env, j_category);
   if (j_extra_key) (*env)->DeleteLocalRef(env, j_extra_key);
   if (j_mime)      (*env)->DeleteLocalRef(env, j_mime);
   if (j_rom_path)  (*env)->DeleteLocalRef(env, j_rom_path);

   /* "true" here means "we successfully posted onto the UI thread" — not
    * "the launch succeeded".  The Java side surfaces real launch failure
    * (package not installed, etc.) as a Toast, so the menu driver no
    * longer needs to. */
   return posted;
}

/* Synchronous install-check.  Called per external folder during menu
 * list build, so it has to be cheap — no UI hop, no Toast, just a JNI
 * round-trip to PackageManager.getPackageInfo (catch-NameNotFound).
 * Returns false on any failure path (missing helper, JNI error, OOM)
 * which conservatively *hides* the folder.  That matches the libretro
 * convention: when in doubt, hide rather than show a broken row. */
bool pastime_external_android_is_installed(const char *package)
{
   JNIEnv  *env;
   jobject  activity;
   jclass   activity_class;
   jstring  j_pkg = NULL;
   jboolean result = JNI_FALSE;

   if (!package || !*package)
      return false;
   if (g_installed_missing)
      return false;
   if (!(activity = pastime_jni_get_activity_clazz()))
      return false;
   if (!(env = jni_thread_getenv()))
      return false;

   if (!g_installed_id)
   {
      activity_class = (*env)->GetObjectClass(env, activity);
      if (!activity_class)
         return false;
      g_installed_id = (*env)->GetMethodID(env, activity_class,
            PASTIME_INSTALLED_METHOD_NAME, PASTIME_INSTALLED_METHOD_SIG);
      (*env)->DeleteLocalRef(env, activity_class);
      if (!g_installed_id)
      {
         if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
         g_installed_missing = true;
         RARCH_ERR("[Pastime] Java helper "
               PASTIME_INSTALLED_METHOD_NAME " not found on the activity "
               "(stale APK?). External folders will be hidden.\n");
         return false;
      }
   }

   if (!make_jstring_checked(env, package, &j_pkg))
      return false;

   result = (*env)->CallBooleanMethod(env, activity, g_installed_id, j_pkg);
   if ((*env)->ExceptionCheck(env))
   {
      (*env)->ExceptionDescribe(env);
      (*env)->ExceptionClear(env);
      result = JNI_FALSE;
   }

   (*env)->DeleteLocalRef(env, j_pkg);
   return result == JNI_TRUE;
}
