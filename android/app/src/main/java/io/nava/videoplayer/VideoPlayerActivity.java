package io.nava.videoplayer;

import io.nava.appshell.AppShellActivity;

/**
 * The activity, and the one thing it exists to add: the static initializer that
 * loads this app's native library.
 *
 * <p>That block is not optional and cannot live in {@link AppShellActivity},
 * because only this app knows the library's name. Without it every {@code
 * native} method on the base class throws {@code UnsatisfiedLinkError} at
 * runtime — even though the library is already mapped into the process and its
 * symbols are exported. NativeActivity brings it up with {@code dlopen()} from
 * its own native code, which never tells the Java runtime about it, and the JVM
 * resolves native methods only against libraries it was itself asked to load.
 *
 * <p>Measured, not assumed: pointing the manifest at {@code AppShellActivity}
 * directly crash-looped on the first frame with "No implementation found for
 * void io.nava.appshell.AppShellActivity.nativeOnImeInset(int)", thrown from
 * the window-insets listener during the first traversal — before any of this
 * app's own native code had run, which is why the phase lines in
 * android/src/main.cc showed a clean start and then nothing.
 */
public class VideoPlayerActivity extends AppShellActivity {
    static { System.loadLibrary("video_player_android"); }
}
