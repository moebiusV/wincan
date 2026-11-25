#! /bin/sh
# no-private.sh -- fails if the source tree names a local user, a host, an absolute local path or a
# private project: documents, comments and examples use generic values ($HOME, /var/lib/candled,
# an example host).  Run by `make check`; it can be run from anywhere.
cd "$(dirname "$0")/.." || exit 2          # the top of the source tree, wherever it is run from
# the patterns are written in pieces so that this file does not match itself
pat="/ho""me/|/m""nt/|/U""sers/|[a-z]:\\\\Users\\\\|192\\.168\\.|10\\.[0-9]+\\.[0-9]+\\.[0-9]+|m""amba|crypto-""scanner|liq""map|crypto""\\.db|silent""pc|matt""hew"
bad=$(grep -rIn -E "$pat" . \
        --exclude-dir=.git --exclude-dir=autom4te.cache --exclude-dir=.deps \
        --exclude=no-private.sh --exclude=COPYING --exclude='config.*' --exclude='*.o' 2>/dev/null \
      | grep -v -E '^\./(Makefile|Makefile\.in|configure|aclocal\.m4|libtool|stamp-h1)[:]' \
      | grep -v -E 'example|EXAMPLE|C:\\Users\\alice|/Users/alice')
if [ -n "$bad" ]; then
    echo "no-private: the tree names something private or machine-specific:" >&2
    echo "$bad" | cut -c1-200 >&2
    exit 1
fi
echo "no-private: ok"
