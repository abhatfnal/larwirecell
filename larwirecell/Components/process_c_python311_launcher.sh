#!/bin/sh
# Enter the Sophia Python runtime from the SL7 Process-B container without
# replacing Process B's archived WCT runtime.  The outer container must expose
# the host's read-only lib64 directory at PROCESS_C_HOST_LIB64.
set -eu

if [ "$#" -lt 1 ]; then
  echo "Process-C launcher requires the Python executable" >&2
  exit 64
fi

python=$1
shift
case "${python}" in
  */bin/python) python_prefix=${python%/bin/python} ;;
  *)
    echo "Process-C launcher requires an absolute */bin/python path" >&2
    exit 64
    ;;
esac

host_lib64=${PROCESS_C_HOST_LIB64:?PROCESS_C_HOST_LIB64 is required}
host_loader=${host_lib64}/ld-linux-x86-64.so.2
exec "${host_loader}" \
  --library-path "${python_prefix}/lib:${host_lib64}" \
  "${python}" "$@"
