#!/usr/bin/env node
//
// Bring-up tool: push a pattern to the wall from the command line, so you can
// confirm the board is subscribed and rendering before touching the web app.
//
//   cp .env.example .env     # fill in broker details
//   npm install
//   npm run demo             # every 10th LED, colours cycling - checks addressing
//   npm run clear            # all off
//   node test-publish.js 3:magenta 17:blue 45:green 60:red
//   node test-publish.js all:blue          # every LED - use to check your PSU
//
// Reads MQTT_URL / MQTT_USER / MQTT_PASS from the environment.

import { readFileSync } from 'node:fs';
import { connect, publishPattern, encodePattern, LED_COUNT } from './wall-publish.js';

// Minimal .env loader. node --env-file needs Node 20.6+, and this has to run
// on 18.x too; not worth a dependency.
try {
  for (const line of readFileSync(new URL('.env', import.meta.url), 'utf8').split('\n')) {
    const match = line.match(/^\s*([A-Z_][A-Z0-9_]*)\s*=\s*(.*?)\s*$/i);
    if (match && !process.env[match[1]]) {
      process.env[match[1]] = match[2].replace(/^["']|["']$/g, '');
    }
  }
} catch {
  // no .env - fall back to whatever is already in the environment
}

const url = process.env.MQTT_URL;
const username = process.env.MQTT_USER;
const password = process.env.MQTT_PASS;

if (!url) {
  console.error('MQTT_URL is not set. Example: mqtts://abcdef.s1.eu.hivemq.cloud:8883');
  process.exit(1);
}

const args = process.argv.slice(2);
const colors = {};

if (args.length === 0 || args[0] === 'demo') {
  const cycle = ['magenta', 'blue', 'green', 'red'];
  for (let i = 0; i < LED_COUNT; i += 10) {
    colors[i] = cycle[(i / 10) % cycle.length];
  }
} else if (args[0] === 'clear') {
  // leave colors empty
} else if (args[0].startsWith('all:')) {
  const name = args[0].slice(4);
  for (let i = 0; i < LED_COUNT; i++) colors[i] = name;
} else {
  for (const arg of args) {
    const [index, name] = arg.split(':');
    if (name === undefined) {
      console.error(`Bad argument "${arg}". Expected <ledIndex>:<color>, e.g. 17:blue`);
      process.exit(1);
    }
    colors[index] = name;
  }
}

const payload = encodePattern(colors);
const lit = payload.length - (payload.match(/\./g)?.length ?? 0);
console.log(`payload (${payload.length} bytes, ${lit} lit):`);
console.log(payload.replace(/(.{50})/g, '$1\n'));

const client = connect({ url, username, password, clientId: 'wall-test-publish' });

client.on('error', (err) => {
  console.error('mqtt error:', err.message);
  process.exit(1);
});

client.on('connect', async () => {
  await publishPattern(client, colors);
  console.log('published to wall/pattern (retained)');
  client.end();
});
