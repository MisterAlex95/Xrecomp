package fr.adserver.xboxrecomp;

/**
 * The one overlay pad (input_overlay.h, port 0) both touch surfaces drive: the companion panel's Back / Start / Black
 * / White on the second screen and the on-screen pad (XBA-42). Each keeps its own state here; the merge (buttons
 * or'ed, each stick and trigger from whichever is pushed further) goes to the sink, and the overlay is off only when
 * neither is on. UI thread only.
 */
final class OverlayMixer {
    /** Where the merged pad goes: the native overlay in the game, a fake in the tests. */
    interface Sink {
        void pad(int buttons, int analogMask, int lt, int rt, int lx, int ly, int rx, int ry);

        void off();
    }

    private final Sink sink;
    private boolean panelOn, touchOn;
    private int panelButtons, panelAnalog;
    private TouchPad.State touch = new TouchPad.State();

    OverlayMixer(Sink sink) {
        this.sink = sink;
    }

    /** The panel's buttons (wButtons bits, analog mask: bit i = bAnalogButtons[i] at 255, triggers included). */
    void panel(int buttons, int analogMask) {
        panelOn = true;
        panelButtons = buttons;
        panelAnalog = analogMask;
        push();
    }

    void panelOff() {
        panelOn = false;
        panelButtons = panelAnalog = 0;
        push();
    }

    void touch(TouchPad.State s) {
        touchOn = true;
        touch = s;
        push();
    }

    void touchOff() {
        touchOn = false;
        touch = new TouchPad.State();
        push();
    }

    /** Everything released and off (onStop, quit). */
    void off() {
        panelOn = touchOn = false;
        panelButtons = panelAnalog = 0;
        touch = new TouchPad.State();
        sink.off();
    }

    boolean on() {
        return panelOn || touchOn;
    }

    private void push() {
        if (!panelOn && !touchOn) {
            sink.off();
            return;
        }
        TouchPad.State t = touch;
        int lt = Math.max(t.lt, (panelAnalog & 1 << TouchPad.AN_LT) != 0 ? 255 : 0);
        int rt = Math.max(t.rt, (panelAnalog & 1 << TouchPad.AN_RT) != 0 ? 255 : 0);
        sink.pad(panelButtons | t.buttons, (panelAnalog | t.analogMask) & 0x3F, lt, rt, t.lx, t.ly, t.rx, t.ry);
    }
}
