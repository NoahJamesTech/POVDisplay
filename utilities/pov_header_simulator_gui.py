#!/usr/bin/env python3
"""
POV header simulator GUI

Desktop GUI version of the POV header simulator.
- Paste header text directly into the app
- Edit all relevant settings in text inputs
- Live preview updates automatically
- Save the current rendered image to PNG

Requirements:
    pip install pillow numpy

Run:
    python pov_header_simulator_gui.py
"""

from __future__ import annotations

import math
import re
import tkinter as tk
from dataclasses import dataclass
from pathlib import Path
from tkinter import filedialog, ttk
from tkinter.scrolledtext import ScrolledText
from typing import Optional, Sequence, Tuple

import numpy as np
from PIL import Image, ImageDraw, ImageTk


@dataclass
class ParsedPOV:
    array_name: str
    columns: int
    blade_leds: int
    data: np.ndarray  # shape: (columns, blade_leds, 3)


ARRAY_DECL_RE = re.compile(
    r"(?P<type>\w+(?:\s+\w+)*)\s+"
    r"(?P<name>\w+)\s*"
    r"\[(?P<d1>[^\]]+)\]\s*"
    r"\[(?P<d2>[^\]]+)\]\s*"
    r"\[(?P<d3>[^\]]+)\]\s*=\s*\{",
    re.MULTILINE,
)


def strip_comments(text: str) -> str:
    text = re.sub(r"//.*?$", "", text, flags=re.MULTILINE)
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    return text


# This intentionally mirrors the original program's behavior.
def parse_int_like(expr: str, defines: dict[str, int]) -> Optional[int]:
    expr = expr.strip()
    expr = re.sub(r"\(([^()]*)\)", r"\1", expr)
    if expr in defines:
        return defines[expr]
    if re.fullmatch(r"0x[0-9a-fA-F]+", expr):
        return int(expr, 16)
    if re.fullmatch(r"\d+", expr):
        return int(expr)
    replaced = expr
    for name, value in sorted(defines.items(), key=lambda kv: -len(kv[0])):
        replaced = re.sub(rf"\b{re.escape(name)}\b", str(value), replaced)
    if re.fullmatch(r"[0-9\s+\-*/%<>|&()]+", replaced):
        try:
            return int(eval(replaced, {"__builtins__": {}}, {}))
        except Exception:
            return None
    return None


def parse_defines(text: str) -> dict[str, int]:
    defines: dict[str, int] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("#define "):
            continue
        parts = line.split(None, 2)
        if len(parts) < 3:
            continue
        _, name, value = parts
        if "(" in name and name.endswith(")"):
            continue
        parsed = parse_int_like(value.strip(), defines)
        if parsed is not None:
            defines[name] = parsed
    return defines


def find_matching_brace(text: str, open_index: int) -> int:
    depth = 0
    for idx in range(open_index, len(text)):
        ch = text[idx]
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                return idx
    raise ValueError("Could not find matching closing brace for array initializer.")


def extract_array_block(text: str, array_name: Optional[str]) -> Tuple[str, str, str, str, str]:
    matches = list(ARRAY_DECL_RE.finditer(text))
    if not matches:
        raise ValueError("No 3D array initializer found in pasted header text.")

    chosen = None
    if array_name:
        for match in matches:
            if match.group("name") == array_name:
                chosen = match
                break
        if chosen is None:
            found = ", ".join(m.group("name") for m in matches)
            raise ValueError(f"Array '{array_name}' not found. Found: {found}")
    else:
        for match in matches:
            if "pov" in match.group("name").lower():
                chosen = match
                break
        if chosen is None:
            chosen = matches[0]

    brace_open = text.find("{", chosen.end() - 1)
    brace_close = find_matching_brace(text, brace_open)
    initializer = text[brace_open : brace_close + 1]

    return (
        chosen.group("name"),
        chosen.group("d1").strip(),
        chosen.group("d2").strip(),
        chosen.group("d3").strip(),
        initializer,
    )


