# xacs-xkeys

Read an XKeys USB device and publish events to MQTT. Listen to MQTT for commands to change the backlights on XKeys device.

Requires linux, libmosquitto, libudev. Also requires a modified libuv - included as a submodule. Uses hidraw kernel interface.

## Events

### xacs/xkeys/events

The event string looks like this:
```
J:0,0,0;B:1 10;P
```

The parts are separated by ;, but there is no ; at the end of the line. The P is optional, it's presence indicates the Program button is pressed. The B is only present if buttons are pressed, then there will be a list of buttons that are currently down separated by spaces. The J is always present and indicates X,Y,Z for the joystick.

### xacs/xkeys/events/dev

This event will be `NEW` followed by string of device product at startup or when device plugged in or `GONE` when device is unplugged. The device select will be first device detected with xkeys vendor id.

## Commands

Send to ```xacs/xkeys/cmd```

|`sys [red &| green]` | Changes System LED in top left corner |
|`isys [red|green] val(off,on,flash|blink)` | Changes System LED in top left corner - isys allows blink, but dont use both sys and isys |
|`poll`| Generates an event of current status |
|`reboot`| Like unplugging and plugging in device |
|`frq num(0-255)`| LED Blink frequency |
|`int blue(0-255) red(0-255)`| LED Backlight Intensity |
|`all [red|blu] [on|off]`| Change backlight for all leds at once |
|`red btnid(0-79) val(off,on,flash|blink)`| Set individual backlight led |

