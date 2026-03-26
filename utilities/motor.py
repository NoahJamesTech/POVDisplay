import tkinter as tk
import dwfpy as dwf

# ===== Signal settings =====
FREQ = 50.0
MIN_DUTY = 5.0     # % duty for 0% throttle (your ESC/servo minimum)
MAX_DUTY = 10.0    # % duty for 100% throttle

# ===== Ramp settings =====
DEFAULT_RAMP_RATE = 10.0     # throttle-% per second
RAMP_INTERVAL_MS = 20        # update period (ms)


def duty_from_throttle(t: float) -> float:
    """Map 0–100 throttle to MIN_DUTY–MAX_DUTY (%)."""
    return MIN_DUTY + (MAX_DUTY - MIN_DUTY) * (t / 100.0)


def main():
    root = tk.Tk()
    root.title("Motor Throttle (0–100)")

    # Current (actual output) and target (user setpoint)
    current_var = tk.DoubleVar(value=0.0)
    target_var = tk.DoubleVar(value=0.0)
    ramp_rate_var = tk.StringVar(value=str(DEFAULT_RAMP_RATE))

    # Mutable state
    state = {
        "is_ramping": False,
        "ramp_after_id": None,
        "device": None,     # dwf.Device or None
        "ch": None,          # DIO-0 channel or None
    }

    start_btn_text = tk.StringVar(value="Start")

    # ===== Connection status bar =====
    conn_frame = tk.Frame(root)
    conn_frame.pack(padx=12, pady=(8, 0), fill="x")

    status_light = tk.Canvas(conn_frame, width=16, height=16, highlightthickness=0)
    status_light.pack(side="left")
    light_oval = status_light.create_oval(2, 2, 14, 14, fill="red", outline="darkred")

    status_label = tk.Label(conn_frame, text="  Disconnected", font=("Segoe UI", 10))
    status_label.pack(side="left")

    def update_status_light():
        if state["device"] is not None:
            status_light.itemconfig(light_oval, fill="lime", outline="darkgreen")
            status_label.config(text="  Connected")
            connect_btn.config(text="Disconnect")
        else:
            status_light.itemconfig(light_oval, fill="red", outline="darkred")
            status_label.config(text="  Disconnected")
            connect_btn.config(text="Connect")

    def do_connect():
        if state["device"] is not None:
            # Already connected — disconnect
            do_disconnect()
            return
        try:
            dev = dwf.Device()
            dev.open()
            state["device"] = dev
            state["ch"] = dev.digital_output[0]
            update_status_light()
            # Re-apply current output so the AD2 starts driving
            apply_output(float(current_var.get()))
        except Exception as e:
            state["device"] = None
            state["ch"] = None
            update_status_light()
            status_label.config(text=f"  Failed: {e}")

    def do_disconnect():
        cancel_ramp()
        current_var.set(0.0)
        try:
            if state["device"] is not None:
                state["device"].close()
        except Exception:
            pass
        state["device"] = None
        state["ch"] = None
        update_status_light()
        refresh_label(0.0)

    connect_btn = tk.Button(conn_frame, text="Connect", command=do_connect)
    connect_btn.pack(side="right")

    # ===== Info label =====
    label = tk.Label(root, font=("Segoe UI", 12), justify="left")
    label.pack(padx=12, pady=8, anchor="w")

    def get_ramp_rate() -> float:
        """Return ramp rate in throttle-%/s (>=0). Invalid input falls back to DEFAULT_RAMP_RATE."""
        try:
            r = float(ramp_rate_var.get().strip())
            if r < 0:
                r = 0.0
            return r
        except ValueError:
            return DEFAULT_RAMP_RATE

    def set_start_label():
        start_btn_text.set("Update" if float(current_var.get()) > 0.0 else "Start")

    def refresh_label(throttle: float):
        """Update the info label (does NOT touch hardware)."""
        duty = duty_from_throttle(throttle)
        period_ms = 1000.0 / FREQ
        high_ms = (duty / 100.0) * period_ms
        current = float(current_var.get())
        target = float(target_var.get())
        ramp = get_ramp_rate()
        label.config(
            text=(
                f"Current: {current:6.2f}%    Target: {target:6.2f}%\n"
                f"Duty:    {duty:5.2f}%    Pulse: {high_ms:0.3f} ms    Ramp: {ramp:g}%/s"
            )
        )
        set_start_label()

    def apply_output(throttle: float):
        """Drive DIO-0 with the mapped PWM (if connected) and refresh label."""
        duty = duty_from_throttle(throttle)
        ch = state["ch"]
        if ch is not None:
            try:
                ch.setup_clock(frequency=FREQ, duty_cycle=duty, start=True)
            except Exception:
                # Device lost — mark disconnected
                state["device"] = None
                state["ch"] = None
                update_status_light()
        refresh_label(throttle)

    def cancel_ramp():
        rid = state["ramp_after_id"]
        if rid is not None:
            root.after_cancel(rid)
        state["ramp_after_id"] = None
        state["is_ramping"] = False

    def estop():
        cancel_ramp()
        current_var.set(0.0)
        apply_output(0.0)

    def ramp_to(target: float):
        """Ramp current_var to target at ramp-rate; updates output each tick."""
        target = max(0.0, min(100.0, float(target)))
        cancel_ramp()

        rate = get_ramp_rate()
        if rate <= 0:
            current_var.set(target)
            apply_output(target)
            return

        state["is_ramping"] = True
        step = rate * (RAMP_INTERVAL_MS / 1000.0)

        def tick():
            cur = float(current_var.get())
            if abs(cur - target) <= step:
                current_var.set(target)
                apply_output(target)
                cancel_ramp()
                return

            direction = 1.0 if target > cur else -1.0
            nxt = cur + direction * step
            current_var.set(nxt)
            apply_output(nxt)
            state["ramp_after_id"] = root.after(RAMP_INTERVAL_MS, tick)

        tick()

    # ===== Commands =====
    def on_start_update():
        ramp_to(float(target_var.get()))

    def on_stop():
        ramp_to(0.0)

    def on_current_slider(_=None):
        cancel_ramp()
        t = max(0.0, min(100.0, float(current_var.get())))
        current_var.set(t)
        apply_output(t)

    def on_target_slider(_=None):
        t = max(0.0, min(100.0, float(target_var.get())))
        target_var.set(t)
        refresh_label(float(current_var.get()))

    def bump_current(delta: float):
        cancel_ramp()
        v = max(0.0, min(100.0, round(float(current_var.get()) + delta, 2)))
        current_var.set(v)
        apply_output(v)

    # ===== Sliders =====
    tk.Label(root, text="Current speed (act ual)").pack(padx=12, anchor="w")
    current_slider = tk.Scale(
        root,
        from_=0,
        to=100,
        resolution=0.1,
        digits=5,
        orient="horizontal",
        variable=current_var,
        command=on_current_slider,
        length=460,
    )
    current_slider.pack(padx=12, pady=(0, 8))
    # Override Scale's built-in arrow keys — run our handler, then "break" to suppress default
    def _slider_arrow(event):
        on_arrow(event)
        return "break"
    for key in ("<Left>", "<Right>", "<Up>", "<Down>",
                "<Shift-Left>", "<Shift-Right>", "<Shift-Up>", "<Shift-Down>",
                "<Control-Left>", "<Control-Right>", "<Control-Up>", "<Control-Down>"):
        current_slider.bind(key, _slider_arrow)
    # Alt+arrow on the slider must not be consumed by the bare-arrow binding above
    current_slider.bind("<Alt-Left>",  lambda e: (on_start_update(), "break")[1])
    current_slider.bind("<Alt-Down>",  lambda e: (on_stop(), "break")[1])
    current_slider.bind("<Alt-Right>", lambda e: (estop(), "break")[1])

    tk.Label(root, text="Target speed (setpoint)").pack(padx=12, anchor="w")
    target_slider = tk.Scale(
        root,
        from_=0,
        to=100,
        resolution=0.1,
        digits=5,
        orient="horizontal",
        variable=target_var,
        command=on_target_slider,
        length=460,
    )
    target_slider.pack(padx=12, pady=(0, 8))

    # ===== Ramp rate input =====
    ramp_frame = tk.Frame(root)
    ramp_frame.pack(padx=12, pady=(0, 8), fill="x")

    tk.Label(ramp_frame, text="Ramp (%/s):").pack(side="left")
    ramp_entry = tk.Entry(ramp_frame, textvariable=ramp_rate_var, width=8)
    ramp_entry.pack(side="left", padx=(6, 0))

    def normalize_ramp(_=None):
        r = get_ramp_rate()
        ramp_rate_var.set(f"{r:g}")
        refresh_label(float(current_var.get()))

    ramp_entry.bind("<Return>", normalize_ramp)
    ramp_entry.bind("<FocusOut>", normalize_ramp)

    # ===== Buttons =====
    btns = tk.Frame(root)
    btns.pack(padx=12, pady=(0, 10), fill="x")

    tk.Button(btns, textvariable=start_btn_text, command=on_start_update).pack(
        side="left", expand=True, fill="x", padx=(0, 6)
    )
    tk.Button(btns, text="Stop", command=on_stop).pack(
        side="left", expand=True, fill="x", padx=(6, 6)
    )
    tk.Button(btns, text="E-Stop", command=estop).pack(
        side="left", expand=True, fill="x", padx=(6, 0)
    )

    # ===== Hotkeys =====
    # Arrow keys: 1% steps | Shift+Arrow: 5% steps | Ctrl+Arrow: 0.25% steps
    def on_arrow(event):
        direction = 1.0 if event.keysym in ("Right", "Up") else -1.0
        if event.state & 0x4:      # Ctrl held
            delta = 0.1
        elif event.state & 0x1:    # Shift held
            delta = 5.0
        else:
            delta = 1.0
        bump_current(direction * delta)
        return "break"

    for key in ("<Left>", "<Right>", "<Up>", "<Down>",
                "<Shift-Left>", "<Shift-Right>", "<Shift-Up>", "<Shift-Down>",
                "<Control-Left>", "<Control-Right>", "<Control-Up>", "<Control-Down>"):
        root.bind(key, on_arrow)

    # Alt hotkeys
    root.bind_all("<Alt-Left>",  lambda e: on_start_update())
    root.bind_all("<Alt-Down>",  lambda e: on_stop())
    root.bind_all("<Alt-Right>", lambda e: estop())

    # Start with label showing 0% (no hardware output until connected)
    refresh_label(0.0)
    current_slider.focus_set()

    # Clean up device on window close
    def on_close():
        if state["device"] is not None:
            try:
                state["device"].close()
            except Exception:
                pass
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.mainloop()


if __name__ == "__main__":
    main()
