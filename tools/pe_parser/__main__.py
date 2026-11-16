"""python3 -m tools.pe_parser IMAGE.exe [--json FILE]"""
import sys

from .pe_parser import main

sys.exit(main())
