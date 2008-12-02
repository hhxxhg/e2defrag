set -e
before="$1"
tst="`echo \"$before\"|cut -d, -f1`"
if [ x"$tst" != x"before" ]; then
  echo "Not designed for interactive use; exiting." >&2
  exit 1
fi
after="`echo \"$before\" | sed s/before/after/`"
