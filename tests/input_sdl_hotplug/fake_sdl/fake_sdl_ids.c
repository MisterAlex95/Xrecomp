/* Weak defaults for the fake SDL's device identity (SDL.h): every controller
 * the same model. tests/input_pad_ports defines its own. */
#include <SDL.h>

__attribute__((weak)) Uint16 SDL_JoystickGetDeviceVendor(int device_index)
{
    (void)device_index;
    return 0x045e;
}

__attribute__((weak)) Uint16 SDL_JoystickGetDeviceProduct(int device_index)
{
    (void)device_index;
    return 0x0289;
}

__attribute__((weak)) const char *SDL_JoystickNameForIndex(int device_index)
{
    (void)device_index;
    return "Fake controller";
}