def parse_pov_text(
    header_text: str,
    array_name: Optional[str] = None,
    columns_override: Optional[int] = None,
    blade_leds_override: Optional[int] = None,
) -> ParsedPOV:
    clean_text = strip_comments(header_text)
    defines = parse_defines(clean_text)

    found_name, d1, d2, d3, initializer = extract_array_block(clean_text, array_name)

    columns = columns_override or parse_int_like(d1, defines)
    blade_leds = blade_leds_override or parse_int_like(d2, defines)
    channels = parse_int_like(d3, defines)

    if channels is None:
        raise ValueError(f"Could not resolve third array dimension '{d3}'.")
    if channels != 3:
        raise ValueError(f"Expected RGB array with third dimension 3, got {channels}.")

    numbers = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", initializer)]

    if blade_leds is None and columns is None:
        for guess_columns in (720, 360):
            if len(numbers) % (guess_columns * 3) == 0:
                blade_leds = len(numbers) // (guess_columns * 3)
                columns = guess_columns
                break

    if blade_leds is None and columns is not None:
        if len(numbers) % (columns * 3) != 0:
            raise ValueError("Could not infer BLADE_LEDS cleanly from initializer length.")
        blade_leds = len(numbers) // (columns * 3)

    if columns is None and blade_leds is not None:
        if len(numbers) % (blade_leds * 3) != 0:
            raise ValueError("Could not infer column count cleanly from initializer length.")
        columns = len(numbers) // (blade_leds * 3)

    if columns is None or blade_leds is None:
        raise ValueError("Could not determine array dimensions. Fill in columns and/or blade LEDs.")

    expected = columns * blade_leds * 3
    if len(numbers) != expected:
        raise ValueError(
            f"Initializer length mismatch: found {len(numbers)} RGB values, expected {expected} "
            f"for shape ({columns}, {blade_leds}, 3)."
        )

    arr = np.array(numbers, dtype=np.uint8).reshape(columns, blade_leds, 3)
    return ParsedPOV(array_name=found_name, columns=columns, blade_leds=blade_leds, data=arr)


def compute_column(elapsed_us: int, rotation_period_us: int, total_columns: int) -> int:
    position_in_rotation = elapsed_us % rotation_period_us
    col = (position_in_rotation * total_columns) // rotation_period_us
    if col >= total_columns:
        col = total_columns - 1
    return int(col)


def render_unwrapped(parsed: ParsedPOV, led_scale: int = 6) -> Image.Image:
    arr = parsed.data[:, ::-1, :]
    img = Image.fromarray(arr.astype(np.uint8), mode="RGB")
    img = img.resize((parsed.columns, parsed.blade_leds * led_scale), Image.Resampling.NEAREST)
    return img


def draw_led(draw: ImageDraw.ImageDraw, x: float, y: float, radius: float, color: Tuple[int, int, int]) -> None:
    if radius <= 0.5:
        draw.point((x, y), fill=color)
        return
    draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)


