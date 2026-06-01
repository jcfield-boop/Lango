# Home Assistant Control

Control lights, switches, climate, and other devices via the ha_request tool.

## When to use
- User asks to turn on/off lights, set brightness, change temperature
- User asks "what's the temperature?", "is the door locked?", "are lights on?"
- User says "turn off everything" or "all lights off"

## Discovery — finding entity IDs

**Important:** You don't know entity IDs in advance. Use these discovery steps:

1. **List entities in a domain** (e.g. all lights):
   - `ha_request` with `GET /api/states` is BLOCKED (too large)
   - Instead, call `ha_request` with `POST /api/services/light/turn_on` and `body: {"entity_id": "all"}` — this turns on ALL lights in one call
   - To list specific entities, use `GET /api/states/light.living_room` (guess common names)

2. **Common entity ID patterns:**
   - Lights: `light.living_room`, `light.bedroom`, `light.kitchen`, `light.office`, `light.hallway`
   - Switches: `switch.living_room`, `switch.bedroom`
   - Climate: `climate.living_room`, `climate.thermostat`
   - Sensors: `sensor.temperature_living_room`, `sensor.humidity_bedroom`
   - Binary: `binary_sensor.front_door`, `binary_sensor.motion_kitchen`

## Bulk actions — controlling all lights

To turn ALL lights on or off, use the service call:
- **All lights ON:** `POST /api/services/light/turn_on` with body `{"entity_id": "all"}`
- **All lights OFF:** `POST /api/services/light/turn_off` with body `{"entity_id": "all"}`

To control a specific area's lights:
- `POST /api/services/light/turn_off` with body `{"entity_id": "light.living_room, light.kitchen"}`

## Common service calls

### Lights
- Turn on: `POST /api/services/light/turn_on` body: `{"entity_id": "light.X"}`
- Turn on with brightness: body: `{"entity_id": "light.X", "brightness": 128}` (0-255)
- Turn on with color: body: `{"entity_id": "light.X", "rgb_color": [255, 0, 0]}`
- Turn off: `POST /api/services/light/turn_off` body: `{"entity_id": "light.X"}`
- Toggle: `POST /api/services/light/toggle` body: `{"entity_id": "light.X"}`

### Switches
- Turn on: `POST /api/services/switch/turn_on` body: `{"entity_id": "switch.X"}`
- Turn off: `POST /api/services/switch/turn_off` body: `{"entity_id": "switch.X"}`

### Climate
- Set temp: `POST /api/services/climate/set_temperature` body: `{"entity_id": "climate.X", "temperature": 72}`
- Set mode: `POST /api/services/climate/set_hvac_mode` body: `{"entity_id": "climate.X", "hvac_mode": "heat"}`

### Check state
- `GET /api/states/light.living_room` — returns state ("on"/"off") + attributes (brightness, color, etc.)
- `GET /api/states/sensor.temperature_living_room` — returns state ("72.5") + unit

## Error handling
- If entity not found (404): try alternative naming (e.g. `light.lounge` instead of `light.living_room`)
- If service call fails: check entity_id spelling; the domain prefix must match the service domain
