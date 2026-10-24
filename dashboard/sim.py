import json, random, sys, time, urllib.request

URL = "http://127.0.0.1:5000/data"      # 127.0.0.1 avoids slow 'localhost' lookups on some Windows PCs
STEP = 0.1 if "--fast" in sys.argv else 1.0
BEDS = [(1, "Bed 1 (sim)"), (2, "Bed 2 (sim)"), (3, "Bed 3 (sim)"), (4, "Bed 4 (sim)")]


def post(d):
    req = urllib.request.Request(URL, json.dumps(d).encode(), {"Content-Type": "application/json"})
    try:
        urllib.request.urlopen(req, timeout=2).read()
        return True
    except Exception as e:
        print("post failed (is app.py running?):", e)
        return False


sent, n, nxt = set(), 0, time.time()
while True:
    for bed, name in BEDS:
        if bed == 4 and 60 <= n < 100:
            continue                                                   # Bed 4 offline
        stress = min(1, (n - 40) / 25) if bed == 3 and n > 40 else (0.5 if bed == 2 and 70 <= n < 90 else 0.0)
        d = dict(bed=bed, name=name, sim=True,
                 hr=72 + 20 * stress + random.gauss(0, 1.5),
                 gsr=1200 * (1 + 0.7 * stress) + random.gauss(0, 15),
                 temp=33.5 - 0.8 * stress + random.gauss(0, 0.05),
                 motion=abs(random.gauss(0.1, 0.05)) + (0.8 if bed == 1 and 50 <= n < 65 else 0),
                 batt=max(5, 95 - n // 30 - bed), contact=True)
        ev = None
        if bed == 3 and n >= 75 and "call" not in sent: d["call"], ev = True, "call"
        if bed == 2 and n >= 80 and "report" not in sent: d["report"], ev = 4, "report"
        if post(d) and ev:
            sent.add(ev); print(f"sample {n}: sent {'nurse call from Bed 3' if ev == 'call' else 'self-report 4 from Bed 2'}")
    if n in (40, 50, 60, 100): print(f"sample {n}")
    n += 1
    nxt += STEP; time.sleep(max(0, nxt - time.time()))
