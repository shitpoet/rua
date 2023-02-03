# rua - Real-time UDP audio

This program allows one to use an Android mobile phone as a low-latency headset for a Linux computer.

The program consists of two parts:

1. A mobile app (Android 8+ application)
2. A desktop daemon (C application using SDL)

These two programs create bi-directional audio stream between a phone and a PC:

* The mobile app captures mic and sends its stream to the PC and pla
* Also it plays audio stream going from the PC.
* Desktop app plays mic stream from the phone.
* And it streams everything from PC to a phone.

Everything is implemented in the most basic way. No UI, no settings. Unfortunately there are also some hard-coded constants that can make this project perform badly on setups different from mine.

## Hard-coded constants

Both apps use 48000 Hz 16-bit mono channels now. This can be suboptimal if desktop or mobile hardware does not support native 48kHz sample rate.

Desktop app records 256 samples at a time. This can be too high of a rate in some cases. But should be ok most of the time.

Both apps try to avoid buffering using different hard-coded conditions. This can result in drops on unstable or slow connection.

In some cases the mobile app can be unable to correcty detect broadcast address. In this case it uses `192.168.43.255`, which should work in hot spot (and I believe in usb tethering configurations), but some device can have different IP hard-coded in it or even use random network every time. In this case the apps will be unable to establish a connection between them. And there is no ability manualy enter peer's IP.


## Need for a good network between devices

Because this projects aims for low-latency communication it sacrifies quality: there can be some drops from time to time, especially if your setup if very different from mine.

I mean the project has to auto-adaptiation capabilities it just assumes that your connection is fast and more or less reliable.

Because of that it is best to connect the phone over USB and enable USB tethering - it is (usually?) the fastest and the most reliable communication path. Another good variant is to activate hot stop (access point) on the phone and connect the PC to the phone directly, not through some wi-fi router. It can be much better or not so much better or it can be ever worse in your situation. I suggest to use separate wi-fi module on the computer used exclusively for audio communication with phone. Also it is recommended to insert this module if it is a USB one to different hardware hub than other active USB devices. Another recommendation may be to connect the PC to a wi-fi router through Ethernet cable.

Summing up: it is strongly recommended to provide good and more or less reliable connection between the phone and PC, otherwise assumtions of the programs will be false and they will behave badly.

The apps can survive infrequent packet drops and some minimal latencies. But they can not recover jittered packets at the time. Or packets arriving on very unevenly distributed time moments.

The apps require a bandwidth of around several megabits to be available 99.9% of time.

Other ideas to make the connection better: try to use real-time kernel, try to increase priority of the desktop process. (I didn't noticed any differece in my case with these tweaks.) Also there are some tweaks you can apply to your desktop audio subsystem, f.ex. pulseaudio allows to adjust buffers it uses. On smartphone it is recommended to disable any battery optimization for the app.


## Setup on desktop

You need to somehow route audio into and from the desktop.

For pulseaudio one can add the following to the `default.pa`:

    load-module module-null-sink sink_name=app_sink sink_properties=device.description="app_sink" rate=48000 channels=1
    load-module module-null-sink sink_name=mic_sink sink_properties=device.description=mic_sink

    set-default-source mic_sink.monitor

(The last line is optional.)

Now you can switch output of interested apps to `app_sink` and associated their input with `mic_sink`.

The `rua` process should be associated with the same sinks but in reversed order: output to `mic_sink`, input from `app_sink`.

Having correct sample rates and channel configuration here can help to avoid resampling and rebuffering somewhere. But in my experiments it does not influence anything a lot.


## Some implementation details

Data is sent using UDP protocol so there can be some lost packets. The program does not do anything to fill the gaps in case of lost packets.

UDP is used to achieve the minimum possible latency.

Audio is streamed in raw format without compression, so a good network connection is required to use the apps. I do not implement comopression of any kind because it is just simpler, but it can help with latency if the network connection is perfect. Also it can decrease CPU usage both on the mobile side and on the PC side.

There is peer discovery so the apps should work ok with dynamic IP addresses. Also it always tries to reconnect.

Note that the apps currencly do not have idle mode - they will use a lot of CPU even if no connection is present.
