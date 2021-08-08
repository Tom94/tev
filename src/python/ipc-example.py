#!/usr/bin/env python3

# This file was developed by Tomáš Iser & Thomas Müller <thomas94@gmx.net>.
# It is published under the BSD 3-Clause License within the LICENSE file.

"""
Example usage of tev's Python IPC implementation.
"""

from ipc import TevIpc
import numpy as np

if __name__ == "__main__":
    with TevIpc() as tev:
        image_data = np.full((300,200,3), 1.0)
        image_data[40:61,:,0] = 0.0
        image_data[:,40:61,1] = 0.0
        image_data[50:71,50:71,2] = 0.0

        bonus_data = image_data[:,:,0] + image_data[:,:,1] + image_data[:,:,2]

        tev.create_image("Test Image", width=200, height=300, channel_names=["R","G","B","Bonus"])
        tev.update_image("Test Image", image_data, ["R", "G", "B"])
        tev.update_image("Test Image", bonus_data, ["Bonus"])
