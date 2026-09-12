"""Read host source groups from the device unity-build inventory."""
from pathlib import Path
import re
import sys
ROOT = Path(__file__).resolve().parents[1]
def sources(*groups):
    entries = re.findall(r'^#include "([a-z_]+\.c)" // (core|ui|app)$',
                         (ROOT / 'src/missile_cmd_core.c').read_text(), re.MULTILINE)
    return ['src/' + name for name, group in entries if group in groups]
if __name__ == '__main__':
    print(' '.join(sources(*sys.argv[1:])))
