# Camper Van HMI — cloud command vocabulary

What the panel accepts over the Hyperwisor Secure Channel, taken from
`components/app/cloud/hyperwisor_app.c`. Use it to configure the product's
commands in the manufacturer dashboard and to build the app's device screen.

Product id: `1f577a41-080e-4f77-8162-30df2c5e82e9`

Every message has the same envelope. `targetId` is the device; the panel replies
to whoever sent it.

```json
{
  "targetId": "<device uuid>",
  "payload": {
    "commands": [
      { "command": "relay_control",
        "actions": [ { "action": "update", "params": { "relay": "1", "state": "true" } } ] }
    ]
  }
}
```

## The six commands

| command | actions | what it does |
| --- | --- | --- |
| `relay_control` | any (ignored) | User relays, coils **0–7** |
| `coil_control` | any (ignored) | Any coil **0–15**, including HVAC-owned ones |
| `hvac_control` | `power`, `auto`, `fan_level`, `set_temp` | Climate |
| `rgb_control` | `roof`, `floor` | LED strip zones |
| `get_status` | any (ignored) | Returns the full state |
| `config` | any (ignored) | Tells the panel which dashboard widgets to push to |

`SYSTEM`, `DEVICE_STATUS` and `OTA_UPDATE` are handled by the library.

---

## relay_control

Coils **0–7** only. The action name is not read — use `update`.

```json
{ "command": "relay_control",
  "actions": [ { "action": "update", "params": { "relay": "1", "state": "true" } } ] }
```

| param | accepts |
| --- | --- |
| `relay` *or* `coil` | number or numeric string. `relay` wins if both are present |
| `state` *or* `on` | bool, number, or `"true"`/`"false"`/`"on"`/`"off"`/`"1"`/`"0"` (case-insensitive). `state` wins |

Several actions in one message are applied in order, so a whole scene is one
command.

Coils **8–15 are refused** here, with `status: "rejected"` and
`reason: "hvac_reserved_or_out_of_range"` — the HVAC controller owns them and
two writers would fight. Use `coil_control` if you really mean to.

### Which coil is what

| coil | panel label | notes |
| --- | --- | --- |
| 0–7 | Reading Light 1–8 | the user relays |
| 8 | HVAC fan LOW | HVAC-owned |
| 9 | HVAC fan MED | HVAC-owned |
| 10 | HVAC fan HIGH | HVAC-owned |
| 11 | *(spare)* | fan MAX if the blower has 4 speeds |
| 12 | HVAC clutch | HVAC-owned |
| 13–14 | Switch 6–7 | free |
| 15 | Star roof | driven by the star-roof control |

Panel tiles "Switch 1–7" are coils 8–14, so the ones that overlap HVAC are
reserved. "Switch 8" is deliberately unmapped.

---

## coil_control

Same params as `relay_control`, but the range is **0–15** and HVAC coils are
**not** protected.

```json
{ "command": "coil_control",
  "actions": [ { "action": "update", "params": { "coil": "15", "state": "true" } } ] }
```

Out of range is refused with `reason: "out_of_range"`; a missing or
unparseable state with `reason: "missing_state"`.

> Driving coils 8–12 while HVAC is on means the controller will overwrite you
> on its next tick. Treat this command as a service tool, not a user control.

---

## hvac_control

Four named actions. **The action name matters here**, unlike the relay
commands.

```json
{ "command": "hvac_control",
  "actions": [
    { "action": "power",     "params": { "on": true } },
    { "action": "auto",      "params": { "on": false } },
    { "action": "fan_level", "params": { "level": 3 } },
    { "action": "set_temp",  "params": { "temp_c": 22 } }
  ] }
```

| action | param | range |
| --- | --- | --- |
| `power` | `on` | **real boolean** |
| `auto` | `on` | **real boolean** |
| `fan_level` | `level` | integer **1–5** |
| `set_temp` | `temp_c` | integer **16–30** |

> **The trap.** `power` and `auto` are read with `cJSON_IsBool`, so a string
> `"true"` is **silently ignored** — no error, no reply naming it, the action
> simply does not appear in the `applied` list. `relay_control` is relaxed
> about this and `hvac_control` is not.
>
> Dashboard widgets commonly send parameters as strings. If a toggle wired to
> `hvac_control` does nothing, this is why: configure it to send a true
> boolean, or wire it to `relay_control` instead.

