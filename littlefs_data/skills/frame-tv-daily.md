# Frame TV — daily art rotation

Generate one fresh AI image every morning and push it to the Samsung Frame TV
in the living room. Turns the Frame from a one-off demo into ambient
decoration that subtly reflects the day's weather and season.

## Schedule

- **Frequency:** Daily, cron id `tvart001` at **06:00 PDT** (just before the
  morning briefing so the art is already up when James wakes).
- **Channel:** system (no notification — silent success).

## Steps

1. `get_weather` for San Francisco — capture: conditions (clear/foggy/rain),
   temperature, time of year context (the tool returns date implicitly).

2. Build a **mood prompt** using:
   - The weather conditions (foggy → soft cool palette; clear → warm light;
     rain → moody tones).
   - The month (May → late spring greens / Pacific bloom; June → coastal
     summer; September → IFA / autumn warmth).
   - A rotating **style seed** chosen by `current_day_of_week % 7`:
     - 0 Sun → "impressionist oil painting"
     - 1 Mon → "minimalist ink wash"
     - 2 Tue → "moody photographic landscape"
     - 3 Wed → "watercolor sketch"
     - 4 Thu → "art-deco poster"
     - 5 Fri → "soft pastel illustration"
     - 6 Sat → "Japanese woodblock print"
   - A rotating **subject seed** picked from a small loop so adjacent days
     don't repeat:
     - Pacific coastline / dunes
     - California redwoods at golden hour
     - Mount Tamalpais ridgeline
     - San Francisco Bay from the Marin headlands
     - Pacifica Linda Mar beach
     - Inland valley vineyards
     - Sierra foothills

3. Compose the final visual prompt as a single sentence under 280 chars.
   Example: `Misty Pacifica Linda Mar at dawn, soft cool palette, impressionist oil painting, low contrast horizon, no people, no text.`

4. Call `frame_tv` with that prompt. The tool takes ~30-60 s to generate +
   transfer; no need to wait/confirm — fire-and-forget.

5. Append a one-line note to today's daily journal:
   `frame_tv: <one-word subject>, <one-word style>` (e.g. `frame_tv: pacifica, oil`).
   Skip if it fails — don't retry, just log the error.

## Guardrails

- **Always include "no people, no text, no logo" in the prompt** — avoids the
  Frame TV showing AI-rendered humans (uncanny valley) or scribbled fake text.
- **Stay terrestrial / nature-focused** — abstract / sci-fi / fantasy doesn't
  read well as ambient living-room art. Coast, hills, valleys, weather.
- If `get_weather` fails: still proceed with a neutral "foggy coastal morning"
  default. The point is daily refresh, not perfect alignment.
- Max 2 tool calls total (`get_weather` + `frame_tv`).
- Silent on success. James notices the image change on the TV — no need for
  a Telegram or email ping.

## Manual invocation

If James says "new art on the frame" / "change the frame TV" / "give me
something new on the wall", run this skill immediately regardless of time.
On manual runs, reply via the originating channel with the prompt used so
James can see what was generated.
