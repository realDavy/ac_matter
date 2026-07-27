# Matter-Based AC Controller for Any Air Conditioner Brand

Matter is a new Smart Home/IoT protocol now built into both iOS and Android devices. This means Matter-compliant devices can be operated directly within environments like "Apple Home" without installing a separate app.

This device is an offline air conditioner controller. The AC control functionality itself does not require an internet connection, so it does not rely on any third-party cloud services or account registrations.

If you are using Home Assistant, the sister project—the ESPHome version of this project (https://github.com/timj-code/bc7215_ac_esphome)—might be a better fit for you. Matter's support for air conditioners is still in its early stages and only supports limited features, making it less comprehensive than Home Assistant.

Based on my development experience, using the Matter protocol on devices that require real-time feedback (like air conditioners) isn't smooth enough yet. Feedback on the phone can sometimes have a noticeable delay, and establishing a connection takes quite a while. (Though this might also be an issue with my code; I would really appreciate it if anyone could help improve this project, especially the user experience aspect). **Overall, however, it's still very cool for controlling non-smart home ACs or giving as a gift to friends who use iPhones, since it works with any AC brand or model, requires no extra apps, and needs no additional hub for iPhone users.**

## Limitations

Not requiring a dedicated app is a huge plus for Matter devices, but it comes with a downside: the device manufacturer has almost no control over the user experience. For instance, the UI and user experience can vary significantly between Apple and Android platforms, especially for complex devices like air conditioners.

Furthermore, the Matter protocol is still evolving and currently offers limited support for AC units. It mainly maps HVAC system modes; many features of split-system air conditioners cannot find a mapping in the protocol, or even if defined in the specification, are not yet supported by mobile operating systems.

Here are the limitations I have observed so far:

### Protocol Limitations

| Item | Standard Split AC | Matter Protocol |
| :--- | :--- | :--- |
| **Temperature Control** | Single target temperature | Divided into Cooling and Heating target temperatures; specifically in "Auto" mode, it sets a temperature range rather than a single value. |
| **Temperature Resolution** | Typically 1°C; the AC control library used in this device also only supports integer temperatures. | The protocol does not allow the device to control temperature resolution. Currently, both Apple and Google use 0.5°C as the minimum step. |
| **Fan Speed Control** | Typically Auto, High, Medium, and Low (4 levels). | Fan control has an independent power toggle. The protocol supports both discrete speed levels and percentage-based control; currently, phone manufacturers only seem to support percentage-based fan adjustments. |

**Project Approach:** This project only implements the most common Cooling and Heating modes; other modes are currently unsupported. Fan speed mapping: 1%–33% maps to Low, 34%–66% maps to Medium, and above 66% maps to High. When set to a 0.5°C increment, the device automatically rounds up to the nearest integer.

### Ecosystem Limitations

| Apple Home | Google Home | Amazon Alexa |
| :--- | :--- | :--- |
| An iPhone itself functions as a Matter controller; no extra device is needed. | Requires a Google Nest device at home as a hub/controller. | Requires an Amazon Echo device at home as a hub/controller. |
| Supports setting temperature and fan speed (percentage). | Supports setting temperature and fan speed (percentage). | Only supports ON/OFF toggles for ACs; setting temperature is not supported. |
| If only using the iPhone as a controller, it works only when connected to the home Wi-Fi. | | |

## Firmware Installation

(This section is TBD)

## Setup and Usage

### Setup

Setting up this device involves 2 steps:
- Pairing with the air conditioner
- Connecting to your phone

The order does not strictly matter, but it is recommended to pair the AC first so it is ready to use immediately after connecting to your phone.

There are two holes on the back cover of the device: one is a button, and the other is an LED indicator.

1. **AC Pairing:** Upon initial power-on, the LED should stay solid ON, indicating it has not been configured. First, set your AC's original IR remote to **25°C / Cooling mode**. Press the button on the device once, and the LED will start blinking rapidly, entering pairing mode. Point the remote at the device and press the "Fan Speed" button. Under normal conditions, pairing will complete, and the LED will switch to blinking twice per second, indicating it is waiting for phone pairing.

2. **Connecting to Phone:** Open the Apple Home or Google Home app. Choose "Add Accessory" in Apple Home or "Add Device" in Google Home. Scan the QR code on the device when the scanning screen appears, and follow the on-screen prompts to complete the setup. During the connection process (including reconnecting after a power cycle), the LED will flash slowly to indicate it is connecting.

**The onboarding process for Matter devices is generally slow and often takes several minutes. Sometimes, even after the device finishes connecting, it may take another 1 to 2 minutes for the phone to show its online status. This appears to be an inherent characteristic of the Matter protocol, and there seems to be little a device developer can do about it.**

*Note: You will encounter an "**Uncertified Accessory**" warning during phone configuration. This occurs because commercial manufacturers must undergo official certification to receive a unique Device Attestation Certificate (DAC). As a DIY device, it cannot acquire official certification; however, this warning does not affect functionality.*

Once connected successfully, the LED will flash once every 3 seconds, indicating the device is ready.

After adding the device on your phone, you should see the thermostat and fan control interfaces. Due to the limitations mentioned earlier, only Cooling and Heating modes are currently supported. For temperature, setting a 0.5°C value will automatically round up since the device hardware only supports integer steps. Similarly, if fan speed displays as a continuous percentage slider, it will snap to the nearest equivalent speed (Low, Medium, or High).

Besides controlling the AC from your phone, this device supports syncing operations from your IR remote back to your phone. You can see physical IR remote adjustments reflected in the phone app. Therefore, the device should be placed near the AC so it can receive IR signals whenever you use the original remote; otherwise, status sync will fail.

### Adding Additional Controllers

Matter devices can be linked to multiple controllers simultaneously (e.g., controlling via both Apple Home and Google Home). To add another controller, select "Add Controller" (shown as "Turn on Pairing Mode" in Apple Home) on the already connected phone. This generates a numeric setup code. You can then add the device on a second platform using this code. Based on my experience, scanning the QR code for a secondary commissioner usually fails—you must manually select the option to use the setup code/numeric key.

### Reset

You can re-pair a new AC at any time without resetting the phone connection. AC pairing is simple and virtually instantaneous.

If you want to start completely from scratch, press and hold the button on the device for 5 seconds until the LED starts blinking rapidly, then release it. This will erase all network settings and AC pairing data.

## LED Status Indicator

| LED Pattern | Meaning |
| :--- | :--- |
| **Solid ON** | Factory default state; not configured yet |
| **Rapid Blinking** | Waiting to receive IR signal for AC pairing |
| **Slow Blinking** | Connecting to the network |
| **1 Short Blink per Second** | Network configured, but AC not yet paired |
| **2 Short Blinks per Second** | AC paired, waiting for phone connection |
| **1 Short Blink every 3 Seconds** | Fully paired and connected; standby state |
