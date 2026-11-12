import sys

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib


PLAYER = "org.mpris.MediaPlayer2.Player"
DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
name = dbus.service.BusName("org.mpris.MediaPlayer2.OrbitFixture", bus=bus)


class Fixture(dbus.service.Object):
    playing = True

    @dbus.service.method("org.freedesktop.DBus.Properties", in_signature="s", out_signature="a{sv}")
    def GetAll(self, interface):
        if interface != PLAYER:
            return {}
        return {
            "Metadata": dbus.Dictionary({
                "xesam:title": "Orbit test track",
                "xesam:artist": dbus.Array(["Orbit artist"], signature="s"),
                "xesam:album": "Orbit album",
                "mpris:length": dbus.Int64(180_000_000),
                "mpris:artUrl": sys.argv[1],
            }, signature="sv"),
            "Position": dbus.Int64(27_000_000),
            "PlaybackStatus": "Playing" if self.playing else "Paused",
        }

    @dbus.service.method(PLAYER)
    def PlayPause(self):
        self.playing = not self.playing


fixture = Fixture(name, "/org/mpris/MediaPlayer2")
GLib.MainLoop().run()
