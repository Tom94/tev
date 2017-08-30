#!/usr/bin/env python3

"""
Generates a list of color values which are dense samples of the
colormap given as argument. The list is 1-dimensional as contains
4 floats in sequence per-sample (R,G,B,A). It can be used in C++
code to make colormap textures for use by the false-color shader.
"""

import sys
import argparse
import matplotlib.pyplot as plt
import numpy as np

def main(arguments):
    """Main function of this program."""

    parser = argparse.ArgumentParser(description="Samples densely from a colormap.")
    parser.add_argument("name", type=str, help="The name of the colormap to sample values from.")

    args = parser.parse_args(arguments)

    cmap = plt.get_cmap(args.name)
    xs = np.linspace(0, 1, 256)
    samples = cmap(xs)

    samples = ",\n".join([", ".join([str(y) + "f" for y in x]) for x in samples]) + ","
    print(samples)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
