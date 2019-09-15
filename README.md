<p align="center">
  <a href="https://nodejs.org/">
    <img
      alt="Node.js"
      src="https://nodejs.org/static/images/logo-light.svg"
      width="400"
    />
  </a>
</p>

This is a fork of Node.js that allows users to run code in a vanilla V8 environment. I run my JS code across the browser, server and mobile devices. Node helpers aren't supported everywhere. The only lowest common denominator is a vanilla V8 engine environment across all the above platforms. Although the wrappers for V8 across platforms (J2V8 for android and server, JSC for iOS) support running JS code, none of them have a good integration with Chrome DevTools for debugging JS code. Node's DevTools support is pretty awesome, so I tweaked Node's `--inspect --inspect-brk` option to run the code in vanilla V8 environment with complete Chrome DevTools support!


## Building Node.js

See [BUILDING.md](BUILDING.md) for instructions on how to build Node.js from
source and a list of supported platforms.

