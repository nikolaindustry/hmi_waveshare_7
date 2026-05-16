# hello_hyperwisor

Smallest possible Hyperwisor IoT app: WiFi + WebSocket + one custom command.

## Build

```sh
idf.py set-target esp32s3
idf.py build flash monitor
```

On first boot the device starts in SoftAP mode
(`NIKOLAINDUSTRY_Setup-XXXX`, password `0123456789`). Connect a phone,
provision WiFi, then it joins the cloud automatically.

## Try the BLINK command

From the Hyperwisor dashboard, send to the device id:

```json
{
  "commands": [
    { "command": "BLINK", "on": true }
  ]
}
```

GPIO 2 toggles. Replies are routed back to the sender.
