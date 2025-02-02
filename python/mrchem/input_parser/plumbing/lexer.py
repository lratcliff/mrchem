# -*- coding: utf-8 -*-

# This file was automatically generated by parselglossy on 2022-10-03
# Editing is *STRONGLY DISCOURAGED*

import json
from pathlib import Path
from typing import Optional, Union

from .utils import ComplexEncoder, JSONDict, path_resolver


def lex_from_str(*,
                 in_str: Union[str, Path],
                 ir_file: Optional[Union[str, Path]] = None
) -> JSONDict:
    """Run grammar on input string.

    Parameters
    ----------
    in_str : Union[str, Path]
         The string to be parsed.
    ir_file : Optional[Union[str, Path]]
         File to write intermediate representation to (JSON format).
         None by default, which means file is not written out.

    Returns
    -------
    The contents of the input string as a dictionary.
    """

    from . import getkw
    lexer = getkw.grammar(has_complex=True)
    ir = getkw.parse_string_to_dict(lexer, in_str)

    if ir_file is not None:
        ir_file = path_resolver(ir_file)
        with ir_file.open("w") as out:
            json.dump(ir, out, cls=ComplexEncoder, indent=4)

    return ir
