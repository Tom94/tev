#!/usr/bin/env python3

# This file was developed by Tomáš Iser & Thomas Müller <thomas94@gmx.net>.
# It is published under the BSD 3-Clause License within the LICENSE file.

"""
Interfaces with tev's IPC protocol to remote control tev with Python.
"""

import socket
import struct
import numpy as np

class TevIpc:
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
        data_bytes.extend(struct.pack("<b", 7)) # open image (v2)
        data_bytes.extend(struct.pack("<b", grab_focus)) # grab focus
        data_bytes.extend(bytes(path, "UTF-8")) # image path
        data_bytes.extend(struct.pack("<b", 0)) # string terminator
        data_bytes.extend(bytes(channel_selector, "UTF-8")) # channels to load
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
        data_bytes.extend(struct.pack("<b", 1)) # reload image
        data_bytes.extend(struct.pack("<b", grab_focus)) # grab focus
        data_bytes.extend(bytes(name, "UTF-8")) # image path
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
        data_bytes.extend(struct.pack("<b", 2)) # close image
        data_bytes.extend(bytes(name, "UTF-8")) # image name
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
        data_bytes.extend(struct.pack("<b", 4)) # create image
        data_bytes.extend(struct.pack("<b", grab_focus)) # grab focus
        data_bytes.extend(bytes(name, "UTF-8")) # image name
        data_bytes.extend(struct.pack("<b", 0)) # string terminator
        data_bytes.extend(struct.pack("<i", width)) # width
        data_bytes.extend(struct.pack("<i", height)) # height
        data_bytes.extend(struct.pack("<i", len(channel_names))) # number of channels
        for cname in channel_names:
            data_bytes.extend(bytes(cname, "ascii")) # channel name
            data_bytes.extend(struct.pack("<b", 0)) # string terminator
        data_bytes[0:4] = struct.pack("<I", len(data_bytes))

        self._socket.sendall(data_bytes)

    """
        Updates the pixel values of a specified image region and a specified set of channels.
        The `image` parameter must be laid out in row-major format, i.e. from most to least
        significant: [col][row][channel], where the channel axis is optional.
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
                    i:(min(i+tile_size[0], image.shape[0])),
                    j:(min(j+tile_size[1], image.shape[1])),
                    ...
                ]

                tile_dense = np.full_like(tile, 0.0, dtype="<f")
                tile_dense[...] = tile[...]

                data_bytes = bytearray()
                data_bytes.extend(struct.pack("<I", 0)) # reserved for length
                data_bytes.extend(struct.pack("<b", 6)) # update image (v3)
                data_bytes.extend(struct.pack("<b", grab_focus)) # grab focus
                data_bytes.extend(bytes(name, "UTF-8")) # image name
                data_bytes.extend(struct.pack("<b", 0)) # string terminator
                data_bytes.extend(struct.pack("<i", n_channels)) # number of channels

                for channel_name in channel_names[0:n_channels]:
                    data_bytes.extend(bytes(channel_name, "UTF-8")) # channel name
                    data_bytes.extend(struct.pack("<b", 0)) # string terminator
                
                data_bytes.extend(struct.pack("<i", x+j)) # x
                data_bytes.extend(struct.pack("<i", y+i)) # y
                data_bytes.extend(struct.pack("<i", tile_dense.shape[1])) # width
                data_bytes.extend(struct.pack("<i", tile_dense.shape[0])) # height

                for channel_offset in channel_offsets:
                    data_bytes.extend(struct.pack("<q", channel_offset)) # channel_offset
                for channel_stride in channel_strides:
                    data_bytes.extend(struct.pack("<q", channel_stride)) # channel_stride

                data_bytes.extend(tile_dense.tobytes()) # data
                data_bytes[0:4] = struct.pack("<I", len(data_bytes))

                self._socket.sendall(data_bytes)