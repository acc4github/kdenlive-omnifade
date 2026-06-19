# OmniFade - Frei0r Transition Plugin for Kdenlive

A versatile cross‑fade transition for Kdenlive.   

<p align="center">
<img width="416" height="351" alt="kdenlive_kQYBd47fkE" src="https://github.com/user-attachments/assets/3f978253-f92d-413e-b2a1-a63cd848deb8" />
</p>

## Features

- **Fade Progress (`position`)** – standard transition progress, animated from 0 to 1.
- **Speed Curve (%)** – applies a power‑law easing curve to the fade timing. Higher values create stronger acceleration/deceleration.
- **Gentle Arrival (%)** – adds a smooth slowdown at the end of the transition.  
  - When `Speed Curve` is 0%, it acts as a reverse ease‑out (fast start, slow end).  
  - When `Speed Curve` is used, it creates a deceleration zone near the end – the fade eases into the final frame.
- **Independent Zoom per Clip** – choose separate behaviours for the outgoing and incoming clips:
  - **Expand** – clip grows larger over the transition.
  - **Static** – no zoom (scale = 1).
  - **Shrink** – clip shrinks over the transition.
  - **Zoom Strength (%)** – controls how intense the zoom effect is.
- **Blur Strength** – radial blur that automatically adjusts its amount based on speed.
- **Fill Background (average)** – fills the empty background with the average colour of each clip.
- **Invert** – swaps the incoming and outgoing clips.

## Demo

https://github.com/user-attachments/assets/8c42bd22-e770-4aee-8b01-798c84a0f23c

## Installation (Windows)

1. Build or obtain the `omni-fade.dll` and `omni-fade.xml` files.
2. Place the `omni-fade.dll` in Kdenlive's frei0r plugins folder (e.g., `kdenlive-master\lib\frei0r-1`).
3. Place the `omni-fade.xml` in Kdenlive's transitions folder (e.g., `kdenlive-master\bin\data\kdenlive\transitions`).
4. Restart Kdenlive. The transition appears under Transitions.

## License

This project is licensed under the **GNU General Public License v3.0** (GPL-3.0).  
See the LICENSE file for full details.

## Credits

Developed for the open‑source video editing community.  
Copyright © 2026 acc4commissions

AI Assistant: Grok (4.3)
