#!/usr/bin/env python3

# This file was developed by Tomáš Iser & Thomas Müller <thomas94@gmx.net>.
# It is published under the BSD 3-Clause License within the LICENSE file.

"""
Example usage of tev's Python IPC implementation.
"""

import tev
import numpy as np
import time

if __name__ == "__main__":
    with tev.Ipc() as tev_ipc:
        # Create sample image in one go. The image will have RGB channels (displayed as one layer)
        # as well as a 'Bonus' channel (displayed as another layer)
        image_data = np.full((300, 200, 3), 1.0)
        image_data[40:61, :, 0] = 0.0
        image_data[:, 40:61, 1] = 0.0
        image_data[50:71, 50:71, 2] = 0.0

        bonus_data = image_data[:,:,0] + image_data[:,:,1] + image_data[:,:,2]

        tev_ipc.create_image("Test image 1", width=200, height=300, channel_names=["R", "G", "B", "Bonus"])
        tev_ipc.update_image("Test image 1", image_data, ["R", "G", "B"])
        tev_ipc.update_image("Test image 1", bonus_data, ["Bonus"])

        # Create another image that will be populated over time
        RESOLUTION = 256
        TILE_SIZE = 64
        N_TILES = (RESOLUTION // TILE_SIZE) ** 2

        tev_ipc.create_image("Test image 2", width=RESOLUTION, height=RESOLUTION, channel_names=["R", "G", "B"])

        idx = 0
        for y in range(0, RESOLUTION, TILE_SIZE):
            for x in range(0, RESOLUTION, TILE_SIZE):
                tile = np.full((TILE_SIZE, TILE_SIZE, 3), idx / N_TILES)
                tev_ipc.update_image("Test image 2", tile, ["R", "G", "B"], x, y)

                tev_ipc.update_vector_graphics("Test image 2", [
                    tev.vg_begin_path(),
                    tev.vg_rect(x, y, TILE_SIZE, TILE_SIZE),
                    #Alternatively: draw rectangle manually
                    # tev.vg_move_to(x, y),
                    # tev.vg_line_to(x, y + TILE_SIZE),
                    # tev.vg_line_to(x + TILE_SIZE, y + TILE_SIZE),
                    # tev.vg_line_to(x + TILE_SIZE, y),
                    # tev.vg_close_path(),
                    tev.vg_stroke(),
                ])

                idx += 1
                time.sleep(0.1)
