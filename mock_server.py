"""
AtomSpectra ESP32 Gateway -- Mock Server (stub)

Stub for testing BecqMoni / InterSpec / custom client integration
without real hardware. Implements all endpoints from openapi.yaml.

Usage:
    pip install flask
    python mock_server.py
    # http://127.0.0.1:5000/healthcheck
"""
import sys, json, time, math, struct, random
from flask import Flask, request, jsonify, Response

app = Flask(__name__)

CHANNELS = 8192
CALIB = [3.522, 0.3837, 2.675E-05, -6.49E-09, 5.675E-13]
START_TIME = time.time()
SAVED_SPECTRA = {}
NEXT_INDEX = 1
MOCK_SERIAL = "ATSP-2024-0042"
TOTAL_COUNTS = 123456
CPS = 42
LIVE_TIME = 3600
DEAD_PCT = 23.0


def make_spectrum():
    bins = [0] * CHANNELS
    peaks = [(662, 500, 15), (1460, 200, 20), (2614, 80, 25)]
    for center_kev, amplitude, sigma in peaks:
        ch = int((center_kev - CALIB[0]) / CALIB[1]) if CALIB[1] != 0 else 0
        for i in range(max(0, ch - 100), min(CHANNELS, ch + 100)):
            bins[i] += int(amplitude * math.exp(-0.5 * ((i - ch) / sigma) ** 2))
    for i in range(CHANNELS):
        bins[i] += random.randint(0, 3)
    return bins


@app.route("/healthcheck")
def healthcheck():
    return jsonify(
        status="ok", analyzer_connected=True, wifi_connected=True,
        uptime_sec=round(time.time() - START_TIME, 1), spectrum_valid=True,
    ), 200


@app.route("/api/status")
def api_status():
    return jsonify(
        analyzer_connected=True, wifi_connected=True, total_counts=TOTAL_COUNTS,
        cpu_load=2300, tcp_client=False, dev=1, version=3, mode=0, freq=40000.0,
        t1=25.3, t2=26.1, t3=0.0, time=LIVE_TIME, noise=5, max=65535,
    )


@app.route("/api/spectrum")
def spectrum_binary():
    bins = make_spectrum()
    data = struct.pack(f"<{CHANNELS}I", *bins)
    return Response(data, mimetype="application/octet-stream")


@app.route("/api/spectrum.json")
def spectrum_json():
    bins = make_spectrum()
    return jsonify(bins=bins, total=TOTAL_COUNTS, cpu=2300, cps=CPS, lost=0,
        time=LIVE_TIME, t1=25.3, t2=26.1, t3=0.0, serial=MOCK_SERIAL, calib=CALIB)


def _render_xml(bins, total, live_time, calib, serial, filename):
    ts = time.strftime("%Y-%m-%dT%H:%M:%S")
    real_time = live_time / (1.0 - DEAD_PCT / 100.0)
    parts = ['<?xml version="1.0"?>', "<ResultDataFile>",
        "  <FormatVersion>120920</FormatVersion>", "  <ResultDataList>",
        "    <ResultData>", f"      <StartTime>{ts}</StartTime>",
        f"      <EndTime>{ts}</EndTime>", "      <EnergySpectrum>",
        f"        <NumberOfChannels>{CHANNELS}</NumberOfChannels>",
        "        <ChannelPitch>1</ChannelPitch>"]
    if calib and len(calib) >= 2:
        parts.append("        <EnergyCalibration>")
        parts.append("          <CoefficientsBiased>")
        for c in calib:
            parts.append(f"            <Coefficient>{c:.15G}</Coefficient>")
        parts.append("          </CoefficientsBiased>")
        parts.append("        </EnergyCalibration>")
    parts += [f"        <ValidPulseCount>{total}</ValidPulseCount>",
        f"        <TotalPulseCount>{total}</TotalPulseCount>",
        f"        <MeasurementTime>{real_time:.1f}</MeasurementTime>",
        f"        <LiveTime>{float(live_time):.1f}</LiveTime>",
        "        <Spectrum>"]
    for b in bins:
        parts.append(f"          <DataPoint>{b}</DataPoint>")
    parts += ["        </Spectrum>", "      </EnergySpectrum>",
        f"      <DeviceConfigReference><Name>{serial}</Name></DeviceConfigReference>",
        "    </ResultData>", "  </ResultDataList>", "</ResultDataFile>"]
    xml = "\n".join(parts)
    return Response(xml, mimetype="application/xml",
        headers={"Content-Disposition": f'attachment; filename="{filename}"',})


def _render_csv(bins, total, live_time, calib, serial, filename):
    real_time = live_time / (1.0 - DEAD_PCT / 100.0)
    calib_str = " ".join(f"{c:.15G}" for c in calib) if calib else ""
    ts = time.strftime("%Y-%m-%dT%H:%M:%S")
    parts = [f"calibcoeff: {calib_str}", "remark: AtomSpectra ESP32 Mock",
        f"livetime: {float(live_time):.1f}", f"realtime: {real_time:.1f}",
        "detectorname: AtomSpectra", f"SerialNumber: {serial}",
        f"starttime: {ts}"]
    for i, b in enumerate(bins):
        parts.append(f"{i + 1}, {b}")
    csv_text = "\n".join(parts)
    return Response(csv_text, mimetype="text/csv",
        headers={"Content-Disposition": f'attachment; filename="{filename}"',})


