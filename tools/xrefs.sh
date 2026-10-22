#!/bin/sh
# usage: tools/xrefs.sh ADDR...   (Ghidra addresses, hex without 0x)
R=$(realpath "$(dirname "$0")/../re")
/opt/ghidra/support/analyzeHeadless $R/ghidra_proj lh -process driver_lighthouse.so -noanalysis -readOnly -scriptPath $R/ghidra_scripts -postScript XRefs.java "$@" 2>&1 | grep -o 'XREF.*' | sed 's/ (GhidraScript)//'
