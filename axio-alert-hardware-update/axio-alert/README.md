# Axio Alert

A low-cost wrist wearable prototype that tracks signs of patient stress (pre-op, chemo) and prompts a nurse to check in. It also gives the patient a vibration breathing guide.

> **Prototype for a hackathon. Not a medical device. Not a diagnosis.** The score is an unvalidated heuristic and "Check in suggested" is a prompt, not a finding.

## What it does
- **Wearable (ESP32-C3):** reads heart rate (MAX30102), skin conductance (GSR), skin temperature (DS18B20) and motion (MPU6050), and sends them to the ward dashboard once a second over Wi-Fi.
- **Button:** N short presses = self-report N (1 to 5). Hold 2 s = nurse call.
- **Breathing guide:** coin motor ramps up for 4 s and fades for 6 s (about 6 breaths/min). The dashboard starts it when a patient reaches "Check in suggested".
- **Ward dashboard (Flask):** ranks beds by score, shows a nurse-call banner and call log with response times, device status, patient detail, self-reports, CSV export. The score is relative to each patient's own baseline and pauses while the patient is moving.

## Status (tick as you verify on real hardware)
- [ ] Bench test passes on hardware (`firmware/bench_test`)
- [ ] Demo firmware uploads and posts to the dashboard
- [ ] Boards dry-fitted in the printed case (`hardware/fit_model.html` is a visual aid only until this is done)
- [ ] Heart rate roughly matches a manual pulse count
- [ ] Nurse call and self-report appear on the dashboard
- [ ] Breathing guide runs from the dashboard trigger
- [x] Dashboard logic tested with simulated beds (`dashboard/sim.py`)
- [ ] Accuracy on WESAD: **not done yet**. No accuracy number is claimed.

Known limits: the heart-rate method is crude and motion-sensitive, the score weights are guesses, and only one real device has been used (other beds are simulated and tagged SIM).

## Hardware
ESP32-C3 SuperMini, MAX30102, GSR sensor V2.0, DS18B20, MPU6050, coin motor (2N2222 driver), 400 mAh LiPo + TP4056, button, LED.
Pins: SDA GPIO6, SCL GPIO7, GSR GPIO3, DS18B20 GPIO4, motor PWM GPIO5, button GPIO10, LED GPIO1.

## Run it
1. **Dashboard:** `cd dashboard && pip install -r requirements.txt && python app.py`, then open http://localhost:5000. No hardware yet? Run `python sim.py` in a second terminal.
2. **Firmware:** in `firmware/axio_alert_demo`, copy `secrets.h.example` to `secrets.h` and fill in your 2.4 GHz Wi-Fi and your laptop's IP. In Arduino IDE choose ESP32C3 Dev Module (core 3.x), enable USB CDC On Boot, install OneWire and DallasTemperature, upload.
3. Keep the laptop and the wearable on the same network (a phone hotspot works) and allow Python through the firewall.

## 3D fit model and wiring
- `hardware/fit_model.html`: interactive 3D model of the enclosure (cavity 66 x 46 x 22 mm) with the boards and the wire runs from the wiring diagram. Open it in a browser (it needs internet to load the 3D library). Board pin positions and wire routes are approximations that have not yet been checked against a physical dry-fit.
- `hardware/wiring_diagram_A4.pdf`: prototype wiring diagram with every wire numbered W1 to W32.

## Layout
`firmware/bench_test` tests each part from the Serial Monitor. `firmware/axio_alert_demo` is the demo firmware. `dashboard` is the Flask app and simulator. `hardware` holds the 3D fit model and the wiring diagram.