@app.route("/api/export.xml")
def export_xml():
    return _render_xml(make_spectrum(), TOTAL_COUNTS, LIVE_TIME, CALIB, MOCK_SERIAL, "spectrum.xml")


@app.route("/api/export.csv")
def export_csv():
    return _render_csv(make_spectrum(), TOTAL_COUNTS, LIVE_TIME, CALIB, MOCK_SERIAL, "spectrum.csv")


@app.route("/api/command", methods=["POST"])
def command():
    body = request.get_data(as_text=True)
    if not body:
        return jsonify(ok=False, error="empty body"), 400
    return jsonify(ok=True)


@app.route("/api/reset", methods=["POST"])
def reset_spectrum():
    return jsonify(ok=True)


@app.route("/api/save", methods=["POST"])
def save():
    global NEXT_INDEX
    idx = NEXT_INDEX
    NEXT_INDEX += 1
    SAVED_SPECTRA[idx] = dict(bins=make_spectrum(), counts=TOTAL_COUNTS,
        time=LIVE_TIME, saved_at=int(time.time()))
    return jsonify(ok=True, index=idx)


@app.route("/api/list")
def list_saved():
    spectra = [dict(index=k, counts=v["counts"], time=v["time"], saved_at=v["saved_at"])
        for k, v in sorted(SAVED_SPECTRA.items())]
    return jsonify(spectra=spectra, count=len(spectra))


@app.route("/api/saved/<int:index>/export.xml")
def saved_xml(index):
    sp = SAVED_SPECTRA.get(index)
    if not sp:
        return jsonify(error="not found"), 404
    return _render_xml(sp["bins"], sp["counts"], sp["time"], CALIB, MOCK_SERIAL, f"spectrum_{index:04d}.xml")


@app.route("/api/saved/<int:index>/export.csv")
def saved_csv(index):
    sp = SAVED_SPECTRA.get(index)
    if not sp:
        return jsonify(error="not found"), 404
    return _render_csv(sp["bins"], sp["counts"], sp["time"], CALIB, MOCK_SERIAL, f"spectrum_{index:04d}.csv")


@app.route("/api/saved/<int:index>/spectrum.json")
def saved_json(index):
    sp = SAVED_SPECTRA.get(index)
    if not sp:
        return jsonify(error="not found"), 404
    return jsonify(bins=sp["bins"], total=sp["counts"], cpu=0, cps=0, lost=0,
        time=sp["time"], t1=0.0, t2=0.0, t3=0.0, serial=MOCK_SERIAL, calib=CALIB)


@app.route("/api/saved/<int:index>/delete", methods=["POST"])
def saved_delete(index):
    ok = index in SAVED_SPECTRA
    if ok:
        del SAVED_SPECTRA[index]
    return jsonify(ok=ok)


@app.route("/api/device")
def device():
    return jsonify(valid=True, dev=1, version=3, rise=10, fall=20, noise=5,
        freq=40000.0, max_integral=65535, hyst=3, mode=0, step=1, time=LIVE_TIME,
        pot=128, t1=25.3, t2=26.1, t3=0.0, tc_on=True, tp=100,
        serial=MOCK_SERIAL, calibration=CALIB, calib_order=4)


@app.route("/api/calibration", methods=["POST"])
def set_calibration():
    global CALIB
    try:
        data = request.get_json(force=True)
        coeffs = data.get("coeffs")
        if not coeffs or not isinstance(coeffs, list):
            return jsonify(ok=False, error="missing coeffs"), 400
        CALIB = [float(c) for c in coeffs[:5]]
        return jsonify(ok=True)
    except Exception as e:
        return jsonify(ok=False, error=str(e)), 400


@app.route("/api/system")
def system_info():
    return jsonify(free_heap=200000, min_free_heap=150000,
        uptime_sec=round(time.time() - START_TIME, 1), usb_connected=True,
        wifi_connected=True, tcp_client=False, flash_total=13500000,
        flash_used=500000, rssi=-55, ssid="MockWiFi")


@app.route("/api/reboot-device", methods=["POST"])
def reboot_device():
    return jsonify(ok=True)


@app.route("/api/reboot-esp", methods=["POST"])
def reboot_esp():
    return jsonify(ok=True)


@app.route("/api/wifi/reset", methods=["POST"])
def wifi_reset():
    return jsonify(ok=True)


@app.route("/")
def index():
    return "<h1>AtomSpectra ESP32 Gateway &mdash; Mock Server</h1><p><a href='/healthcheck'>/healthcheck</a></p>"


if __name__ == "__main__":
    print("AtomSpectra Mock Server starting on http://127.0.0.1:5000")
    print("Swagger spec: openapi.yaml (in repo root)")
    app.run(host="0.0.0.0", port=5000, debug=True)
