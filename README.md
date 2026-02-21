# Rigoberto ESP32-S3-BOX3 Voice Pipeline (Path-1)

Wake word + cloud assistant loop on ESP32-S3-BOX3:

1. **WakeNet** listens for built-in wake word **"Hi ESP"**
2. Captures post-wake speech audio (PCM)
3. Sends WAV to **Whisper STT** endpoint
4. Sends transcribed text to **OpenClaw assistant** endpoint
5. Sends assistant reply text to **TTS** endpoint
6. Plays WAV reply on speaker
7. Drives avatar mouth/talk animation during playback

Also exposes a minimal avatar API at `:8080` (`/v1/state`, `/v1/perform`).

## Requirements

- ESP-IDF v5.5.x
- ESP32-S3-BOX3 connected on `/dev/ttyACM0`
- Network access from board to your cloud endpoints
- Endpoint contracts:
  - STT: `POST audio/wav` -> JSON `{ "text": "..." }`
  - Assistant: `POST application/json {"text":"..."}` -> JSON `{ "reply": "..." }` (or `text`)
  - TTS: `POST application/json {"text":"..."}` -> raw WAV bytes (PCM16)

## Config

Do **not** put secrets in tracked files.

- Safe defaults: `sdkconfig.defaults`
- Local secrets (gitignored): `sdkconfig.defaults.local`

Example local file:

```ini
CONFIG_RIGO_WIFI_STA_SSID="YOUR_WIFI_SSID"
CONFIG_RIGO_WIFI_STA_PASSWORD="YOUR_WIFI_PASSWORD"
CONFIG_RIGO_STT_URL="https://your-api.example.com/stt"
CONFIG_RIGO_ASSISTANT_URL="https://your-api.example.com/assistant"
CONFIG_RIGO_TTS_URL="https://your-api.example.com/tts"
CONFIG_RIGO_API_BEARER="YOUR_BEARER_TOKEN"
```

## Build / Flash / Monitor

```bash
cd /home/paisa/projects/rigoberto-esp32
source /home/paisa/projects/esp-idf-v5.5/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

## Checkpoints

### Checkpoint A: Build passes

```bash
idf.py build
```

Expected: successful build and generated `build/hello_world.bin`.

### Checkpoint B: Device boots and WakeNet starts

On serial monitor, look for logs similar to:

- `Rigo voice pipeline ready`
- `WakeNet ready (...). Say: Hi ESP`

### Checkpoint C: Avatar API reachable

From host (replace IP with STA IP from serial logs):

```bash
curl -s http://<BOX3_IP>:8080/v1/state
curl -s -X POST http://<BOX3_IP>:8080/v1/perform \
  -H 'content-type: application/json' \
  -d '{"emotion":"happy","talk":true,"duration_ms":1200}'
```

### Checkpoint D: End-to-end voice loop

1. Say: **"Hi ESP"**
2. Speak a short phrase after wake (capture window default 3.5s)
3. Confirm serial logs show STT + Assistant text
4. Confirm spoken TTS reply and avatar mouth animation

## Notes

- Wake model enabled by default: `CONFIG_SR_WN_WN9_HIESP=y`
- Capture window is configurable via `CONFIG_RIGO_CAPTURE_MS`
- Keep endpoint adapters simple; this firmware expects the response shapes above.
