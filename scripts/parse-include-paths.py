#!/usr/bin/env python3

"""
Parses .vscode/.cmaketools.json to obtain a list of include paths.
These can then be subsequently pasted into .vscode/c_cpp_properties.json
to make intellisense work. This is script exists purely for convenience
and only needs to be used when the include paths change (e.g. when a new
dependency is added).
"""

import json
import os
import sys

def iterate_over(dict_or_list, result):
    """
    Iterates recursively over nested lists and dictionaries
    keeping track of all "path" values with the key "includePath"
    within nested dictionaries.
    """
    if isinstance(dict_or_list, list):
        for child in dict_or_list:
            iterate_over(child, result)
    elif isinstance(dict_or_list, dict):
        for key, value in dict_or_list.items():
            if key == "includePath":
                for child in value:
                    result.add(child["path"])
            else:
                iterate_over(value, result)

def main(arguments):
    """Main function of this program."""

    workspace = os.path.realpath(os.path.join(__file__, os.pardir, os.pardir))
    print("Workspace root: '{}'".format(workspace))

    with open(os.path.join(workspace, ".vscode", ".cmaketools.json")) as f:
        data = json.loads(f.read())

    result = set()

    iterate_over(data, result)

    result = [x.replace(workspace, "${workspaceRoot}") for x in result]

    print(json.dumps(result, indent=0))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
