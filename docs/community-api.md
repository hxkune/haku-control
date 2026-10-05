# Community presets: the site's API

haku control's *Community* tab (Effects → Community) lists presets people published, adds them with one click and
publishes the user's own. The app talks to one PHP endpoint on hakune.blog. This file is what the endpoint has to
do; `tools/sim/fake_community.py` is a working stand-in to compare against.

A preset is a look only: effect, colours, speed, brightness. Nothing about anyone's devices or PC.

## Endpoint

`https://hakune.blog/api/presets.php` (the app can be pointed elsewhere with `[community] api=` in its settings).

- Every answer is JSON (`Content-Type: application/json; charset=utf-8`) and carries
  `Access-Control-Allow-Origin: *` (the app's window fetches it).
- The app sends POST bodies as JSON **with `Content-Type: text/plain`** (a "simple" request: no CORS preflight).
  Read them with `json_decode(file_get_contents('php://input'), true)`. Answer `OPTIONS` with 204 anyway, with
  `Access-Control-Allow-Methods: GET, POST` and `Access-Control-Allow-Headers: Content-Type`.
- Errors: HTTP 200 with `{"error": "<code>"}`; codes below.

## A preset as the list gives it

```json
{
  "id": "k3f9a1",            // short, unique, never reused
  "name": "Evening",         // 1..40 characters
  "author": "haku",          // 1..24 characters
  "effect": "lava",          // an effect id: [a-z0-9_]{1,24}
  "palette": ["#FF3300", "#FF8800", "#CC1100"],   // 0..8 colours, #RRGGBB upper case
  "speed": 3,                // 1..10
  "bri": 60,                 // 0 = keep the user's brightness, else 5..100
  "dl": 12,                  // times added
  "likes": 4,
  "liked": false,            // by the install in the request (false without one)
  "ts": 1790000000           // published (approved), unix seconds
}
```

## GET: the list

`GET presets.php?sort=top|new&q=<text>&effect=<id>&page=<n>&install=<id>`

- `sort=top`: by likes, then downloads, then newest. `sort=new`: newest first. Default `top`.
- `q`: case-insensitive search in name and author. `effect`: only that effect. Both optional.
- `page`: 0-based, **24 per page**. `install`: fills `liked`.
- Only approved presets.

```json
{ "items": [ { ...preset... } ], "more": true }
```

## POST: publish

```json
{ "action": "publish", "name": "Evening", "author": "haku", "effect": "lava",
  "palette": ["#FF3300", "#FF8800", "#CC1100"], "speed": 3, "bri": 60,
  "install": "5c2e…", "version": "0.4.0" }
```

- Validate every field as above (trim; reject control characters; colours `#RRGGBB`, upper-cased). Bad: `{"error":"invalid"}`.
- Rate limit: at most 5 per install and 10 per IP (keep only a hash of it) per 24 hours: `{"error":"rate"}`.
- Stored as **pending**; it shows in the list only once approved: `{"ok": true, "status": "pending"}`.

## POST: download

`{ "action": "download", "id": "k3f9a1", "install": "5c2e…" }` → `{"ok": true, "dl": 13}`

Counts once per install and preset (later ones answer the same count). Unknown id: `{"error":"unknown"}`.

## POST: like

`{ "action": "like", "id": "k3f9a1", "install": "5c2e…" }` → `{"ok": true, "likes": 5, "liked": true}`

A toggle per install and preset: liked → unliked and back.

## Moderation (the site's admin)

A page in the existing admin: the pending presets (name, author, effect, colours, when, a count of what that install
published before), **Approve** / **Reject**, and for approved ones **Remove**. Approving sets `ts` to now.

## Storage (a suggestion)

A table `presets(id, name, author, effect, palette, speed, bri, dl, likes, status, ts, created, install_hash, ip_hash)`
and `marks(preset_id, install_hash, kind)` with `kind` = `dl` / `like`, unique per (preset, install, kind). Installs
and IPs only as salted hashes. Or JSON files, if MySQL is not wanted.

## Privacy

The app sends the install id (a random id made on first use, nothing about the PC or the person) with publish,
download and like, and the author name the user typed when publishing. The site keeps hashes of installs and IPs only.
