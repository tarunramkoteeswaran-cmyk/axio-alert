"""Axio Alert ward dashboard (prototype, not a medical device).
Run:  pip install flask   then   python app.py   ->  http://localhost:5000
ESP32 (or sim.py) POSTs JSON to /data. Everything is kept in memory."""
import csv, io, threading, time
from collections import deque
from flask import Flask, jsonify, request, Response

app = Flask(__name__)
LOCK = threading.Lock()
BEDS, CALLS, REPORTS = {}, [], []
BASE_N = 20        # samples used to learn each patient's baseline
MOTION_MAX = 0.5   # above this the reading is paused (motion gate)
OFFLINE_S = 10     # no data for this long = offline


def score(b):
    """Placeholder heuristic: rise in HR and GSR, drop in skin temp, all vs baseline. Tune later."""
    h0, g0, t0 = b["baseline"]
    hr = min(1, max(0, (b["hr"] - h0) / h0) / 0.25)
    gs = min(1, max(0, (b["gsr"] - g0) / g0) / 0.5)
    tp = min(1, max(0, t0 - b["temp"]))
    return 100 * (0.45 * hr + 0.40 * gs + 0.15 * tp)


def ingest(d):
    now, bed = time.time(), str(d.get("bed", "")).strip()
    if not bed:
        return False
    b = BEDS.setdefault(bed, {"bed": bed, "base": [], "baseline": None,
                              "hist": deque(maxlen=120), "score": 0, "run": 0})
    b.update(name=d.get("name") or b.get("name") or "Bed " + bed, sim=bool(d.get("sim")), seen=now,
             hr=d.get("hr"), gsr=d.get("gsr"), temp=d.get("temp"), batt=d.get("batt"),
             motion=float(d.get("motion") or 0), contact=bool(d.get("contact", True)))
    b["paused"] = b["motion"] > MOTION_MAX or not b["contact"]
    if not b["paused"] and None not in (b["hr"], b["gsr"], b["temp"]):
        if b["baseline"] is None:
            b["base"].append((b["hr"], b["gsr"], b["temp"]))
            if len(b["base"]) >= BASE_N:
                b["baseline"] = [sum(x) / len(x) for x in zip(*b["base"])]
        else:
            b["score"] = float(d["score"]) if "score" in d else score(b)
            b["run"] = b["run"] + 1 if b["score"] >= 65 else 0
    if b["baseline"] is not None:
        b["hist"].append(round(b["score"]))
    if d.get("call") and not any(c["bed"] == bed and c["ack"] is None for c in CALLS):
        CALLS.append({"id": len(CALLS) + 1, "bed": bed, "name": b["name"], "raised": now, "ack": None})
    if d.get("report"):
        REPORTS.append({"bed": bed, "t": now, "level": int(d["report"])})
    return True


def status(b, now):
    if now - b["seen"] > OFFLINE_S: return "offline", "Offline"
    if b["baseline"] is None:      return "calib", "Calibrating"
    if b["paused"]:                return "paused", "Reading paused"
    if b["score"] >= 65 and b["run"] >= 5: return "check", "Check in suggested"
    if b["score"] >= 35:           return "elev", "Elevated"
    return "ok", "Normal"


@app.post("/data")
def data():
    d = request.get_json(silent=True) or {}
    with LOCK:
        ok = ingest(d)
        breathe = ok and status(BEDS[str(d["bed"]).strip()], time.time())[0] == "check"
    return jsonify(ok=ok, breathe=bool(breathe)), (200 if ok else 400)


@app.get("/api/state")
def state():
    now = time.time()
    with LOCK:
        beds = []
        for b in BEDS.values():
            code, label = status(b, now)
            beds.append(dict(bed=b["bed"], name=b["name"], sim=b["sim"], code=code, label=label,
                             score=round(b["score"]), hr=b["hr"], gsr=b["gsr"], temp=b["temp"],
                             motion=b["motion"], batt=b["batt"], contact=b["contact"],
                             age=round(now - b["seen"]), hist=list(b["hist"]),
                             baseline=b["baseline"], calib=len(b["base"]),
                             call=any(c["bed"] == b["bed"] and c["ack"] is None for c in CALLS)))
        beds.sort(key=lambda x: (x["code"] == "offline", not x["call"], -x["score"]))
        return jsonify(now=now, beds=beds, calls=CALLS[::-1], reports=REPORTS[-15:][::-1], base_n=BASE_N)


@app.post("/api/ack")
def ack():
    cid = (request.get_json(silent=True) or {}).get("id")
    with LOCK:
        for c in CALLS:
            if c["id"] == cid and c["ack"] is None:
                c["ack"] = time.time()
    return jsonify(ok=True)


