# xacs-xkeys

Read an XKeys USB device and publish events to MQTT. Listen to MQTT for commands to change the backlights on XKeys device.

Requires linux, libmosquitto, libudev. Also requires a modified libuv - included as a submodule. Uses hidraw kernel interface.
