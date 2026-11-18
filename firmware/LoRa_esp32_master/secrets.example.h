// Template for secrets.h. Copy this file to secrets.h and fill in real values.
// secrets.h is gitignored: never commit it.
// DEVICE_TOKEN must match AUTOMATION_DEVICE_TOKEN in /etc/automation/automation.env on the server.
#pragma once

#define SERVER_HOST "automation.example.com"
#define SERVER_PORT 443
#define SERVER_PATH "/ws/device"
#define DEVICE_TOKEN "replace-with-a-long-random-hex-token"   // e.g. `openssl rand -hex 32`
#define PROV_PIN "change-me"   // BLE Wi-Fi provisioning PIN the phone app asks for
