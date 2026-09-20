// Publisher side of tensionLEDMQTT. Framework-agnostic ES module - works in
// the browser over MQTT-over-WebSocket, and in Node over plain MQTT/TLS.
//
//   import { connect, publishPattern } from './wall-publish.js';
//
//   const wall = connect({
//     url: 'wss://abcdef.s1.eu.hivemq.cloud:8884/mqtt',
//     username: 'webapp',
//     password: '...',
//   });
//
//   publishPattern(wall, { 3: 'magenta', 17: 'blue', 45: 'green' });
//
// Install: npm install mqtt

import mqtt from 'mqtt';

export const LED_COUNT = 250;
export const TOPIC_PATTERN = 'wall/pattern';
export const TOPIC_STATUS = 'wall/status';

// Must match colorFor() in tensionLEDMQTT.ino.
const CODES = {
  magenta: 'm',
  blue: 'b',
  green: 'g',
  red: 'r',
};

const OFF = '.';

/**
 * Turn the colour map the web app already builds - { "0": "magenta", ... },
 * keyed by LED index - into the fixed-width string the board expects.
 *
 * Accepts a plain object, an array, or a Map. Unknown colour names and
 * out-of-range indexes are ignored rather than throwing, so one bad hold can't
 * take down the whole wall.
 */
export function encodePattern(colorsByIndex, ledCount = LED_COUNT) {
  const out = new Array(ledCount).fill(OFF);

  const entries =
    colorsByIndex instanceof Map
      ? colorsByIndex.entries()
      : Object.entries(colorsByIndex ?? {});

  for (const [key, name] of entries) {
    const i = Number(key);
    const code = CODES[String(name).toLowerCase()];
    if (Number.isInteger(i) && i >= 0 && i < ledCount && code) {
      out[i] = code;
    }
  }

  // Fixed 250 bytes, deliberately not trimmed: an empty payload published with
  // retain = true would delete the retained message instead of blanking the
  // wall, and a rebooting board would come up dark.
  return out.join('');
}

/** Inverse of encodePattern - handy for tests and for reading wall state back. */
export function decodePattern(payload) {
  const names = Object.fromEntries(Object.entries(CODES).map(([n, c]) => [c, n]));
  const result = {};
  for (let i = 0; i < payload.length; i++) {
    const name = names[payload[i]];
    if (name) result[i] = name;
  }
  return result;
}

export function connect({ url, username, password, clientId }) {
  return mqtt.connect(url, {
    username,
    password,
    clientId: clientId ?? `wall-publisher-${Math.random().toString(16).slice(2, 10)}`,
    clean: true,
    reconnectPeriod: 2000,
  });
}

/**
 * Push a pattern to the wall. retain = true means the broker keeps it, so the
 * board picks up the current problem on boot without any polling.
 */
export function publishPattern(client, colorsByIndex) {
  const payload = encodePattern(colorsByIndex);
  return new Promise((resolve, reject) => {
    client.publish(TOPIC_PATTERN, payload, { retain: true, qos: 1 }, (err) =>
      err ? reject(err) : resolve(payload),
    );
  });
}

export function clearWall(client) {
  return publishPattern(client, {});
}

/** Subscribe to the board's online/offline state (set by its MQTT last will). */
export function onWallStatus(client, handler) {
  client.subscribe(TOPIC_STATUS, { qos: 1 });
  client.on('message', (topic, payload) => {
    if (topic === TOPIC_STATUS) handler(payload.toString());
  });
}
