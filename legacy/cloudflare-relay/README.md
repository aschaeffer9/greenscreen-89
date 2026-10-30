# Tetris 89 link relay (Cloudflare Worker)

This little Worker lets two phones running Tetris 89 link up over the
internet. It pairs the first two players who connect and passes their link
cable traffic back and forth. Nothing is stored.

## Files

```
tetris89-link/
  wrangler.toml
  src/index.js
```

Put `index.js` inside a `src` folder next to `wrangler.toml`.

## Deploy (about 2 minutes)

You need Node.js installed. In a terminal, inside the `tetris89-link` folder:

```
npx wrangler login
npx wrangler deploy
```

The first command opens your browser to sign in to Cloudflare. The second one
uploads the Worker and prints its address, something like:

```
https://tetris89-link.<your-subdomain>.workers.dev
```

Open that address in a browser: it should say
"Tetris 89 link relay is running."

## Point the app at it

1. Copy the address above on your phone (any form works, with or without https://).
2. In Tetris 89, tap **2PLAYER** on the title screen, then **SET SERVER**.
   The app reads the address from the clipboard and remembers it.
3. Both players tap **2PLAYER > ONLINE**. The first one waits; when the
   second connects you're linked. Then either player taps 2PLAYER again to
   start, just like plugging in the cable.

The other player's phone needs the same server address (send it to them and
have them do step 2).

## Free plan

Durable Objects are included on the free plan. Each link packet counts toward
the daily request allowance (incoming WebSocket messages count 20 to 1), which
works out to several hours of play per day. If you ever hit the limit, the
link just drops until the next day.
