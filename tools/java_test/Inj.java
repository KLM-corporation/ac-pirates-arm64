import android.os.SystemClock;
import android.view.InputDevice;
import android.view.InputEvent;
import android.view.MotionEvent;

import java.lang.reflect.Method;

/**
 * Test helper run from adb with app_process (shell privileges): injects a tap with several fingers.
 *   adb shell CLASSPATH=/data/local/tmp/inj.dex app_process /system/bin Inj <x> <y> <spacing> [fingers] [holdMs] [repeat] [gapMs]
 */
public class Inj {
    private static Object manager;
    private static Method inject;

    private static void setup() throws Exception {
        Class<?> cls;
        try {
            cls = Class.forName("android.hardware.input.InputManagerGlobal");
        } catch (ClassNotFoundException e) {
            cls = Class.forName("android.hardware.input.InputManager");
        }
        manager = cls.getDeclaredMethod("getInstance").invoke(null);
        inject = manager.getClass().getMethod("injectInputEvent", InputEvent.class, int.class);
    }

    private static void send(long downTime, int action, int count, MotionEvent.PointerProperties[] props,
                             MotionEvent.PointerCoords[] coords) throws Exception {
        MotionEvent e = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), action, count, props, coords, 0, 0,
                1f, 1f, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        inject.invoke(manager, e, 0);
        e.recycle();
    }

    public static void main(String[] args) throws Exception {
        int x = Integer.parseInt(args[0]);
        int y = Integer.parseInt(args[1]);
        int spacing = Integer.parseInt(args[2]);
        int fingers = args.length > 3 ? Integer.parseInt(args[3]) : 4;
        long hold = args.length > 4 ? Long.parseLong(args[4]) : 300;
        setup();
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[fingers];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[fingers];
        for (int i = 0; i < fingers; i++) {
            props[i] = new MotionEvent.PointerProperties();
            props[i].id = i;
            props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords();
            coords[i].x = x + i * spacing;
            coords[i].y = y;
            coords[i].pressure = 1f;
            coords[i].size = 1f;
        }
        int repeat = args.length > 5 ? Integer.parseInt(args[5]) : 1;
        long gap = args.length > 6 ? Long.parseLong(args[6]) : 150;
        for (int rep = 0; rep < repeat; rep++) {
            long down = SystemClock.uptimeMillis();
            for (int n = 1; n <= fingers; n++) {
                MotionEvent.PointerProperties[] p = java.util.Arrays.copyOf(props, n);
                MotionEvent.PointerCoords[] c = java.util.Arrays.copyOf(coords, n);
                int action = n == 1 ? MotionEvent.ACTION_DOWN
                        : MotionEvent.ACTION_POINTER_DOWN | ((n - 1) << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
                send(down, action, n, p, c);
                Thread.sleep(15);
            }
            Thread.sleep(hold);
            for (int n = fingers; n >= 1; n--) {
                MotionEvent.PointerProperties[] p = java.util.Arrays.copyOf(props, n);
                MotionEvent.PointerCoords[] c = java.util.Arrays.copyOf(coords, n);
                int action = n == 1 ? MotionEvent.ACTION_UP
                        : MotionEvent.ACTION_POINTER_UP | ((n - 1) << MotionEvent.ACTION_POINTER_INDEX_SHIFT);
                send(down, action, n, p, c);
                Thread.sleep(15);
            }
            if (rep + 1 < repeat) Thread.sleep(gap);
        }
        System.out.println("injected " + fingers + "-finger tap at " + x + "," + y);
    }
}