Values outside the ranges above are dropped the same silent way. The reply
lists what was actually applied:

```json
{ "status": "ok", "applied": ["power", "fan_level"] }
```

Check `applied` rather than `status` — `status` is `"ok"` even when every
action was ignored.

---

## rgb_control

The **action name is the zone**: `roof` or `floor`. Anything else is ignored.

```json
{ "command": "rgb_control",
  "actions": [ { "action": "roof", "params": { "hex": "#FF8800" } } ] }
```

Colour is read in this order, first match wins:

1. nested — `"rgb": { "r": 255, "g": 136, "b": 0 }`
2. flat — `"r": 255, "g": 136, "b": 0`
3. hex string — `"hex"` or `"color"`, as `#rrggbb`

Values clamp to 0–255.

**On/off is implied by the colour.** A non-black colour turns the zone on;
`r=g=b=0` turns it off. There is no separate power param.

Optionally send HSV alongside so the panel's own colour wheel and brightness
slider come up matching what the app set — otherwise the panel converts from
RGB, which round-trips imperfectly:

```json
{ "action": "floor",
  "params": { "hex": "#FF8800", "hsv": { "h": 32, "s": 100, "v": 80 } } }
```

`h` wraps to 0–359, `s` and `v` clamp to 0–100. Flat `h`/`s`/`v` also work.
When the colour is black the panel keeps the previous hue and saturation, so
the wheel does not snap back to red.

---

## get_status

Any action. Returns everything in one reply:

```json
{
  "relays": [ { "relay": 0, "state": false }, … ],
  "bmp280": { "valid": true, "temp_c": 21.4, "hpa": 1013.2 },
  "welcome": "Mayur",
  "hvac": {
    "power": true, "auto": false,
    "fan_level": 3, "fan_step": 2,
    "clutch": true,
    "cabin_valid": true, "cabin_c": 22.8, "delta_c": 0.8,
    "target_c": 22, "mode": "cooling"
  }
}
```

- `relays` covers coils 0–7 only.
- `bmp280.valid` false means the sensor did not answer; `temp_c` and `hpa` are
  then absent, not zero.
- `fan_level` is what was asked for, `fan_step` is what the controller is
  actually driving. They differ while it ramps.
- `cabin_c` and `delta_c` are absent when `cabin_valid` is false.

---

## config

Tells the panel which dashboard **widget ids** to push live values into, so the
panel updates the dashboard without the dashboard polling. Persisted in NVS.

### The welcome name

`config` also carries the greeting shown on the panel's Overview tab:

```json
{ "command": "config",
  "actions": [ { "action": "set", "params": { "welcome": "Mayur" } } ] }
```

`owner` is accepted as a synonym. Up to **32 characters**
(`OWNER_NAME_MAX`); the panel persists it itself under NVS `cfg`/`owner` and
re-fits its cursive font to the new length, so it repaints as the command
lands. The current value comes back in both `config`'s ack and `get_status`,
so an app can show and edit it in one round trip.

### Widget wiring

```json
{ "command": "config",
  "actions": [ { "action": "set", "params": {
    "targetid": "<dashboard target id>",
    "w_temp": "<widget id>",
    "w_hpa": "<widget id>",
    "w_hvac_mode": "<widget id>",
    "w_hvac_fan": "<widget id>",
    "w_hvac_clutch": "<widget id>",
    "w_hvac_cur": "<widget id>",
    "w_hvac_set": "<widget id>",
    "rw_0": "<widget id>", "rw_1": "<widget id>"
  } } ] }
```

`rw_0` … `rw_7` are the per-relay widgets for coils 0–7. Send only the keys you
want to change; omitted keys keep their stored value. The reply echoes the
stored `targetid`.

---

## Setting this up in the manufacturer dashboard

Define six commands with these names. The actions that matter are
`hvac_control`'s four and `rgb_control`'s two; the rest can use a single
`update` action, since the panel ignores the name.

Two things that will cost you time if missed:

1. **`hvac_control` needs real booleans.** A widget sending `"true"` is
   silently dropped. Verify through `applied` in the reply, not `status`.
2. **Coils 8–12 are HVAC-owned.** `relay_control` refuses them on purpose.
   Build user-facing switches on coils 0–7, 13 and 14.
