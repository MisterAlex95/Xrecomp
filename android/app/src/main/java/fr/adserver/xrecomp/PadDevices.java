package fr.adserver.xrecomp;

import android.content.Context;
import android.hardware.input.InputManager;
import android.os.Build;
import android.view.InputDevice;

import java.util.ArrayList;
import java.util.List;

/**
 * The game controllers Android sees now (XBA-123 ports, XBA-42 touch pad): what SDL opens as a game controller, a
 * device with gamepad or joystick sources that is not virtual, once per physical device (a pad can show up as several
 * input devices sharing a descriptor). The handheld's own pad is the one that is not external (API 29+).
 */
final class PadDevices {
    private PadDevices() {}

    static boolean isGamepad(InputDevice d) {
        if (d == null || d.isVirtual())
            return false;
        int s = d.getSources();
        return (s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
               || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    /** Tests: the controllers to report instead of InputManager's (null: the real ones). */
    static List<PadPorts.Device> forTest;

    static List<PadPorts.Device> connected(Context c) {
        if (forTest != null)
            return new ArrayList<>(forTest);
        List<PadPorts.Device> out = new ArrayList<>();
        List<String> descriptors = new ArrayList<>();
        InputManager im = (InputManager)c.getSystemService(Context.INPUT_SERVICE);
        int[] ids = im != null ? im.getInputDeviceIds() : InputDevice.getDeviceIds();
        for (int id : ids) {
            InputDevice d = im != null ? im.getInputDevice(id) : InputDevice.getDevice(id);
            if (!isGamepad(d))
                continue;
            String desc = d.getDescriptor();
            if (desc != null && descriptors.contains(desc))
                continue;
            descriptors.add(desc);
            boolean builtin = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q && !d.isExternal();
            out.add(new PadPorts.Device(PadPorts.key(d.getVendorId(), d.getProductId()), d.getName(), builtin));
        }
        return out;
    }

    static boolean any(Context c) {
        return !connected(c).isEmpty();
    }
}
