# tensionLEDMQTT

The board holds one MQTT
connection open to a broker and gets new patterns pushed to it instead of
downloading a JSON file from Firebase Storage every 5 seconds.

```
web app ──publish (wss, 8884)──▶ broker ──push (mqtts, 8883)──▶ Feather M0
```

Both ends connect *outbound*, so there's no port forwarding, no dynamic DNS,
and nothing listening on the home network — and it still works from anywhere.

## Setup

### 1. Broker

[HiveMQ Cloud](https://www.hivemq.com/mqtt-cloud-broker/) has a free tier
that's plenty for this. Create a cluster, then under *Access Management* make
two credentials:

- `wall` — subscribe only, topic `wall/pattern`
- `webapp` — publish only, topic `wall/pattern`

Note the hostname. TLS-only: port **8883** for MQTT, **8884** for WebSocket.

### 2. WINC1500 firmware and root certificate

**This is the step that will otherwise silently fail.** The WINC1500 does TLS
in the module and only trusts certificates you flash into it.

1. Load a blank sketch (or `../hello-world_blink`) to free the module.
2. Arduino IDE → *Tools → WiFi101 / WiFiNINA Firmware Updater*.
3. Section 1: check firmware. Needs **19.6.1 or newer** for TLS 1.2 —
   HiveMQ Cloud won't accept anything older. `../CheckWifi101FirmwareVersion`
   reports the current version too.
4. Section 3: *Add domain* → your broker hostname → **Upload Certificates to
   WiFi module**.

Uploading replaces the module's entire certificate set, and it holds roughly
ten, so add every domain you need in one pass.

### 3. Libraries

Library Manager: **WiFi101**, **ArduinoMqttClient**, **Adafruit NeoPixel**.

### 4. Credentials

Fill in the broker fields in `arduino_secrets.h`. WiFi is already carried over
from the old sketch.

### 5. Flash and verify

Serial monitor at 9600 should show:

```
tensionLEDMQTT
wifi: connecting to NETGEAR82
wifi: ok, ip 192.168.1.x
mqtt: connected
```

The onboard LED blips once every 2 s when connected, and blinks fast when not.

### 6. Test before touching the web app

```bash
cd publisher && npm install && cp .env.example .env
```

Fill in `.env`, then:

```bash
npm run demo
```

Every 10th LED lights, cycling magenta/blue/green/red — that confirms
addressing across the full strip. `npm run clear` turns everything off.

## Wiring in the web app

Whatever currently builds the `{ "0": "magenta", ... }` object and uploads it to
Firebase Storage instead does:

```js
import { connect, publishPattern } from './wall-publish.js';

const wall = connect({
  url: 'wss://YOUR-CLUSTER.s1.eu.hivemq.cloud:8884/mqtt',
  username: 'webapp',
  password: '...',
});

await publishPattern(wall, colorsByIndex);   // same object as before
```

`encodePattern()` does the conversion, so the app's existing data structure
doesn't have to change. Firebase Storage and its download token can go away —
worth revoking that token once this is running.

`onWallStatus()` subscribes to `wall/status`, which the board sets to `online`
on connect and which its MQTT last will sets to `offline` if it drops — useful
for an indicator in the UI.

**One caveat:** browser MQTT credentials are visible to anyone who loads the
page. Hence the publish-only `webapp` user restricted to one topic — worst case
someone changes the lights. If the app is public and that's not acceptable,
publish from a small serverless function instead and keep the credentials there.

## Plan B: Mosquitto on the LAN

If the WINC1500's TLS stack gives trouble — it's old, and the certificate step
is fiddly — run [Mosquitto](https://mosquitto.org/) on a Raspberry Pi instead:

- Board → Mosquitto over plain MQTT on the LAN. No certificates at all. Change
  `WiFiSSLClient net;` to `WiFiClient net;` and set `SECRET_MQTT_PORT` to 1883.
- Web app → Mosquitto through a [Cloudflare Tunnel](https://developers.cloudflare.com/cloudflare-one/connections/connect-networks/)
  or [Tailscale](https://tailscale.com/) on the same Pi.

TLS then terminates on the Pi, which is much better at it than the WINC1500,
and still no ports forwarded. Everything else in the sketch stays the same.

## Protocol

Topic `wall/pattern`, 250 ASCII bytes, one per LED, index 0 first:

```
'.' off    'm' magenta    'b' blue    'g' green    'r' red
```

Publish with `retain: true, qos: 1`. Retain is what lets a rebooted board pick
up the current problem immediately — don't drop it, and don't publish an empty
payload, which would delete the retained message and leave a rebooting board
dark. `encodePattern()` always returns the full 250 bytes for that reason.

## Troubleshooting

| Symptom | Cause |
|---|---|
| `mqtt: failed, error -2` | TLS failed — root certificate not flashed for that hostname, or firmware older than 19.6.1 |
| `mqtt: failed, error 5` | bad username/password, or the ACL doesn't allow that topic |
| `mqtt: failed, error -1` | hostname didn't resolve, or port blocked |
| Connects, no LEDs change | nothing retained yet — run `npm run demo` |
| `beginWill` won't compile | update ArduinoMqttClient, or delete the three `Will` lines in `connectBroker()` |
| Board reboots every 10 min | it can't stay connected; see `GIVE_UP_MS` |
| First ~50 LEDs work, rest don't | power, not code — 250 NeoPixels at brightness 150 draw several amps; inject power at both ends |
