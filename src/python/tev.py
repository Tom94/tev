#!/usr/bin/env python3

# This file was developed by Tomáš Iser & Thomas Müller <thomas94@gmx.net>.
# It is published under the BSD 3-Clause License within the LICENSE file.

"""
Interfaces with tev's IPC protocol to remote control tev with Python.
"""

from enum import Enum
import socket
import struct
import numpy as np

class IpcPacketType(Enum):
    # This stays in sync with `IpcPacket::EType` from Ipc.h
    OpenImage = 7 # v2
    ReloadImage = 1
    CloseImage = 2
    CreateImage = 4
    UpdateImage = 6 # v3
    VectorGraphics = 8

class VgCommand:
    class Type(Enum):
        # This stays in sync with `VgCommand::EType` from VectorGraphics.h
        Save = 0
        Restore = 1
        MoveTo = 2
        LineTo = 3
        FillColor = 4
        StrokeColor = 5
        Fill = 6
        Stroke = 7
        BeginPath = 8

    def __init__(self, type, data = None):
        self.type = type
        self.data = data

def vg_save():
    return VgCommand(VgCommand.Type.Save)

def vg_restore():
    return VgCommand(VgCommand.Type.Restore)

def vg_move_to(x, y):
    return VgCommand(VgCommand.Type.MoveTo, (x, y))

def vg_line_to(x, y):
    return VgCommand(VgCommand.Type.LineTo, (x, y))

def vg_fill_color(r, g, b, a):
    return VgCommand(VgCommand.Type.FillColor, (r, g, b, a))

def vg_stroke_color(r, g, b, a):
    return VgCommand(VgCommand.Type.StrokeColor, (r, g, b, a))

def vg_fill():
    return VgCommand(VgCommand.Type.Fill)

def vg_stroke():
    return VgCommand(VgCommand.Type.Stroke)

def vg_begin_path():
    return VgCommand(VgCommand.Type.BeginPath)

