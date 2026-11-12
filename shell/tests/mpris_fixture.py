import pathlib
import struct
import sys
import tempfile
import zlib

import dbus
import dbus.service
from dbus.mainloop.glib import DBusGMainLoop
from gi.repository import GLib


PLAYER = "org.mpris.MediaPlayer2.Player"
DESKTOP = "org.freedesktop.portal.Desktop"
DBusGMainLoop(set_as_default=True)
bus = dbus.SessionBus()
player_name = dbus.service.BusName("org.mpris.MediaPlayer2.OrbitFixture", bus=bus)
portal_name = dbus.service.BusName(DESKTOP, bus=bus)


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


class Request(dbus.service.Object):
    @dbus.service.signal("org.freedesktop.portal.Request", signature="ua{sv}")
    def Response(self, code, result):
        pass

    def send(self):
        self.Response(0, {"uri": picture.as_uri()})
        self.remove_from_connection()
        return GLib.SOURCE_REMOVE


class Portal(dbus.service.Object):
    @dbus.service.method("org.freedesktop.portal.Screenshot", in_signature="sa{sv}",
                         out_signature="o", sender_keyword="sender")
    def Screenshot(self, parent, options, sender=None):
        request_path = "/org/freedesktop/portal/desktop/request/{}/{}".format(
            sender.lstrip(":").replace(".", "_"), options["handle_token"]
        )
        request = Request(portal_name, request_path)
        GLib.idle_add(request.send)
        return dbus.ObjectPath(request_path)


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


with tempfile.TemporaryDirectory(prefix="orbit-portal-fixture-") as directory:
    picture = pathlib.Path(directory) / "screen.png"
    picture.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 96, 64, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress((b"\0" + b"\x78\x56\xaa" * 96) * 64))
        + chunk(b"IEND", b"")
    )
    fixture = Fixture(player_name, "/org/mpris/MediaPlayer2")
    portal = Portal(portal_name, "/org/freedesktop/portal/desktop")
    GLib.MainLoop().run()
