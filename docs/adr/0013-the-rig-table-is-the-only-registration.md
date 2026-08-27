# ADR 0013 — the rig table is the only registration

Status: accepted (v3, the extension layer)
Supersedes nothing. Depends on ADR 0002 (the sink boundary), D7 (`iris_source.h`).

## Context

The PI's directive reframes the project: **the students design the sensors and
the interactions.** We supply the platform. Extensibility is therefore not a
feature of this system, it is the product. The measure is:

> a student must be able to add a sensor, an output or a feature in ONE SMALL
> FILE without touching the core, the UI, or anything else — and get a clear
> error if they get it wrong.

`iris_source.h` already made a sensor one file. What it did not answer is
where the *wiring* lives: the instance, the bus, the channel-to-input mapping,
the feature count, the model shape, the arena size. In v2 all of that was
spread across `hot.c`, `app.h`, `field.c` and `surface.c`, hard-coded to two
inputs that happened to be the pixels of the touch panel.

## Decision

**`app/sense/rig.h` is the single registration site, and it is three X-macro
tables.** Everything else — ids, counts, per-source storage, per-slot extractor
state, `EWA_NI`/`NO`/`NH`/`CAP`, the arena size, the save file's schema block,
the console dump — is derived from those tables by the preprocessor.

```c
#define EWX_SOURCE_LIST(X)  X(imu, src_mpu6050, MPU_N, EWX_I2C(0, 0x68))
#define EWX_INPUT_LIST(X)   X(tilt, imu, MPU_AX, 0.5f, EWX_SMOOTH(0.15f))
#define EWX_OUTPUT_LIST(X)  X(bright, 20)
```

No code generator, no build-system magic, no registry object, no init order.
C99 and a text editor.

Three consequences we chose deliberately:

1. **The instance is declared by the TABLE, not by the source file.** Two of
   the same part is two table rows and nothing else; a source file holds no
   state of its own, so copying one to make another needs no renaming.

2. **`sense/` is platform-free.** A source reaches hardware only through
   `ewx_bus` (four function pointers), so `host/fakebus.c` can replay a
   register transcript at a student's driver and gate it on their laptop.
   `ewx_plat_bind()` — one function, in `device/iris_app/bus_esp32.cpp` — is
   the entire platform surface for sensors.

3. **The rig is found on the include path** (`#include <rig.h>`, angle
   brackets), so swapping rigs is a `-I` and not a code change. That is how
   `host/testrig/rig.h` gates the table mechanism with four sources while the
   shipped rig has one.

## The error seam

Integers and identities are checked at COMPILE TIME, through a bit-field of
negative width whose tag names the mistake:

```
error: bit-field 'ewx_channel_belongs_to_a_different_sensor'
       has negative width (-1)
```

C99 requires an *integer* constant expression for a bit-field width, so this
is exactly the set that can live there: names, channel ownership, table sizes,
window lengths, the arena budget. Floats and hardware are checked at BOOT and
named on the glass: a default outside [0,1], a part not fitted, a port that
lies about its own width, two windowed stages on one slot.

`app/host/rig_errors.sh` compiles one deliberately-broken rig per mistake and
asserts the compiler refuses it WITH THE RIGHT TAG. The compiler is part of
the user interface, so it is gated like one.

## Consequences

* Adding a 6-channel IMU is two lines in `rig.h` and one file under
  `sense/sources/`. Nothing in `core/` changes.
* Overflowing the RAM budget is a compile error naming the budget, never a
  boot crash.
* A source that declares `blocking = 1` is routed to the sensor pass by the
  platform, so a student's blocking I2C read cannot stall the gesture path and
  they do not have to understand why.
* The cost is one layer of X-macro, which is genuinely harder to read than
  straight-line C. We accept that in `sense.h` and `chain.c` — two files the
  student never opens — to buy it back in `rig.h`, the one they do.
