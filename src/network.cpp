#include "network.h"

#include <DNSServer.h>
#include <ESP8266WiFi.h>
#include <ESPAsyncWebServer.h>

#include <algorithm>
#include <functional>

#include "ArduinoJson.h"
#include "async_ota.h"
#include "battery_pack.h"
#include "bms_relay.h"
#include "data.h"
#include "ride_logger.h"
#include "settings.h"
#include "task_queue.h"

extern RideLogger rideLogger;  // defined in bms_main.cpp

namespace {
DNSServer dnsServer;
AsyncWebServer webServer(80);
AsyncWebSocket ws("/rawdata");

const String defaultPass("****");
BmsRelay *relay;

const String owie_version = "1.7.0";

String renderPacketStatsTable() {
  String result(
      PSTR("<table><tr><th>ID</th><th>Period</th><th>Deviation</th><th>Count</"
           "th></tr>"));
  for (const IndividualPacketStat &stat :
       relay->getPacketTracker().getIndividualPacketStats()) {
    if (stat.id < 0) {
      continue;
    }
    result.concat(PSTR("<tr><td>"));

    char buffer[16];
    snprintf_P(buffer, sizeof(buffer), PSTR("%X"), stat.id);
    result.concat(buffer);

    result.concat(PSTR("</td><td>"));
    result.concat(stat.mean_period_millis());
    result.concat(PSTR("</td><td>"));
    result.concat(stat.deviation_millis());
    result.concat(PSTR("</td><td>"));
    result.concat(stat.total_num);
    result.concat(PSTR("</td></tr>"));
  }

  result.concat(PSTR(
      "<tr><th>Unknown Bytes</th><th>Checksum Mismatches</th></tr><tr><td>"));
  result.concat(
      relay->getPacketTracker().getGlobalStats().total_unknown_bytes_received);
  result.concat(PSTR("</td><td>"));
  result.concat(relay->getPacketTracker()
                    .getGlobalStats()
                    .total_packet_checksum_mismatches);
  result.concat(PSTR("</td></tr></table>"));
  return result;
}

String uptimeString() {
  const unsigned long nowSecs = millis() / 1000;
  const int hrs = nowSecs / 3600;
  const int mins = (nowSecs % 3600) / 60;
  const int secs = nowSecs % 60;
  String ret;
  if (hrs) {
    ret.concat(hrs);
    ret.concat('h');
  }
  ret.concat(mins);
  ret.concat('m');
  ret.concat(secs);
  ret.concat('s');
  return ret;
}

String bmsWarningString() {
  const int secs = relay->getShutdownCountdownSeconds();
  if (secs < 0) {
    return String("none");
  }
  String s;
  s.reserve(64);
  s.concat("BMS off in ");
  s.concat(secs);
  s.concat(" s:");
  const uint8_t mask = relay->getShutdownReasonMask();
  if (mask & 1) s.concat(" over-voltage");
  if (mask & 2) s.concat(" under-voltage");
  if (mask & 4) s.concat(" over-temperature");
  if (mask & 8) s.concat(" idle");
  return s;
}

String getTempString() {
  const uint8_t *thermTemps = relay->getTemperaturesCelsius();
  String temps;
  temps.reserve(256);
  temps.concat("<tr>");
  for (int i = 0; i < 5; i++) {
    // The last column is the thermistor on the BMS board, not a pack sensor.
    temps.concat(i == BmsRelay::BOARD_TEMPERATURE_INDEX ? "<td title='BMS board'>"
                                                        : "<td>");
    if (thermTemps[i] == BmsRelay::TEMPERATURE_INVALID) {
      temps.concat("n/a");
    } else {
      temps.concat(thermTemps[i]);
    }
    temps.concat("</td>");
  }
  temps.concat("<tr>");
  return temps;
}

String generateOwieStatusJson() {
  DynamicJsonDocument status(1024);
  String jsonOutput;
  const uint16_t *cellMillivolts = relay->getCellMillivolts();
  String out;
  out.reserve(256);
  for (int i = 0; i < 3; i++) {
    out.concat("<tr>");
    for (int j = 0; j < 5; j++) {
      out.concat("<td>");
      out.concat(cellMillivolts[i * 5 + j] / 1000.0);
      out.concat("</td>");
    }
    out.concat("<tr>");
  }

  status["TOTAL_VOLTAGE"] =
      String(relay->getTotalVoltageMillivolts() / 1000.0, 2) + "v";
  status["CURRENT_AMPS"] =
      String(relay->getCurrentMilliamps() / 1000.0, 1) + " Amps";
  status["MAX_CURRENT_AMPS"] =
      String(relay->getMaxCurrentMilliamps() / 1000.0, 1) + " Amps";
  status["BMS_SOC"] = String(relay->getBmsReportedSOC()) + "%";
  status["OVERRIDDEN_SOC"] = String(relay->getOverriddenSOC()) + "%";
  status["USED_CHARGE_MAH"] = String(relay->getUsedChargeMah()) + " mAh";
  status["REGENERATED_CHARGE_MAH"] =
      String(relay->getRegeneratedChargeMah()) + " mAh";
  status["UPTIME"] = uptimeString();
  status["BMS_WARNING"] = bmsWarningString();
  status["CELL_VOLTAGE_TABLE"] = out;
  status["TEMPERATURE_TABLE"] = getTempString();

  // BMS Serial Numbers
  char hexBuf[16];
  uint32_t capturedSerial = relay->getCapturedBMSSerial();
  uint32_t overrideSerial = relay->getBMSSerialOverride();

  status["RECEIVED_SERIAL_DEC"] = String(capturedSerial);
  snprintf(hexBuf, sizeof(hexBuf), "0x%X", capturedSerial);
  status["RECEIVED_SERIAL_HEX"] = String(hexBuf);

  status["SENT_SERIAL_DEC"] = String(overrideSerial);
  snprintf(hexBuf, sizeof(hexBuf), "0x%X", overrideSerial);
  status["SENT_SERIAL_HEX"] = String(hexBuf);

  serializeJson(status, jsonOutput);
  return jsonOutput;
}

bool lockingPreconditionsMet() {
  return strlen(Settings->ap_self_password) > 0;
}
const char *lockedStatusDataAttrValue() {
  return Settings->is_locked ? "1" : "";
};

// The async web server re-scans every value it substitutes into a page, so a
// '%' in a value is read as the start of another placeholder and breaks the
// page ("UNKNOWN PLACEHOLDER"). Values that may contain '%' (or markup) go
// through this so they can never do that.
String htmlSafe(const String &in) {
  String out;
  out.reserve(in.length() + 8);
  for (unsigned i = 0; i < in.length(); i++) {
    const char c = in[i];
    switch (c) {
      case '%': out.concat("&#37;"); break;
      case '&': out.concat("&amp;"); break;
      case '<': out.concat("&lt;"); break;
      case '>': out.concat("&gt;"); break;
      case '"': out.concat("&quot;"); break;
      case '\'': out.concat("&#39;"); break;
      default: out.concat(c);
    }
  }
  return out;
}

String templateProcessor(const String &var) {
  if (var == "TOTAL_VOLTAGE") {
    return String(relay->getTotalVoltageMillivolts() / 1000.0,
                  /* decimalPlaces = */ 2);
  } else if (var == "CURRENT_AMPS") {
    return String(relay->getCurrentMilliamps() / 1000.0,
                  /* decimalPlaces = */ 1);
  } else if (var == "RIDELOG_STATUS") {
    if (!rideLogger.active()) {
      return "Not available (not enough free memory on the board).";
    }
    const unsigned minutes = rideLogger.size() * RideLogger::INTERVAL_MS / 60000;
    const unsigned keepMinutes =
        rideLogger.capacity() * RideLogger::INTERVAL_MS / 60000;
    return "Recording automatically: " + String(minutes) +
           " min stored (keeps the latest " + String(keepMinutes) + " min).";
  } else if (var == "LEARN_STATUS") {
    const auto &gauge = relay->getBatteryFuelGauge();
    if (gauge.isLearned()) {
      return "Learned";
    }
    return "Learning (" + String(gauge.getLearningProgressPercent()) +
           "&#37;)";
  } else if (var == "PACK_NAME") {
    if (!battery_pack::isConfigured(
            Settings->pack_cell_type, Settings->pack_series,
            Settings->pack_parallel, Settings->pack_custom_cell_mah)) {
      return "Not set up (Settings > Battery Pack)";
    }
    String s(Settings->pack_series);
    s.concat("S");
    s.concat(Settings->pack_parallel);
    s.concat("P ");
    s.concat(battery_pack::cellName(Settings->pack_cell_type));
    return s;
  } else if (var == "PACK_RATED_MAH") {
    const uint32_t mah = battery_pack::ratedMah(
        Settings->pack_cell_type, Settings->pack_series,
        Settings->pack_parallel, Settings->pack_custom_cell_mah);
    return mah == 0 ? String("n/a") : String(mah) + " mAh";
  } else if (var == "PACK_RATED_WH") {
    const uint32_t wh = battery_pack::ratedWh(
        Settings->pack_cell_type, Settings->pack_series,
        Settings->pack_parallel, Settings->pack_custom_cell_mah);
    return wh == 0 ? String("n/a") : String(wh) + " Wh";
  } else if (var == "PACK_LEARNED_MAH") {
    const int32_t mah = relay->getBatteryFuelGauge().getEstimatedCapacityMah();
    return mah < 0 ? String("Learning (needs a ride covering about half the pack)")
                   : String(mah) + " mAh";
  } else if (var == "PACK_HEALTH") {
    const uint32_t rated = battery_pack::ratedMah(
        Settings->pack_cell_type, Settings->pack_series,
        Settings->pack_parallel, Settings->pack_custom_cell_mah);
    const int32_t learned =
        relay->getBatteryFuelGauge().getEstimatedCapacityMah();
    if (rated == 0 || learned < 0) {
      return "n/a";
    }
    // Capped at 100: estimates above the rating are measurement noise.
    const uint32_t pct = std::min<uint32_t>(100, learned * 100UL / rated);
    return String(pct) + "&#37;";
  } else if (var == "MAX_CURRENT_AMPS") {
    return String(relay->getMaxCurrentMilliamps() / 1000.0,
                  /* decimalPlaces = */ 1);
  } else if (var == "BMS_SOC") {
    return String(relay->getBmsReportedSOC());
  } else if (var == "OVERRIDDEN_SOC") {
    return String(relay->getOverriddenSOC());
  } else if (var == "VOLTAGE_BASED_SOC") {
    return String(relay->getBatteryFuelGauge().getVoltageBasedSoc());
  } else if (var == "BOTTOM_SOC") {
    return String(relay->getBatteryFuelGauge().getState().bottomSoc);
  } else if (var == "TOP_SOC") {
    return String(relay->getBatteryFuelGauge().getState().topSoc);
  } else if (var == "BOTTOM_MILLIAMP_HOURS") {
    return String(
        relay->getBatteryFuelGauge().getState().bottomMilliampSeconds / 3600);
  } else if (var == "CURRENT_MILLIAMP_HOURS") {
    return String(
        relay->getBatteryFuelGauge().getState().currentMilliampSeconds / 3600);
  } else if (var == "USED_CHARGE_MAH") {
    return String(relay->getUsedChargeMah());
  } else if (var == "REGENERATED_CHARGE_MAH") {
    return String(relay->getRegeneratedChargeMah());
  } else if (var == "OWIE_version") {
    return owie_version;
  } else if (var == "BMS_FIRMWARE_VERSION") {
    const uint16_t v = relay->getBmsFirmwareVersion();
    if (v == 0) {
      return "n/a (sent once at BMS boot)";
    }
    String s(v);
    if (v == 2006) s.concat(" (XR 4206-4209)");
    else if (v == 2007) s.concat(" (XR 4210)");
    else if (v >= 3000 && v < 4000) s.concat(" (Pint)");
    return s;
  } else if (var == "BMS_VARIANT") {
    const uint8_t v = relay->getBmsVariant();
    switch (v) {
      case 0: return "n/a";
      case 0x02: return "0x02 (XR 4208-4210)";
      case 0x04: return "0x04 (XR 4206)";
      case 0x06: return "0x06 (Pint)";
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "0x%02X", v);
    return String(buf);
  } else if (var == "BMS_LAST_SHUTDOWN") {
    const uint8_t code = relay->getLastShutdownReasonCode();
    String s(code);
    s.concat(" (");
    s.concat(BmsRelay::shutdownReasonName(code));
    s.concat(")");
    return s;
  } else if (var == "BMS_THRESHOLDS") {
    if (relay->getOverVoltageThresholdMillivolts() == 0) {
      return "n/a";
    }
    String s;
    s.reserve(80);
    s.concat("OV ");
    s.concat(relay->getOverVoltageThresholdMillivolts());
    s.concat(" mV, full ");
    s.concat(relay->getChargeCompleteThresholdMillivolts());
    s.concat(" mV, UV ");
    s.concat(relay->getUnderVoltageThresholdMillivolts());
    s.concat(" mV, OT ");
    s.concat(relay->getOverTemperatureThresholdCelsius());
    s.concat(" C");
    return s;
  } else if (var == "BMS_BALANCING") {
    const uint16_t mask = relay->getBalancingMask();
    int cells = 0;
    for (uint16_t m = mask; m; m >>= 1) cells += m & 1;
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%04X (%d cells)", mask, cells);
    return String(buf);
  } else if (var == "BMS_CYCLES") {
    if (!relay->hasCycleInfo()) {
      return "n/a (not reported by XR 4206-4209 BMS)";
    }
    return String(relay->getCycleCount());
  } else if (var == "BMS_HEALTH") {
    if (!relay->hasCycleInfo()) {
      return "n/a";
    }
    return String(relay->getStateOfHealthPercent()) + "&#37;";
  } else if (var == "SSID") {
    return htmlSafe(Settings->ap_name);
  } else if (var == "PASS") {
    if (strlen(Settings->ap_password) > 0) {
      return defaultPass;
    }
    return "";
  } else if (var == "GRACEFUL_SHUTDOWN_COUNT") {
    return String(Settings->graceful_shutdown_count);
  } else if (var == "UPTIME") {
    return uptimeString();
  } else if (var == "BMS_WARNING") {
    return bmsWarningString();
  } else if (var == "IS_LOCKED") {
    return lockedStatusDataAttrValue();
  } else if (var == "CAN_ENABLE_LOCKING") {
    return lockingPreconditionsMet() ? "1" : "";
  } else if (var == "LOCKING_ENABLED") {
    return Settings->locking_enabled ? "1" : "";
  } else if (var == "PACKET_STATS_TABLE") {
    return renderPacketStatsTable();
  } else if (var == "CELL_VOLTAGE_TABLE") {
    const uint16_t *cellMillivolts = relay->getCellMillivolts();
    String out;
    out.reserve(256);
    for (int i = 0; i < 3; i++) {
      out.concat("<tr>");
      for (int j = 0; j < 5; j++) {
        out.concat("<td>");
        out.concat(cellMillivolts[i * 5 + j] / 1000.0);
        out.concat("</td>");
      }
      out.concat("<tr>");
    }
    return out;
  } else if (var == "TEMPERATURE_TABLE") {
    return getTempString();
  } else if (var == "RECEIVED_SERIAL_DEC") {
    return String(relay->getCapturedBMSSerial());
  } else if (var == "RECEIVED_SERIAL_HEX") {
    char hexBuf[16];
    snprintf(hexBuf, sizeof(hexBuf), "0x%X", relay->getCapturedBMSSerial());
    return String(hexBuf);
  } else if (var == "SENT_SERIAL_DEC") {
    return String(relay->getBMSSerialOverride());
  } else if (var == "SENT_SERIAL_HEX") {
    char hexBuf[16];
    snprintf(hexBuf, sizeof(hexBuf), "0x%X", relay->getBMSSerialOverride());
    return String(hexBuf);
  } else if (var == "AP_PASSWORD") {
    return htmlSafe(Settings->ap_self_password);
  } else if (var == "AP_SELF_NAME") {
    return htmlSafe(Settings->ap_self_name);
  } else if (var == "DISPLAY_AP_NAME") {
    char apDisplayName[64];
    if (strlen(Settings->ap_self_name) > 0) {
      snprintf(apDisplayName, sizeof(apDisplayName), "%s",
               Settings->ap_self_name);
    } else {
      snprintf(apDisplayName, sizeof(apDisplayName), "Owie-%04X",
               ESP.getChipId() & 0xFFFF);
    }
    return htmlSafe(String(apDisplayName));
  } else if (var == "WIFI_POWER") {
    return String(Settings->wifi_power);
  } else if (var == "WIFI_POWER_OPTIONS") {
    String opts;
    opts.reserve(256);
    for (int i = 8; i < 18; i++) {
      opts.concat("<option value='");
      opts.concat(String(i));
      opts.concat("'");
      if (i == Settings->wifi_power) {
        opts.concat(" selected ");
      }
      opts.concat(">");
      opts.concat(String(i));
      opts.concat("</option>");
    }
    return opts;
  }
  return "<script>alert('UNKNOWN PLACEHOLDER')</script>";
}

}  // namespace