def render_disk(
    parsed: ParsedPOV,
    rotation_period_us: int,
    size: int = 1200,
    samples: Optional[int] = None,
    led_radius: float = 2.0,
    center_gap: float = 20.0,
    outer_margin: float = 20.0,
    reverse_led_order: bool = False,
    background: Tuple[int, int, int] = (0, 0, 0),
) -> Image.Image:
    columns = parsed.columns
    blade_leds = parsed.blade_leds
    pov_image = parsed.data

    if samples is None:
        samples = max(columns * 2, 1440)

    img = Image.new("RGB", (size, size), background)
    draw = ImageDraw.Draw(img)

    cx = size / 2.0
    cy = size / 2.0
    max_radius = (size / 2.0) - outer_margin
    usable_radius = max_radius - center_gap

    if usable_radius <= 0:
        raise ValueError("center gap / outer margin leave no room for LEDs.")

    for sample_idx in range(samples):
        elapsed_us = int(sample_idx * rotation_period_us / samples)
        col_a = compute_column(elapsed_us, rotation_period_us, columns)
        col_b = (col_a + (columns // 2)) % columns

        angle_deg = (sample_idx * 360.0) / samples
        angle_a = math.radians(angle_deg - 90.0)
        angle_b = angle_a + math.pi

        for i in range(blade_leds):
            logical_i = blade_leds - 1 - i if reverse_led_order else i
            radius = center_gap + ((i + 0.5) / blade_leds) * usable_radius

            color_a = tuple(int(c) for c in pov_image[col_a, logical_i])
            color_b = tuple(int(c) for c in pov_image[col_b, logical_i])

            xa = cx + radius * math.cos(angle_a)
            ya = cy + radius * math.sin(angle_a)
            xb = cx + radius * math.cos(angle_b)
            yb = cy + radius * math.sin(angle_b)

            draw_led(draw, xa, ya, led_radius, color_a)
            draw_led(draw, xb, yb, led_radius, color_b)

    return img


def stack_images(images: Sequence[Image.Image], gap: int = 16, background=(20, 20, 20)) -> Image.Image:
    widths = [im.width for im in images]
    heights = [im.height for im in images]
    out = Image.new("RGB", (max(widths), sum(heights) + gap * (len(images) - 1)), background)

    y = 0
    for image in images:
        x = (out.width - image.width) // 2
        out.paste(image, (x, y))
        y += image.height + gap
    return out


class POVSimulatorGUI(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("POV Header Simulator")
        self.geometry("1500x900")
        self.minsize(1200, 700)

        self.preview_photo: Optional[ImageTk.PhotoImage] = None
        self.last_rendered_image: Optional[Image.Image] = None
        self.redraw_job: Optional[str] = None

        self.var_array_name = tk.StringVar(value="pov_image")
        self.var_columns = tk.StringVar(value="720")
        self.var_blade_leds = tk.StringVar(value="")
        self.var_period_define = tk.StringVar(value="ROTATION_PERIOD_US")
        self.var_period_us = tk.StringVar(value="")
        self.var_mode = tk.StringVar(value="both")
        self.var_size = tk.StringVar(value="1000")
        self.var_samples = tk.StringVar(value="")
        self.var_led_radius = tk.StringVar(value="2.0")
        self.var_center_gap = tk.StringVar(value="20")
        self.var_outer_margin = tk.StringVar(value="20")
        self.var_unwrapped_scale = tk.StringVar(value="6")
        self.var_reverse_led_order = tk.BooleanVar(value=False)
        self.var_live_update = tk.BooleanVar(value=True)

        self._build_ui()
        self._bind_live_update_events()
        self.after(50, self.schedule_redraw)

    def _build_ui(self) -> None:
        self.columnconfigure(0, weight=0)
        self.columnconfigure(1, weight=1)
        self.rowconfigure(0, weight=1)

        left = ttk.Frame(self, padding=10)
        left.grid(row=0, column=0, sticky="nsew")
        left.rowconfigure(2, weight=1)

        right = ttk.Frame(self, padding=(0, 10, 10, 10))
        right.grid(row=0, column=1, sticky="nsew")
        right.rowconfigure(1, weight=1)
        right.columnconfigure(0, weight=1)

        settings = ttk.LabelFrame(left, text="Inputs", padding=10)
        settings.grid(row=0, column=0, sticky="new")
        settings.columnconfigure(1, weight=1)

        row = 0
        self._add_labeled_entry(settings, row, "Array name", self.var_array_name)
        row += 1
        self._add_labeled_entry(settings, row, "Columns", self.var_columns)
        row += 1
        self._add_labeled_entry(settings, row, "Blade LEDs", self.var_blade_leds)
        row += 1
        self._add_labeled_entry(settings, row, "Period define", self.var_period_define)
        row += 1
        self._add_labeled_entry(settings, row, "Period us", self.var_period_us)
        row += 1
        self._add_labeled_entry(settings, row, "Preview size", self.var_size)
        row += 1
        self._add_labeled_entry(settings, row, "Samples", self.var_samples)
        row += 1
        self._add_labeled_entry(settings, row, "LED radius", self.var_led_radius)
        row += 1
        self._add_labeled_entry(settings, row, "Center gap", self.var_center_gap)
        row += 1
        self._add_labeled_entry(settings, row, "Outer margin", self.var_outer_margin)
        row += 1
        self._add_labeled_entry(settings, row, "Unwrapped scale", self.var_unwrapped_scale)
        row += 1

        ttk.Label(settings, text="Mode").grid(row=row, column=0, sticky="w", pady=4)
        mode_combo = ttk.Combobox(settings, textvariable=self.var_mode, values=["disk", "unwrapped", "both"], state="readonly", width=18)
        mode_combo.grid(row=row, column=1, sticky="ew", pady=4)
        row += 1

        ttk.Checkbutton(settings, text="Reverse LED order", variable=self.var_reverse_led_order, command=self.schedule_redraw).grid(row=row, column=0, columnspan=2, sticky="w", pady=4)
        row += 1
        ttk.Checkbutton(settings, text="Live update", variable=self.var_live_update).grid(row=row, column=0, columnspan=2, sticky="w", pady=4)
        row += 1

        button_row = ttk.Frame(settings)
        button_row.grid(row=row, column=0, columnspan=2, sticky="ew", pady=(8, 0))
        button_row.columnconfigure(0, weight=1)
        button_row.columnconfigure(1, weight=1)
        button_row.columnconfigure(2, weight=1)
        ttk.Button(button_row, text="Render Now", command=self.redraw_preview).grid(row=0, column=0, sticky="ew", padx=(0, 4))
        ttk.Button(button_row, text="Load .h File", command=self.load_header_file).grid(row=0, column=1, sticky="ew", padx=4)
        ttk.Button(button_row, text="Save PNG", command=self.save_png).grid(row=0, column=2, sticky="ew", padx=(4, 0))

        header_frame = ttk.LabelFrame(left, text="Header text (paste here)", padding=8)
        header_frame.grid(row=2, column=0, sticky="nsew", pady=(10, 0))
        header_frame.rowconfigure(0, weight=1)
        header_frame.columnconfigure(0, weight=1)
        self.header_text = ScrolledText(header_frame, wrap=tk.NONE, font=("Consolas", 10), undo=True, width=70)
        self.header_text.grid(row=0, column=0, sticky="nsew")

        status_frame = ttk.LabelFrame(right, text="Status", padding=8)
        status_frame.grid(row=0, column=0, sticky="ew")
        status_frame.columnconfigure(0, weight=1)
        self.status_var = tk.StringVar(value="Paste a header and the preview will render here.")
        self.meta_var = tk.StringVar(value="")
        ttk.Label(status_frame, textvariable=self.status_var, foreground="#0a5").grid(row=0, column=0, sticky="w")
        ttk.Label(status_frame, textvariable=self.meta_var).grid(row=1, column=0, sticky="w", pady=(4, 0))

        preview_frame = ttk.LabelFrame(right, text="Live output", padding=8)
        preview_frame.grid(row=1, column=0, sticky="nsew", pady=(10, 0))
        preview_frame.rowconfigure(0, weight=1)
        preview_frame.columnconfigure(0, weight=1)

        self.preview_canvas = tk.Canvas(preview_frame, bg="#111111", highlightthickness=0)
        self.preview_canvas.grid(row=0, column=0, sticky="nsew")
        self.preview_canvas.bind("<Configure>", lambda _e: self.schedule_redraw())

    def _add_labeled_entry(self, parent: ttk.Frame, row: int, label: str, variable: tk.StringVar) -> ttk.Entry:
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=4)
        entry = ttk.Entry(parent, textvariable=variable)
        entry.grid(row=row, column=1, sticky="ew", pady=4)
        return entry

    def _bind_live_update_events(self) -> None:
        watched_vars = [
            self.var_array_name,
            self.var_columns,
            self.var_blade_leds,
            self.var_period_define,
            self.var_period_us,
            self.var_mode,
            self.var_size,
            self.var_samples,
            self.var_led_radius,
            self.var_center_gap,
            self.var_outer_margin,
            self.var_unwrapped_scale,
        ]
        for var in watched_vars:
            var.trace_add("write", self._on_input_change)
        self.header_text.bind("<<Modified>>", self._on_header_modified)

    def _on_input_change(self, *_args) -> None:
        if self.var_live_update.get():
            self.schedule_redraw()

    def _on_header_modified(self, _event=None) -> None:
        self.header_text.edit_modified(False)
        if self.var_live_update.get():
            self.schedule_redraw()

    def schedule_redraw(self) -> None:
        if not self.var_live_update.get():
            return
        if self.redraw_job is not None:
            self.after_cancel(self.redraw_job)
        self.redraw_job = self.after(350, self.redraw_preview)

    def _parse_optional_int(self, value: str) -> Optional[int]:
        value = value.strip()
        return int(value) if value else None

    def _parse_float(self, value: str, field_name: str) -> float:
        value = value.strip()
        if not value:
            raise ValueError(f"{field_name} is required.")
        return float(value)

    def _build_image(self) -> Image.Image:
        header_text = self.header_text.get("1.0", tk.END)
        if not header_text.strip():
            raise ValueError("Paste header text into the editor first.")

        array_name = self.var_array_name.get().strip() or None
        columns = self._parse_optional_int(self.var_columns.get())
        blade_leds = self._parse_optional_int(self.var_blade_leds.get())
        period_us = self._parse_optional_int(self.var_period_us.get())
        size = self._parse_optional_int(self.var_size.get()) or 1000
        samples = self._parse_optional_int(self.var_samples.get())
        unwrapped_scale = self._parse_optional_int(self.var_unwrapped_scale.get()) or 6
        led_radius = self._parse_float(self.var_led_radius.get(), "LED radius")
        center_gap = self._parse_float(self.var_center_gap.get(), "Center gap")
        outer_margin = self._parse_float(self.var_outer_margin.get(), "Outer margin")
        mode = self.var_mode.get().strip() or "both"

        clean_text = strip_comments(header_text)
        defines = parse_defines(clean_text)

        parsed = parse_pov_text(
            header_text=header_text,
            array_name=array_name,
            columns_override=columns,
            blade_leds_override=blade_leds,
        )

        if period_us is None:
            period_define_name = self.var_period_define.get().strip() or "ROTATION_PERIOD_US"
            period_us = defines.get(period_define_name)

        if mode in {"disk", "both"} and period_us is None:
            raise ValueError("Rotation period not found. Fill in Period us or include the define in the header text.")

        images = []
        if mode in {"unwrapped", "both"}:
            images.append(render_unwrapped(parsed, led_scale=unwrapped_scale))
        if mode in {"disk", "both"}:
            images.append(
                render_disk(
                    parsed=parsed,
                    rotation_period_us=period_us,
                    size=size,
                    samples=samples,
                    led_radius=led_radius,
                    center_gap=center_gap,
                    outer_margin=outer_margin,
                    reverse_led_order=self.var_reverse_led_order.get(),
                    background=(0, 0, 0),
                )
            )

        final_image = images[0] if len(images) == 1 else stack_images(images)
        self.meta_var.set(
            f"Parsed array={parsed.array_name} | columns={parsed.columns} | blade_leds={parsed.blade_leds}"
            + (f" | period_us={period_us}" if period_us is not None else "")
        )
        return final_image

    def redraw_preview(self) -> None:
        self.redraw_job = None
        try:
            image = self._build_image()
            self.last_rendered_image = image
            self._show_image_on_canvas(image)
            self.status_var.set("Preview updated.")
        except Exception as exc:
            self.last_rendered_image = None
            self.preview_canvas.delete("all")
            self.preview_canvas.create_text(
                20,
                20,
                anchor="nw",
                text=str(exc),
                fill="#ff7777",
                width=max(200, self.preview_canvas.winfo_width() - 40),
                font=("Segoe UI", 11),
            )
            self.status_var.set("Render failed.")
            self.meta_var.set("")

    def _show_image_on_canvas(self, image: Image.Image) -> None:
        canvas_w = max(100, self.preview_canvas.winfo_width())
        canvas_h = max(100, self.preview_canvas.winfo_height())

        img = image.copy()
        img.thumbnail((canvas_w - 20, canvas_h - 20), Image.Resampling.LANCZOS)

        self.preview_photo = ImageTk.PhotoImage(img)
        self.preview_canvas.delete("all")
        self.preview_canvas.create_image(canvas_w // 2, canvas_h // 2, image=self.preview_photo, anchor="center")

    def load_header_file(self) -> None:
        file_path = filedialog.askopenfilename(
            title="Open header file",
            filetypes=[("Header files", "*.h *.hpp *.c *.cpp *.txt"), ("All files", "*.*")],
        )
        if not file_path:
            return
        
        path_obj = Path(file_path)
        text = path_obj.read_text(encoding="utf-8", errors="ignore")
        self.header_text.delete("1.0", tk.END)
        self.header_text.insert("1.0", text)
        
        # Switch the array name to the name of the file
        self.var_array_name.set(path_obj.stem)
        
        self.schedule_redraw()

    def save_png(self) -> None:
        if self.last_rendered_image is None:
            try:
                self.last_rendered_image = self._build_image()
            except Exception as exc:
                self.status_var.set(f"Cannot save: {exc}")
                return

        save_path = filedialog.asksaveasfilename(
            title="Save PNG",
            defaultextension=".png",
            filetypes=[("PNG image", "*.png")],
        )
        if not save_path:
            return

        self.last_rendered_image.save(save_path)
        self.status_var.set(f"Saved PNG: {save_path}")


def main() -> None:
    app = POVSimulatorGUI()
    app.mainloop()


if __name__ == "__main__":
    main()