class Ipc:
    def __init__(self, hostname = "localhost", port = 14158):
        self._hostname = hostname
        self._port = port
        self._socket = None

    def __enter__(self):
        if self._socket is not None:
            raise Exception("Communication already started")
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM) # SOCK_STREAM means a TCP socket
        self._socket.__enter__()
        self._socket.connect((self._hostname, self._port))
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if self._socket is None:
            raise Exception("Communication was not started")
        self._socket.__exit__(exc_type, exc_val, exc_tb)

    """
        Opens an image from a specified path from the disk of the machine tev is running on.
    """
    def open_image(self, path: str, channel_selector: str = "", grab_focus = True):
        if self._socket is None:
            raise Exception("Communication was not started")

        data_bytes = bytearray()
        data_bytes.extend(struct.pack("<I", 0)) # reserved for length
        data_bytes.extend(struct.pack("<b", IpcPacketType.OpenImage.value))
        data_bytes.extend(struct.pack("<b", grab_focus))
        data_bytes.extend(bytes(path, "UTF-8"))
        data_bytes.extend(struct.pack("<b", 0)) # string terminator
        data_bytes.extend(bytes(channel_selector, "UTF-8"))
        data_bytes.extend(struct.pack("<b", 0)) # string terminator

        data_bytes[0:4] = struct.pack("<I", len(data_bytes))
        self._socket.sendall(data_bytes)

    """
        Reloads the image with specified path from the disk of the machine tev is running on.
    """
    def reload_image(self, name: str, grab_focus = True):
        if self._socket is None:
            raise Exception("Communication was not started")

        data_bytes = bytearray()
        data_bytes.extend(struct.pack("<I", 0)) # reserved for length
        data_bytes.extend(struct.pack("<b", IpcPacketType.ReloadImage.value))
        data_bytes.extend(struct.pack("<b", grab_focus))
        data_bytes.extend(bytes(name, "UTF-8"))
        data_bytes.extend(struct.pack("<b", 0)) # string terminator

        data_bytes[0:4] = struct.pack("<I", len(data_bytes))
        self._socket.sendall(data_bytes)

    """
        Closes a specified image.
    """
    def close_image(self, name: str):
        if self._socket is None:
            raise Exception("Communication was not started")

        data_bytes = bytearray()
        data_bytes.extend(struct.pack("<I", 0)) # reserved for length
        data_bytes.extend(struct.pack("<b", IpcPacketType.CloseImage.value))
        data_bytes.extend(bytes(name, "UTF-8"))
        data_bytes.extend(struct.pack("<b", 0)) # string terminator

        data_bytes[0:4] = struct.pack("<I", len(data_bytes))
        self._socket.sendall(data_bytes)

    """
        Create a blank image with a specified size and a specified set of channel names.
        "R", "G", "B" [, "A"] is what should be used if an image is rendered.
    """
    def create_image(self, name: str, width: int, height: int, channel_names  = ["R", "G", "B", "A"], grab_focus = True):
        if self._socket is None:
            raise Exception("Communication was not started")

        data_bytes = bytearray()
        data_bytes.extend(struct.pack("<I", 0)) # reserved for length
        data_bytes.extend(struct.pack("<b", IpcPacketType.CreateImage.value))
        data_bytes.extend(struct.pack("<b", grab_focus))
        data_bytes.extend(bytes(name, "UTF-8"))
        data_bytes.extend(struct.pack("<b", 0)) # string terminator
        data_bytes.extend(struct.pack("<i", width))
        data_bytes.extend(struct.pack("<i", height))
        data_bytes.extend(struct.pack("<i", len(channel_names)))
        for channel_name in channel_names:
            data_bytes.extend(bytes(channel_name, "ascii"))
            data_bytes.extend(struct.pack("<b", 0)) # string terminator

        data_bytes[0:4] = struct.pack("<I", len(data_bytes))
        self._socket.sendall(data_bytes)

    """
        Updates the pixel values of a specified image region and a specified set of channels.
        The `image` parameter must be laid out in row-major format, i.e. from most to least
        significant: [row][col][channel], where the channel axis is optional.
    """
    def update_image(self, name: str, image, channel_names = ["R", "G", "B", "A"], x = 0, y = 0, grab_focus = False, perform_tiling = True):
        if self._socket is None:
            raise Exception("Communication was not started")

        # Break down image into tiles of manageable size for typical TCP packets
        tile_size = [128, 128] if perform_tiling else image.shape[0:2]

        n_channels = 1 if len(image.shape) < 3 else image.shape[2]

        channel_offsets = [i for i in range(n_channels)]
        channel_strides = [n_channels for _ in range(n_channels)]

        if len(channel_names) < n_channels:
            raise Exception("Not enough channel names provided")

        for i in range(0, image.shape[0], tile_size[0]):
            for j in range(0, image.shape[1], tile_size[1]):
                tile = image[
                    i:(min(i + tile_size[0], image.shape[0])),
                    j:(min(j + tile_size[1], image.shape[1])),
                    ...
                ]

                tile_dense = np.full_like(tile, 0.0, dtype="<f")
                tile_dense[...] = tile[...]

                data_bytes = bytearray()
                data_bytes.extend(struct.pack("<I", 0)) # reserved for length
                data_bytes.extend(struct.pack("<b", IpcPacketType.UpdateImage.value))
                data_bytes.extend(struct.pack("<b", grab_focus))
                data_bytes.extend(bytes(name, "UTF-8"))
                data_bytes.extend(struct.pack("<b", 0)) # string terminator
                data_bytes.extend(struct.pack("<i", n_channels))

                for channel_name in channel_names[0:n_channels]:
                    data_bytes.extend(bytes(channel_name, "UTF-8"))
                    data_bytes.extend(struct.pack("<b", 0)) # string terminator

                data_bytes.extend(struct.pack("<i", x+j)) # x
                data_bytes.extend(struct.pack("<i", y+i)) # y
                data_bytes.extend(struct.pack("<i", tile_dense.shape[1])) # width
                data_bytes.extend(struct.pack("<i", tile_dense.shape[0])) # height

                for channel_offset in channel_offsets:
                    data_bytes.extend(struct.pack("<q", channel_offset))
                for channel_stride in channel_strides:
                    data_bytes.extend(struct.pack("<q", channel_stride))

                data_bytes.extend(tile_dense.tobytes()) # data

                data_bytes[0:4] = struct.pack("<I", len(data_bytes))
                self._socket.sendall(data_bytes)

    """
        Updates vector graphics
        TODO: flesh out details
    """
    def update_vector_graphics(self, name: str, commands, append = False):
        if self._socket is None:
            raise Exception("Communication was not started")

        data_bytes = bytearray()
        data_bytes.extend(struct.pack("<I", 0)) # reserved for length
        data_bytes.extend(struct.pack("<b", IpcPacketType.VectorGraphics.value))
        data_bytes.extend(bytes(name, "UTF-8"))
        data_bytes.extend(struct.pack("<b", 0)) # string terminator
        data_bytes.extend(struct.pack("<b", append))
        data_bytes.extend(struct.pack("<I", len(commands)))
        for command in commands:
            data_bytes.extend(struct.pack("<b", command.type.value))
            if command.data is not None:
                data_bytes.extend(np.array(command.data, dtype="<f").tobytes())

        data_bytes[0:4] = struct.pack("<I", len(data_bytes))
        self._socket.sendall(data_bytes)
        pass

# `Ipc` used to be called `TevIpc`, so we keep the alias for backwards compatibility.
TevIpc = Ipc