@app.post("/api/reset")
def reset():
    with LOCK:
        BEDS.clear(); CALLS.clear(); REPORTS.clear()
    return jsonify(ok=True)


@app.get("/export/calls.csv")
def export():
    out = io.StringIO(); w = csv.writer(out)
    w.writerow(["id", "bed", "raised", "acknowledged", "response_s"])
    f = lambda t: time.strftime("%H:%M:%S", time.localtime(t)) if t else ""
    with LOCK:
        for c in CALLS:
            w.writerow([c["id"], c["bed"], f(c["raised"]), f(c["ack"]), round(c["ack"] - c["raised"], 1) if c["ack"] else ""])
    return Response(out.getvalue(), mimetype="text/csv", headers={"Content-Disposition": "attachment; filename=calls.csv"})


@app.get("/")
def index():
    return Response(PAGE, mimetype="text/html")


PAGE = r"""<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Axio Alert ward dashboard</title><style>
:root{--bg:#f2f5f5;--card:#fff;--fg:#14201f;--mut:#5b6b69;--line:#d4dddb;--ok:#16793f;--elev:#a8650a;--check:#c23a28;--off:#7a8886}
@media(prefers-color-scheme:dark){:root{--bg:#0e1616;--card:#152120;--fg:#e5eeec;--mut:#9bb0ad;--line:#2a3d3b;--ok:#63d391;--elev:#f0b45a;--check:#ff7d68;--off:#8fa6a3}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,sans-serif;padding:14px}
h1{font-size:22px;margin:0}h2{font-size:12px;letter-spacing:.07em;text-transform:uppercase;color:var(--mut);margin:0 0 8px}
.top{display:flex;flex-wrap:wrap;gap:6px 16px;align-items:baseline;margin-bottom:10px}.mut{color:var(--mut);font-size:12px}
.disc{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:6px 10px;font-size:12px;margin-bottom:10px}
.grid{display:grid;grid-template-columns:minmax(0,1.6fr) minmax(280px,1fr);gap:12px;align-items:start}@media(max-width:860px){.grid{grid-template-columns:1fr}}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:12px;margin-bottom:12px;overflow-x:auto}
table{width:100%;border-collapse:collapse}th{text-align:left;font-size:11px;color:var(--mut);padding:0 6px 4px 0}td{padding:6px 6px 6px 0;border-top:1px solid var(--line);vertical-align:middle}
tr.row{cursor:pointer}tr.sel td{background:rgba(10,118,111,.1)}
.pill{display:inline-block;padding:2px 8px;border-radius:99px;font-weight:600;font-size:12px;border:1.5px solid currentColor}
.ok{color:var(--ok)}.elev{color:var(--elev)}.check{color:var(--check)}.offline,.paused,.calib{color:var(--off)}
.call{background:var(--check);color:#fff;border-radius:10px;padding:10px 12px;margin-bottom:8px;display:flex;flex-wrap:wrap;gap:8px;align-items:center;justify-content:space-between;font-weight:600}
button{font:600 13px system-ui;border:0;border-radius:7px;padding:7px 12px;cursor:pointer;background:#fff;color:#14201f}
.btn2{background:var(--card);color:var(--fg);border:1px solid var(--line)}
.sim{font-size:10px;border:1px solid var(--line);border-radius:4px;padding:0 4px;color:var(--mut);margin-left:4px}
.kv{display:grid;grid-template-columns:auto 1fr 1fr;gap:4px 12px;font-variant-numeric:tabular-nums}.kv b{color:var(--mut);font-weight:500}
</style></head><body>
<div class="top"><h1>Axio Alert</h1><span class="mut">Ward dashboard</span><span class="mut" id="conn">Connecting...</span></div>
<div class="disc"><b>Prototype.</b> Early-warning aid, not a diagnosis or a medical device. Scores are a heuristic that is not clinically validated. "Check in suggested" is a prompt for a nurse, not a finding.</div>
<div id="calls"></div>
<div class="grid"><div>
<div class="card"><h2>Ward, highest score first</h2><table><thead><tr><th>Bed</th><th>Status</th><th>Score</th><th>Last 2 min</th><th>Device</th></tr></thead><tbody id="rows"></tbody></table><div class="mut" id="empty"></div></div>
<div class="card"><h2>Nurse call log</h2><table><thead><tr><th>#</th><th>Bed</th><th>Raised</th><th>Acknowledged</th><th>Response</th></tr></thead><tbody id="log"></tbody></table>
<p style="margin:8px 0 0"><a href="/export/calls.csv">Download CSV</a> <button class="btn2" onclick="if(confirm('Clear all beds and logs?'))fetch('/api/reset',{method:'POST'})">Reset demo</button></p></div></div>
<div><div class="card"><h2>Patient detail</h2><div id="detail" class="mut">Click a bed.</div></div>
<div class="card"><h2>Self-reports (1 calm to 5 very stressed)</h2><table><tbody id="rep"></tbody></table></div></div></div>
<script>
const $=s=>document.querySelector(s);let sel=null,S=null;
const esc=s=>String(s).replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const tm=t=>t?new Date(t*1000).toLocaleTimeString():'';
const spark=h=>{if(h.length<2)return'';const p=h.slice(-120).map((v,i,a)=>(i/(a.length-1)*110).toFixed(1)+','+(26-v/100*26).toFixed(1)).join(' ');return`<svg width="110" height="28" viewBox="0 0 110 28"><polyline fill="none" stroke="currentColor" stroke-width="1.6" points="${p}"/></svg>`};
const dev=b=>b.code=='offline'?`Offline ${b.age}s`:(b.contact?'':'Poor contact. ')+(b.batt!=null&&b.batt<20?'Battery low ('+b.batt+'%)':'OK');
function draw(){
 const d=S;$('#empty').textContent=d.beds.length?'':'No devices yet. Start sim.py or power on a wearable.';
 $('#calls').innerHTML=d.calls.filter(c=>!c.ack).map(c=>`<div class="call"><span>NURSE CALL: Bed ${esc(c.bed)} (${esc(c.name)}) at ${tm(c.raised)}, waiting ${Math.round(d.now-c.raised)} s</span><button onclick="ack(${c.id})">Acknowledge</button></div>`).join('');
 $('#rows').innerHTML=d.beds.map(b=>`<tr class="row ${sel==b.bed?'sel':''}" onclick="sel='${esc(b.bed)}';draw()"><td><b>${esc(b.bed)}</b> ${esc(b.name)}${b.sim?'<span class="sim">SIM</span>':''}${b.call?' 🔔':''}</td><td><span class="pill ${b.code}">${b.label}</span></td><td>${b.code=='calib'?b.calib+'/'+d.base_n:b.score}</td><td class="${b.code}">${spark(b.hist)}</td><td class="mut">${dev(b)}</td></tr>`).join('');
 $('#log').innerHTML=d.calls.map(c=>`<tr><td>${c.id}</td><td>${esc(c.bed)}</td><td>${tm(c.raised)}</td><td>${c.ack?tm(c.ack):'<b>waiting</b>'}</td><td>${c.ack?(c.ack-c.raised).toFixed(1)+' s':''}</td></tr>`).join('');
 $('#rep').innerHTML=d.reports.map(r=>`<tr><td>${tm(r.t)}</td><td>Bed ${esc(r.bed)}</td><td><b>${r.level}</b> / 5</td></tr>`).join('')||'<tr><td class="mut">None yet</td></tr>';
 const b=d.beds.find(x=>x.bed==sel);if(!b){$('#detail').textContent='Click a bed.';return}
 const bl=b.baseline,f=(v,n)=>v==null?'-':(+v).toFixed(n);
 $('#detail').innerHTML=`<p><b>Bed ${esc(b.bed)}, ${esc(b.name)}</b> <span class="pill ${b.code}">${b.label}</span></p>`+
 (b.code=='paused'?'<p class="mut">Reading paused (patient moving or poor contact). Score is the last trusted value.</p>':'')+
 `<div class="kv"><b></b><b>Now</b><b>Baseline</b><b>HR (bpm)</b><span>${f(b.hr,0)}</span><span>${bl?f(bl[0],0):'-'}</span><b>GSR</b><span>${f(b.gsr,0)}</span><span>${bl?f(bl[1],0):'-'}</span><b>Skin temp (C)</b><span>${f(b.temp,1)}</span><span>${bl?f(bl[2],1):'-'}</span><b>Motion</b><span>${f(b.motion,2)}</span><span></span><b>Battery</b><span>${b.batt==null?'-':b.batt+'%'}</span><span></span></div>`+
 `<p class="mut">Score ${b.score}/100, relative to this patient's own baseline. Last data ${b.age} s ago.</p>`;
}
async function ack(id){await fetch('/api/ack',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id})});tick()}
async function tick(){try{S=await(await fetch('/api/state')).json();$('#conn').textContent='Server OK';draw()}catch(e){$('#conn').textContent='Server not reachable'}}
tick();setInterval(tick,1000);
</script></body></html>"""

if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)