void setupWifi() {
  WiFi.setOutputPower(Settings->wifi_power);
  bool stationMode = (strlen(Settings->ap_name) > 0);
  WiFi.mode(stationMode ? WIFI_AP_STA : WIFI_AP);
  char apName[64];
  // sprintf isn't causing the issue of bungled SSID anymore (can't reproduce)
  // but snprintf should be safer, so trying that now
  // 9 bytes should be sufficient
  if (strlen(Settings->ap_self_name) > 0) {
    snprintf(apName, sizeof(apName), Settings->ap_self_name);
  } else {
    snprintf(apName, sizeof(apName), "Owie-%04X", ESP.getChipId() & 0xFFFF);
  }
  WiFi.softAP(apName, Settings->ap_self_password);
  if (stationMode) {
    WiFi.begin(Settings->ap_name, Settings->ap_password);
    WiFi.hostname(apName);
  }
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", WiFi.softAPIP());  // DNS spoofing.
  TaskQueue.postRecurringTask([]() { dnsServer.processNextRequest(); });
}

void setupWebServer(BmsRelay *bmsRelay) {
  relay = bmsRelay;
  AsyncOta.listen(&webServer);
  webServer.addHandler(&ws);
  webServer.onNotFound([](AsyncWebServerRequest *request) {
    request->redirect("http://" + request->client()->localIP().toString() +
                      "/");
  });
  webServer.on("/favicon.ico", HTTP_GET,
               [](AsyncWebServerRequest *request) { request->send(404); });

  webServer.on("/autoupdate", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", generateOwieStatusJson());
  });

  webServer.on("/pack", HTTP_GET, [](AsyncWebServerRequest *request) {
    String out = "{\"cell\":";
    out.concat(Settings->pack_cell_type);
    out.concat(",\"s\":");
    out.concat(Settings->pack_series);
    out.concat(",\"p\":");
    out.concat(Settings->pack_parallel);
    out.concat(",\"mah\":");
    out.concat(Settings->pack_custom_cell_mah);
    out.concat("}");
    request->send(200, "application/json", out);
  });

  webServer.on("/ridelog.csv", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginChunkedResponse(
        "text/csv",
        [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
          // Rows are produced on the fly; `index` is the byte offset, so a
          // fresh download (index 0) restarts the state. One download at a
          // time is expected.
          static size_t nextRow;
          static bool headerSent;
          static String pending;
          if (index == 0) {
            nextRow = 0;
            headerSent = false;
            pending = "";
          }
          if (!headerSent) {
            pending = "t_s,mv,ma_avg,ma_peak,min_cell_mv,max_cell_mv,soc,"
                      "bms_soc,voltage_soc,temp_max_c\n";
            headerSent = true;
          }
          while (pending.length() < maxLen && nextRow < rideLogger.size()) {
            const RideLogger::Sample &s = rideLogger.at(nextRow);
            char line[96];
            snprintf(line, sizeof(line), "%u,%u,%d,%d,%u,%u,%d,%d,%d,%u\n",
                     (unsigned)rideLogger.secondsAt(nextRow), s.total_mv,
                     s.current_ma_avg, s.current_ma_peak, s.min_cell_mv,
                     s.max_cell_mv, s.soc, s.bms_soc, s.voltage_soc,
                     s.temp_max_c);
            pending.concat(line);
            nextRow++;
          }
          const size_t n = std::min<size_t>(maxLen, pending.length());
          memcpy(buffer, pending.c_str(), n);
          pending.remove(0, n);
          return n;  // 0 ends the response once everything has been sent
        });
    response->addHeader("Content-Disposition",
                        "attachment; filename=\"owie-ride-log.csv\"");
    request->send(response);
  });

  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", INDEX_HTML_PROGMEM_ARRAY, INDEX_HTML_SIZE,
                    templateProcessor);
  });
  webServer.on("/styles.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(
        200, "text/css", STYLES_CSS_PROGMEM_ARRAY, STYLES_CSS_SIZE);
    response->addHeader("Cache-Control", "max-age=3600");
    request->send(response);
  });
  webServer.on("/dev_settings", HTTP_GET, [](AsyncWebServerRequest *request) {
    AsyncWebServerResponse *response = request->beginResponse_P(
        200, "text/html", DEV_SETTINGS_HTML_PROGMEM_ARRAY,
        DEV_SETTINGS_HTML_SIZE, templateProcessor);
    response->addHeader("Cache-Control", "max-age=3600");
    request->send(response);
  });
  webServer.on("/battery", HTTP_ANY, [](AsyncWebServerRequest *request) {
    switch (request->method()) {
      case HTTP_GET:
        request->send_P(200, "text/html", BATTERY_HTML_PROGMEM_ARRAY,
                        BATTERY_HTML_SIZE, templateProcessor);
        return;
      case HTTP_POST:
        if (request->getParam("clear_ridelog", true) != nullptr) {
          rideLogger.clear();
        } else if (request->getParam("relearn", true) != nullptr) {
          // Forget everything learned about this battery, in memory and in the
          // copy saved in flash, and start learning again.
          relay->getBatteryFuelGauge().reset();
          Settings->battery_state = BatteryStateMsg_init_default;
          saveSettings();
        } else if (request->getParam("reset_stats", true) != nullptr) {
          relay->getBatteryFuelGauge().reset();
        } else if (request->getParam("reset_settings", true) != nullptr) {
          Settings->battery_state = BatteryStateMsg_init_default;
          saveSettings();
        }
        request->redirect("/battery");
        return;
    }
    request->send(404);
  });
  webServer.on("/wifi", HTTP_ANY, [](AsyncWebServerRequest *request) {
    switch (request->method()) {
      case HTTP_GET:
        request->send_P(200, "text/html", WIFI_HTML_PROGMEM_ARRAY,
                        WIFI_HTML_SIZE, templateProcessor);
        return;
      case HTTP_POST:
        const auto ssidParam = request->getParam("s", true);
        const auto passwordParam = request->getParam("p", true);
        if (ssidParam == nullptr || passwordParam == nullptr ||
            ssidParam->value().length() > sizeof(Settings->ap_name) ||
            passwordParam->value().length() > sizeof(Settings->ap_password)) {
          request->send(200, "text/html", "Invalid SSID or Password.");
          return;
        }
        snprintf(Settings->ap_name, sizeof(Settings->ap_name), "%s",
                 ssidParam->value().c_str());
        snprintf(Settings->ap_password, sizeof(Settings->ap_password), "%s",
                 passwordParam->value().c_str());
        saveSettingsAndRestartSoon();
        request->send(200, "text/html", "WiFi settings saved, restarting...");
        return;
    }
    request->send(404);
  });
  webServer.on("/monitor", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", MONITOR_HTML_PROGMEM_ARRAY,
                    MONITOR_HTML_SIZE, templateProcessor);
  });
  webServer.on("/stockmode", HTTP_GET, [](AsyncWebServerRequest *request) {
    const bool regen =
        Settings->pass_through_current && Settings->pass_through_status;
    request->send(200, "text/plain", regen ? "1" : "0");
  });
  webServer.on("/settings", HTTP_ANY, [](AsyncWebServerRequest *request) {
    switch (request->method()) {
      case HTTP_GET:
        request->send_P(200, "text/html", SETTINGS_HTML_PROGMEM_ARRAY,
                        SETTINGS_HTML_SIZE, templateProcessor);
        return;
      case HTTP_POST:
        const auto apSelfPassword = request->getParam("pw", true);
        const auto apSelfName = request->getParam("apselfname", true);
        const auto wifiPower = request->getParam("wifipower", true);
        if (apSelfPassword == nullptr ||
            apSelfPassword->value().length() >
                sizeof(Settings->ap_self_password) ||
            (apSelfPassword->value().length() < 8 &&
             apSelfPassword->value().length() >
                 0)) {  // this check is necessary so the user can't set a too
                        // small password and thus the network wont' show up
          request->send(400, "text/html",
                        "AP password must be between 8 and 31 characters");
          return;
        }
        if (apSelfName == nullptr ||
            apSelfName->value().length() > sizeof(Settings->ap_self_name) ||
            (apSelfName->value().length() < 1 &&
             apSelfName->value().length() > 0)) {
          request->send(400, "text/html", "Invalid Custom AP Name.");
          return;
        }

        // Set wifi power
        // add aditional sanity checks, so that the power range is between 8 and
        // 17 only!
        if (wifiPower == nullptr || wifiPower->value().toInt() < 8 ||
            wifiPower->value().toInt() > 17) {
          request->send(
              400, "text/html",
              "Wifi Power range MUST be between 8 (dBm) and 17 (dBm).");
          return;
        }
        // Battery pack description (optional; all four fields travel together).
        const auto packCell = request->getParam("pack_cell", true);
        const auto packS = request->getParam("pack_s", true);
        const auto packP = request->getParam("pack_p", true);
        const auto packMah = request->getParam("pack_mah", true);
        const uint32_t cellType =
            packCell == nullptr ? 0 : packCell->value().toInt();
        const uint32_t series = packS == nullptr ? 0 : packS->value().toInt();
        const uint32_t parallel = packP == nullptr ? 0 : packP->value().toInt();
        const uint32_t customMah =
            packMah == nullptr ? 0 : packMah->value().toInt();
        if (!battery_pack::validSettings(cellType, series, parallel,
                                         customMah)) {
          request->send(400, "text/html",
                        "Battery pack: series must be 1-30, parallel 1-20 and "
                        "a custom cell 500-10000 mAh.");
          return;
        }
        Settings->pack_cell_type = cellType;
        Settings->pack_series = cellType == 0 ? 0 : series;
        Settings->pack_parallel = cellType == 0 ? 0 : parallel;
        Settings->pack_custom_cell_mah =
            cellType == battery_pack::CELL_CUSTOM ? customMah : 0;
        Settings->wifi_power = wifiPower->value().toInt();
        // Unchecked checkboxes aren't sent in the form post, so a missing
        // param means "off".
        const bool stockRegen =
            request->getParam("stock_regen", true) != nullptr;
        Settings->pass_through_current = stockRegen;
        Settings->pass_through_status = stockRegen;
        // Owie's own battery percentage is always used; the old "stock
        // battery percentage" option was removed.
        Settings->pass_through_soc = false;
        snprintf(Settings->ap_self_password, sizeof(Settings->ap_self_password),
                 "%s", apSelfPassword->value().c_str());
        snprintf(Settings->ap_self_name, sizeof(Settings->ap_self_name), "%s",
                 apSelfName->value().c_str());
        saveSettingsAndRestartSoon();
        request->send(200, "text/html", "Settings saved, restarting...");
        return;
    }
    request->send(404);
  });
  webServer.on("/lock", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("unlock")) {
      Settings->is_locked = false;
      saveSettings();
      request->send(200, "text/html", "Board unlocked, restart your board.");
      return;
    } else if (request->hasParam("toggleArm")) {
      if (!lockingPreconditionsMet()) {
        request->send(500, "text/html",
                      "Cannot enable locking with empty WiFi password");
        return;
      }
      Settings->locking_enabled = !Settings->locking_enabled;
      Settings->is_locked = false;
      saveSettings();
      request->send(200, "text/html", Settings->locking_enabled ? "1" : "");
      return;
    }
    request->send(404);
  });

  webServer.begin();
}

void streamBMSPacket(uint8_t *const data, size_t len) {
  ws.binaryAll((char *const)data, len);
}
