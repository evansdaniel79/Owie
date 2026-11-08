#!/usr/bin/env python3
"""Preview the Owie web UI on your laptop, no board needed.

    python3 tools/preview_ui.py [port]    # then open http://localhost:8000
"""
import http.server, json, os, re, sys

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")
CELLS = "".join(f"<tr><th>Cell {i+1}</th><td>{3.92+i*0.003:.3f}v</td></tr>" for i in range(15))
VALS = {
    "TOTAL_VOLTAGE": "58.84", "CURRENT_AMPS": "1.2", "MAX_CURRENT_AMPS": "18.4", "BMS_SOC": "71", "OVERRIDDEN_SOC": "68",
    "VOLTAGE_BASED_SOC": "69", "BOTTOM_SOC": "10", "TOP_SOC": "95", "BOTTOM_MILLIAMP_HOURS": "2500",
    "CURRENT_MILLIAMP_HOURS": "800", "USED_CHARGE_MAH": "1234", "REGENERATED_CHARGE_MAH": "210",
    "UPTIME": "0d 0h 12m 3s", "GRACEFUL_SHUTDOWN_COUNT": "4", "OWIE_version": "1.7.0",
    "DISPLAY_AP_NAME": "Owie-1234", "AP_SELF_NAME": "", "AP_PASSWORD": "", "WIFI_POWER": "20",
    "WIFI_POWER_OPTIONS": "<option>10</option><option selected>20</option>",
    "CAN_ENABLE_LOCKING": "1", "LOCKING_ENABLED": "", "IS_LOCKED": "", 
    "CELL_VOLTAGE_TABLE": CELLS,
    "TEMPERATURE_TABLE": "<tr><th>Pack 1</th><td>24C</td></tr><tr><th>BMS board</th><td>31C</td></tr>",
    "PACKET_STATS_TABLE": "<tr><th>Type 0</th><td>100</td></tr>",
    "BMS_WARNING": "none", "BMS_FIRMWARE_VERSION": "3001 (Pint)", "BMS_VARIANT": "1",
    "BMS_LAST_SHUTDOWN": "BU - power button", "BMS_THRESHOLDS": "4.2/4.15/2.8V, 60C",
    "BMS_BALANCING": "none", "BMS_CYCLES": "12", "BMS_HEALTH": "98%",
    "RECEIVED_SERIAL_DEC": "1234567", "RECEIVED_SERIAL_HEX": "0x12D687",
    "SENT_SERIAL_DEC": "4289449455", "SENT_SERIAL_HEX": "0xFFABCDEF",
    "SSID": "Owie-1234", "PASS": "", "UPDATE_ERROR": "example error",
}
PAGES = {"/": "index", "/settings": "settings", "/battery": "battery", "/monitor": "monitor",
         "/dev_settings": "dev_settings", "/wifi": "wifi", "/update": "update",
         "/update_failed": "update_failed_template", "/update_ok": "update_successful_response"}

def render(name):
    s = open(os.path.join(DATA, name + ".html")).read()
    return re.sub(r"%([A-Za-z_]*)%", lambda m: "%" if not m.group(1) else VALS.get(m.group(1), "??" + m.group(1)), s)

class H(http.server.BaseHTTPRequestHandler):
    def send(self, body, ctype="text/html"):
        b = body.encode()
        self.send_response(200); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        p = self.path.split("?")[0]
        if p in PAGES: return self.send(render(PAGES[p]))
        if p == "/styles.css": return self.send(open(os.path.join(DATA, "styles.css")).read(), "text/css")
        if p == "/autoupdate": return self.send(json.dumps({k: VALS[k] for k in VALS if k.isupper() or k == "OWIE_version"}), "application/json")
        if p == "/stockmode": return self.send("00", "text/plain")
        if p == "/lock": return self.send("1" if "toggleArm" in self.path else "ok", "text/plain")
        self.send_response(404); self.end_headers()
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n).decode()
        print("POST", self.path, body)
        self.send_response(302); self.send_header("Location", self.path.split("?")[0]); self.end_headers()
    def log_message(self, *a): pass

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    print(f"Owie UI preview at http://localhost:{port}")
    http.server.ThreadingHTTPServer(("", port), H).serve_forever()
