"""Fake wearables for testing the dashboard. Run in a second terminal:  python sim.py
Beds are marked sim=True so the dashboard shows a SIM tag. Change URL when the ESP32 is on another machine."""
import json, math, random, time, urllib.request

URL = "http://localhost:5000/data"
BEDS = [(1, "Bed 1 (sim)"), (2, "Bed 2 (sim)"), (3, "Bed 3 (sim)"), (4, "Bed 4 (sim)")]
t0 = time.time()


def post(d):
    req = urllib.request.Request(URL, json.dumps(d).encode(), {"Content-Type": "application/json"})
    try: urllib.request.urlopen(req, timeout=2).read()
    except Exception as e: print("post failed:", e)


while True:
    t = time.time() - t0
    for bed, name in BEDS:
        if bed == 4 and 60 < t < 100: continue                       # bed 4 goes offline for 40 s
        stress = 0.0
        if bed == 3 and t > 40: stress = min(1, (t - 40) / 25)      # bed 3 ramps up after 40 s
        if bed == 2 and 70 < t < 90: stress = 0.5                    # bed 2 has a mild bump
        d = dict(bed=bed, name=name, sim=True,
                 hr=72 + 20 * stress + random.gauss(0, 1.5),
                 gsr=1200 * (1 + 0.7 * stress) + random.gauss(0, 15),
                 temp=33.5 - 0.8 * stress + random.gauss(0, .05),
                 motion=abs(random.gauss(0.1, 0.05)) + (0.8 if bed == 1 and 50 < t < 56 else 0),
                 batt=max(5, 95 - int(t / 30) - bed), contact=True)
        if bed == 3 and 75 < t < 76: d["call"] = True               # one nurse call
        if bed == 2 and 80 < t < 81: d["report"] = 4                # one self-report
        post(d)
    time.sleep(1)
