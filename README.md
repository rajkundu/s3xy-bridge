# S3XY Bridge (ESP32)

This is an ESP32 sketch that simultaneously:
1. Acts as a Commander/BLE Central for a S3XY Stalk, and
2. Virtualizes a S3XY Button thanks to [this](https://github.com/Beat-YT/s3xy-virtual-button) repo

## What This Does

My main goal was to talk to a S3XY Stalk just like a S3XY Commander does. This way, I could customize its exact behavior upon press/release/hold. Thankfully, the Stalk is set up as a straightforward BLE peripheral, so any BLE Central can talk to it easily.

For my specific use case, I also virtualize a S3XY Button for triggering CAN bus functions via S3XY Commander/app. In particular, I wanted to customize the behavior of the stalk based on how long it was pressed down, and I also wanted to assign functions upon press & release.

## Bluetooth Protocol Information

The following was derived from playing around with nRF Connect on an iPhone and LightBlue on macOS:

- Local Name: ENH_STLK_L
- Service UUID: 00003D69-87D2-479E-7E45-8551415A6DE1
- Characteristic 1 UUID (Switches/Notify): 00003D50-87D2-479E-7E45-8551415A6DE1
- Characteristic 2 UUID ("ID"): 00003D49-87D2-479E-7E45-8551415A6DE1

**Stalk End-Cap Button**
- On Press: 0xA101
- On Release: 0xA100
- Single Press: 0xA1C101
- Double Press: 0xA1C102
- Long Press: 0xA1C301

**Stalk "Up" Switch**
- On Press: 0xA301
- On Release: 0xA300
- Single Press: 0xA3C101
- Double Press: 0xA3C102
- Long Press: 0xA3C301

**Stalk "Down" Switch**
- On Press: 0xA201
- On Release: 0xA200
- Single Press: 0xA2C101
- Double Press: 0xA2C102
- Long Press: 0xA2C301

## Note
I would NOT consider this code production-quality. It is somewhat modular, but many improvements can be made to make it more abstracted & object-oriented.

This code is probably best used by having an LLM harvest the parts you want. Refer to [BeatYT's s3xy-virtual-button repo](github.com/Beat-YT/s3xy-virtual-button) for more detailed information on the S3XY Button's Bluetooth handshake, protocols, etc.
