Jumping Jack (WASM + macOS)
===========================

.. image:: https://github.com/dtz-labs/wasm-jumping-jack/actions/workflows/build.yml/badge.svg
   :target: https://github.com/dtz-labs/wasm-jumping-jack/actions/workflows/build.yml
   :alt: Build

A tribute to the 1983 ZX Spectrum classic *Jumping Jack*.

**▶ Play in the browser:** https://dtz-labs.github.io/wasm-jumping-jack/

One game core in freestanding C (``src/game.c``) is compiled to:

- **WebAssembly**, played in the browser (``web/``), with on-screen touch controls on phones,
- a **native macOS app** (Cocoa + AudioToolbox, ``mac/main.m``).

Every push to ``main`` runs the tests, builds the WASM version and deploys it to
GitHub Pages. The macOS ``.app`` is attached to each workflow run as the
``JumpingJack-macOS`` artifact.

Requirements
------------

- macOS with Xcode Command Line Tools (Apple clang), for the native app and tests
- Homebrew ``llvm@22`` and ``lld@22``, for the WASM build (``brew install llvm@22 lld@22``)
- Python 3, for the local web server

Build & run
-----------

.. code-block:: sh

   make serve     # build WASM, serve on http://localhost:8000 and open the browser
   make run-mac   # build build/JumpingJack.app and launch it
   make test      # unit tests of the game logic
   make all       # build both targets
   make clean

Other toolchains can be selected with ``make web WASM_CC=... WASM_LD=...``.

Controls
--------

=========  ===============================
Action     Keys
=========  ===============================
Left       Z, O, ←
Right      X, P, →
Jump       Space, Q, ↑
Start      Space / Enter / tap the screen
Pause      H, Esc
Mute       M
=========  ===============================

On phones and tablets the page shows big ◀ ▶ and JUMP buttons. You can hold
a direction and jump at the same time, and the layout adapts to landscape.

Rules
-----

Jump through the gaps to climb the 8 lines. Each successful jump opens one more
gap (up to 8). Right-moving gaps wrap to the line below and left-moving gaps
wrap to the line above. Jumping into a solid line knocks Jack out for a moment,
and a gap passing under his feet drops him one floor. If he falls all the way
to the ground, he loses a life. From level 2 on, each level adds another hazard.
